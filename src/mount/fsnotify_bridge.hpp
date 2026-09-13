#pragma once

#include "agent/client.hpp"
#include "core/error.hpp"
#include "core/types.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <thread>

namespace wsld::mount {

/// Makes far-side changes visible to ordinary Linux file watchers.
///
/// # The problem
///
/// A change made on the Windows side of the boundary reaches this mount as an
/// invalidation, so the metadata mirror and the page cache are correct within
/// milliseconds. What does *not* happen is an `inotify` event, and that is what
/// every watch-mode tool in the Linux ecosystem is waiting for. The tool does
/// not error; it simply never fires. That is the whole of microsoft/WSL#4739.
///
/// # Why it cannot be delivered directly
///
/// The kernel raises fsnotify events from the VFS, at the point an operation is
/// performed — `vfs_create` calls `fsnotify_create`, `vfs_unlink` calls
/// `fsnotify_unlink`, and so on. A filesystem cannot raise one on its own, and
/// FUSE offers no notification that does. The three `fuse_lowlevel_notify_*`
/// calls invalidate dentries and pages, which is a cache-coherence mechanism,
/// not a notification one: `fuse_reverse_inval_entry` never touches fsnotify.
/// A userspace daemon has no way to hand an inotify watcher an event.
///
/// # What this does instead
///
/// It asks the kernel to raise the event, by performing the operation the far
/// side already performed — on this mount, from a thread of this process. A
/// file created over there becomes a `mknodat` here, a deletion an `unlinkat`,
/// a rename a `renameat`, a write a `utimensat` that touches only mtime (which
/// `fsnotify_change` reports as FS_MODIFY, not FS_ATTRIB). The kernel runs its
/// own hooks and every watcher gets a genuine event of the right type, with a
/// genuine rename cookie pairing the two halves of a move. Consumers need no
/// cooperation, no preload, and no knowledge that wsldrive exists.
///
/// The operations must not cross the boundary a second time — the far side
/// already has this state, and re-applying it would at best be wasted work and
/// at worst destroy the file that prompted the event. So the FUSE handlers
/// recognise these requests and answer them locally without forwarding. That
/// recognition is the delicate part, and it is deliberately over-determined:
/// see `claim()`.
///
/// Only the mount's own view is touched. Nothing is written to the served tree.
///
/// # Cost
///
/// One syscall per changed path, served by the FUSE loop from the in-RAM
/// mirror. No boundary crossing, no I/O.
///
/// Linux only. `supported()` is false elsewhere and `start()` fails with
/// Unsupported; Direction A has no equivalent problem, because a WinFsp volume
/// raises Windows change notifications through its own mechanism.
class FsNotifyBridge {
 public:
  /// The operations the bridge performs on itself. A FUSE handler names the one
  /// it implements when asking whether the request in front of it is a poke.
  enum class Poke : std::uint8_t {
    None,
    Mknod,   // a file appeared
    Mkdir,   // a directory appeared
    Unlink,  // a file went away
    Rmdir,   // a directory went away
    Rename,  // both halves of a move
  };

  /// What a handler should pretend about a path, so that the kernel will run
  /// the operation that raises the event. Both are visible only to the bridge's
  /// own thread: any other caller gets the mirror's real answer.
  enum class Pretend : std::uint8_t {
    Nothing,
    Absent,   // the mirror already has the new path; hide it so ->create runs
    Present,  // the mirror already dropped the old path; ghost it so ->unlink runs
  };

  struct Stats {
    std::uint64_t events = 0;    // changes taken off the queue
    std::uint64_t delivered = 0; // pokes the kernel accepted (an event went out)
    std::uint64_t failed = 0;    // pokes that returned an error
    std::uint64_t dropped = 0;   // changes discarded because the queue was full
    std::uint64_t rescans = 0;   // mirror replacements, which name no path
  };

  FsNotifyBridge() = default;
  ~FsNotifyBridge();
  FsNotifyBridge(const FsNotifyBridge&) = delete;
  FsNotifyBridge& operator=(const FsNotifyBridge&) = delete;

  /// Drops the kernel's cached dentry and pages for one mount-relative path.
  /// The rename poke needs it; see the comment in poke_move().
  using InvalidatePath = std::function<void(const std::string&)>;
  void set_invalidate(InvalidatePath fn) { invalidate_ = std::move(fn); }

  [[nodiscard]] static bool supported() noexcept;

  /// Starts the poke thread against an already-mounted `mount_root`. Fails with
  /// Unsupported off Linux, and with InvalidArgument if `mount_root` is not a
  /// FUSE mount — the pokes are real filesystem operations, so pointing them at
  /// anything else would modify the wrong tree.
  [[nodiscard]] Result<void> start(const std::string& mount_root);

  /// Stops delivery and joins. Must run before the mount goes away. Idempotent.
  void stop();

  [[nodiscard]] bool running() const noexcept { return running_.load(std::memory_order_relaxed); }

  /// Queues changes for delivery. Called from the client's reader thread; never
  /// blocks, and drops rather than growing without bound.
  void post(std::span<const agent::RemoteRoot::AppliedChange> changes);

  [[nodiscard]] Stats stats() const;

  // --- consulted by the FUSE handlers ---------------------------------------

  /// Whether `caller` should be told something other than the truth about
  /// `rel`, so the kernel proceeds into the operation that raises the event.
  /// `kind` receives the type to report for Pretend::Present.
  [[nodiscard]] Pretend pretend(int caller, std::string_view rel, NodeKind& kind) const noexcept;

  /// Whether the request in front of a handler is this bridge's own poke, and
  /// so must be answered locally instead of forwarded across the boundary.
  ///
  /// Three independent things must hold, because the cost of a false positive
  /// is a silently swallowed user operation — a delete that does not delete:
  ///
  ///   1. the caller is the bridge's own poke thread (its tid, or this
  ///      process's pid; no other thread here ever touches the mount),
  ///   2. a poke of exactly this kind is in flight, and
  ///   3. it names exactly this path (both paths, for a rename).
  ///
  /// A poke is claimed at most once: the record is retired by this call, so a
  /// retry or a duplicate request is forwarded like any other.
  [[nodiscard]] bool claim(int caller, Poke what, std::string_view rel, std::string_view rel2 = {});

  /// Whether `caller` is the bridge's poke thread at all, matching poke or not.
  ///
  /// This is what makes an unrecognised poke safe. The bridge never performs a
  /// genuine mutation, so a mutation request arriving from its thread is one of
  /// its own replays and nothing else. If `claim()` did not recognise it — a
  /// bug, a path spelled differently than expected, a syscall the kernel turned
  /// into a different operation — the only safe answer is to refuse it. The
  /// alternative is forwarding it, and forwarding a create means asking the
  /// agent to replace the very file whose arrival prompted the notification
  /// with an empty one.
  [[nodiscard]] bool is_poke_thread(int caller) const noexcept;

 private:
  struct Job {
    ChangeKind change = ChangeKind::Unknown;
    std::string path;
    std::string path2;  // move destination
    NodeKind kind = NodeKind::File;
    bool rescan = false;
  };

  // The single poke in flight. One at a time, so the handlers' test is an exact
  // match rather than a search, and a stale record cannot outlive its syscall.
  struct Active {
    Poke what = Poke::None;
    std::string path;
    std::string path2;
    Pretend pretend_path = Pretend::Nothing;
    NodeKind kind = NodeKind::File;
  };

  void run();
  void deliver(const Job& job);
  bool poke_create(const Job& job);
  bool poke_modify(const Job& job);
  bool poke_remove(const Job& job);
  bool poke_move(const Job& job);
  // True if `rel`'s parent directory is inside the mount. The pokes are real
  // filesystem calls; this is what keeps one that resolves outside the mount
  // (a path that escapes it, a mount that went away under us) from being
  // applied to whatever is there instead.
  [[nodiscard]] bool inside_mount(std::string_view rel) const;
  [[nodiscard]] std::string abs(std::string_view rel) const;
  [[nodiscard]] static long this_tid_or_zero() noexcept;
  // Confirms `root_` really is the FUSE mount the pokes are meant for, and
  // records its device. Runs on the poke thread; see the comment at its call.
  [[nodiscard]] bool verify_root();
  void set_active(Active a);
  void clear_active();
  [[nodiscard]] bool is_self(int caller) const noexcept;

  std::string root_;
  InvalidatePath invalidate_;  // set once, before start()
  std::uint64_t root_dev_ = 0;  // st_dev of the mount root; pokes never leave it
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_{false};
  std::atomic<long> poke_tid_{0};
  std::atomic<long> self_pid_{0};

  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Job> queue_;

  mutable std::mutex active_mu_;
  Active active_;

  mutable std::mutex stats_mu_;
  Stats stats_;

  // Past this many queued changes the oldest are dropped. A watcher that misses
  // one gets a late notification, not a wrong one: the mirror and the page
  // cache are already correct, so the file reads right the moment anything
  // looks at it. Sized for a burst the size of an `npm install`.
  static constexpr std::size_t kQueueCap = 65536;

  // How many failed pokes are named individually before the log goes quiet.
  static constexpr std::uint64_t kMaxReportedFailures = 8;
};

}  // namespace wsld::mount
