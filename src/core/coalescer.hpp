#pragma once

#include "core/hash_util.hpp"
#include "core/types.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace wsld {

/// Raw filesystem event as reported by a platform watcher, already normalised
/// to a '/'-separated path relative to the watched root.
enum class FsEventKind : std::uint8_t {
  Created,
  Modified,
  Removed,
  RenamedFrom,  // old name of a rename
  RenamedTo,    // new name of a rename
  Overflow,     // the watcher's buffer overflowed; events were lost
};

struct FsEvent {
  FsEventKind kind;
  std::string_view path;
  // Ties the two halves of a rename together. inotify supplies one per move
  // pair; the Windows watcher mints one for each adjacent OLD_NAME/NEW_NAME
  // pair, since ReadDirectoryChangesW has no equivalent. Zero means "not part
  // of a pair we could identify", which a consumer must degrade gracefully on:
  // an unpaired RenamedFrom is a removal, an unpaired RenamedTo a creation.
  std::uint32_t cookie = 0;
};

/// A planned invalidation. Attributes are not resolved here: the watcher side
/// stats the path when the batch is sent, so a burst of writes costs one stat.
struct PlannedOp {
  InvalidationKind kind;
  std::string path;
  // The entry is new at this path (a Created or RenamedTo was seen, and no
  // removal since). A directory that appears by rename brings a whole subtree
  // the watcher never reports, so the sender enumerates it; a Modified on a
  // directory the peers already know must not trigger that scan.
  bool appeared = false;
  // What the watcher actually saw, kept alongside `kind` for the change
  // notification path. Collapsing preserves the shape of the burst rather than
  // its last event: a create followed by three writes is still a creation.
  ChangeKind change = ChangeKind::Unknown;
  std::uint32_t cookie = 0;  // meaningful only for the two move kinds

  friend bool operator==(const PlannedOp& a, const PlannedOp& b) noexcept {
    return a.kind == b.kind && a.path == b.path;  // the rest is advisory
  }
};

/// Degrades every move half in `ops` that has no partner into the plain change
/// it amounts to on its own: a source with no destination is a removal, a
/// destination with no source is a creation.
///
/// Halves get separated twice on the way out: the coalescer can collapse one of
/// them away, and the sender re-stats every path afterwards and may reclassify
/// one of them. Running this after each step keeps one invariant true for
/// consumers — a move that reaches them is always matched — so none of them has
/// to carry its own half-move fallback.
///
/// Works on anything with `change` and `cookie` members (PlannedOp on the way
/// out of the coalescer, proto::InvalidationOp on the way onto the wire).
template <class Op>
void repair_move_pairs(std::vector<Op>& ops) {
  std::unordered_map<std::uint32_t, int> halves;  // cookie -> bitmask of halves seen
  for (const Op& op : ops) {
    if (op.cookie == 0 || !is_move(op.change)) continue;
    halves[op.cookie] |= op.change == ChangeKind::MovedFrom ? 1 : 2;
  }
  if (halves.empty()) return;
  for (Op& op : ops) {
    if (!is_move(op.change)) continue;
    const auto it = halves.find(op.cookie);
    if (op.cookie != 0 && it != halves.end() && it->second == 3) continue;
    op.change = op.change == ChangeKind::MovedFrom ? ChangeKind::Removed : ChangeKind::Created;
    op.cookie = 0;
  }
}

/// Collapses bursts of watcher events into a minimal ordered batch.
///
/// - Repeated events on one path collapse to the last relevant operation.
/// - Removing a directory drops pending operations on everything below it.
/// - An overflow discards everything and yields a single Rescan of the root.
/// - A batch becomes ready after a quiet period, after a maximum latency since
///   its first event, or when the pending count reaches a cap.
///
/// Time is injected so the class is deterministic and testable.
class Coalescer {
 public:
  using clock = std::chrono::steady_clock;

  struct Options {
    std::size_t max_pending = 4096;
    clock::duration quiet_period = std::chrono::milliseconds(2);
    clock::duration max_latency = std::chrono::milliseconds(25);
  };

  Coalescer() noexcept : Coalescer(Options{}) {}
  explicit Coalescer(Options opts) noexcept : opts_(opts) {}

  void push(const FsEvent& ev, clock::time_point now);

  [[nodiscard]] bool ready(clock::time_point now) const noexcept;

  /// When `ready` would flip to true if no further events arrive; nullopt if idle.
  [[nodiscard]] std::optional<clock::time_point> deadline() const noexcept;

  /// Returns the pending batch ordered by the arrival time of each path's last
  /// event, and resets the coalescer.
  [[nodiscard]] std::vector<PlannedOp> take();

  [[nodiscard]] std::size_t pending() const noexcept { return overflow_ ? 1 : pending_.size(); }
  [[nodiscard]] bool empty() const noexcept { return !overflow_ && pending_.empty(); }
  [[nodiscard]] bool overflowed() const noexcept { return overflow_; }

 private:
  struct Entry {
    InvalidationKind kind;
    std::uint64_t seq;
    bool appeared;
    ChangeKind change;
    std::uint32_t cookie;
  };

  Options opts_;
  std::unordered_map<std::string, Entry, StringHash, std::equal_to<>> pending_;
  std::uint64_t seq_ = 0;
  bool overflow_ = false;
  clock::time_point first_{};
  clock::time_point last_{};
};

}  // namespace wsld
