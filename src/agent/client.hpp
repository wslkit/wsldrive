#pragma once

#include "core/error.hpp"
#include "core/metadata_tree.hpp"
#include "net/frame_channel.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <list>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <shared_mutex>
#include <span>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wsld::agent {

/// Client side of a served root: keeps a MetadataTree mirror up to date from
/// snapshots and pushed invalidations, and issues reads.
///
/// A reader thread owns `receive()`; responses are matched to waiting callers
/// by request id, invalidations are applied to the tree under the write lock.
class RemoteRoot {
 public:
  struct Stats {
    std::uint64_t invalidation_batches = 0;
    std::uint64_t invalidation_ops = 0;
    std::uint64_t generation = 0;
    std::size_t snapshot_bytes = 0;
    std::chrono::nanoseconds last_snapshot_time{0};
    std::uint64_t read_cache_hits = 0;
    std::uint64_t read_cache_misses = 0;
    std::uint64_t read_cache_bytes = 0;
    // Cold-read instrumentation: time spent fetching bytes on a read miss (the
    // boundary cost that warming hides), and what the background prefetcher did.
    std::uint64_t read_miss_fetch_ns = 0;
    std::uint64_t prefetch_files = 0;
    std::uint64_t prefetch_bytes = 0;
    // Snapshots re-fetched because the agent's watcher overflowed (Rescan), and
    // re-fetches that failed (the mirror is then stale until the next one).
    std::uint64_t rescans = 0;
    std::uint64_t rescan_failures = 0;
  };

  /// One far-side change this client actually applied to its mirror, described
  /// the way a change-notification consumer needs it rather than the way the
  /// mirror needed it.
  ///
  /// The difference is what the mirror knows and the agent does not. The agent
  /// reports that a path exists and has changed; only the mirror can say
  /// whether it had that path a moment ago, which is what separates a file
  /// being created from the same file being rewritten. `change` here is that
  /// verdict, already made.
  struct AppliedChange {
    ChangeKind change = ChangeKind::Unknown;
    std::string path;          // relative, '/'-separated
    std::uint32_t cookie = 0;  // pairs the two halves of a move
    NodeKind kind = NodeKind::File;  // what the path is — for a removal, what it was
    // The whole mirror was replaced: the agent's watcher lost events, so no
    // per-path change can be named. `path` is empty and every other field
    // meaningless. Delivered after the replacement snapshot has landed.
    bool rescan = false;
  };

  using InvalidationHook = std::function<void(const proto::InvalidationBatch&)>;
  /// Changes a batch actually made to the mirror - the ops that were applied,
  /// not the ones discarded as stale relative to this client's own mutation.
  /// Separate from InvalidationHook, which reports the raw batch for
  /// diagnostics (`wsldrive watch`) and so has different callers.
  using InvalidatedPathsHook = std::function<void(std::span<const AppliedChange>)>;

  explicit RemoteRoot(std::unique_ptr<net::FrameChannel> ch);
  ~RemoteRoot();
  RemoteRoot(const RemoteRoot&) = delete;
  RemoteRoot& operator=(const RemoteRoot&) = delete;

  /// Shared secret presented in the Hello handshake; must match the agent's.
  /// Set before connect().
  void set_auth_token(std::string token) { token_ = std::move(token); }

  /// How this client resolves paths against the mirror. It must match what the
  /// caller resolves with: the mount looks paths up case-insensitively, and a
  /// read that resolved one way but keyed its cache the other missed the cache
  /// on every differently-spelled open (and, against a Linux agent, failed).
  void set_lookup_mode(LookupMode mode) noexcept { lookup_mode_.store(mode, std::memory_order_relaxed); }

  /// Starts the reader thread and performs the Hello handshake.
  [[nodiscard]] Result<proto::Hello> connect(std::chrono::milliseconds timeout = std::chrono::seconds(10));

  /// Requests a full snapshot and replaces the tree with it. Invalidations that
  /// arrive while the request is in flight and post-date the snapshot are
  /// re-applied on top, so nothing observed during the fetch is lost.
  [[nodiscard]] Result<void> fetch_snapshot(std::chrono::milliseconds timeout = std::chrono::minutes(5));

  /// Reads `length` bytes at `offset` of `path`; returns fewer at EOF. Small
  /// files are cached in RAM and served locally on repeat reads until an
  /// invalidation (or a size/mtime change) supersedes them.
  [[nodiscard]] Result<std::vector<std::byte>> read(std::string_view path, std::uint64_t offset, std::uint32_t length,
                                                    std::chrono::milliseconds timeout = std::chrono::seconds(30));

  /// Reads into a caller-supplied buffer and returns the byte count. On a cache
  /// hit this copies straight from the cached file into `out`, so the hot path
  /// (a mount reading a cached file in chunks) does no intermediate allocation.
  /// Misses take the same path as read().
  [[nodiscard]] Result<std::size_t> read_into(std::string_view path, std::uint64_t offset, std::span<std::byte> out,
                                              std::chrono::milliseconds timeout = std::chrono::seconds(30));

  /// Ceilings on a streamed snapshot. Nothing else bounds the accumulation, so
  /// a server that keeps setting the more-frames flag can grow these until the
  /// client is out of memory. A million nodes is far past any tree worth
  /// mounting, and the byte cap covers pathological name lengths.
  static constexpr std::size_t kMaxSnapshotEntries = 1u << 20;
  static constexpr std::size_t kMaxSnapshotBytes = 1u << 30;  // 1 GiB

  /// Largest file the read cache will hold whole; larger reads stream uncached.
  static constexpr std::uint64_t kMaxCacheableFile = 8u << 20;

  /// Reads several files in one round-trip; each result is the whole file, or
  /// nullopt if the server could not return it (caller may fetch individually).
  [[nodiscard]] Result<std::vector<std::optional<std::vector<std::byte>>>> read_many(
      const std::vector<std::string>& paths, std::chrono::milliseconds timeout = std::chrono::seconds(30));

  /// Target of a symlink in the tree, as stored ('/'-separated). Cached per path
  /// until an invalidation touches it, so listing a directory full of links costs
  /// one round-trip per link, once.
  [[nodiscard]] Result<std::string> readlink(std::string_view path,
                                             std::chrono::milliseconds timeout = std::chrono::seconds(30));

  [[nodiscard]] Result<std::chrono::nanoseconds> ping(std::chrono::milliseconds timeout = std::chrono::seconds(5));

  /// Proactively warm the read cache after mount: queue every directory whose
  /// small files fit within the cache budget for background prefetch, so the
  /// first reads a tool makes are already warm (cold reads avoided). Returns the
  /// number of directories queued; skips directories once the estimated small-
  /// file bytes would exceed the cache cap (huge trees fall back to lazy
  /// read-ahead). Non-blocking — the background prefetcher does the work.
  std::size_t warm_cache();

  /// Blocks until the background prefetcher has drained its queue (or the
  /// connection closes / the timeout elapses). Mainly for tests and benchmarks.
  void wait_prefetch_idle(std::chrono::milliseconds timeout = std::chrono::minutes(5));

  /// Byte budget for the in-RAM read cache (default 256 MiB). Lowering it evicts
  /// least-recently-used entries immediately.
  void set_read_cache_limit(std::uint64_t bytes);

  // --- write-through mutations (Phase 3) -------------------------------------
  // Each performs the operation on the far side, then optimistically updates the
  // local mirror so an immediate stat/read is consistent; the server's watcher
  // also pushes an invalidation that reconciles any drift.
  [[nodiscard]] Result<void> create_file(std::string_view path, std::uint32_t mode = 0644,
                                         std::chrono::milliseconds timeout = std::chrono::seconds(30));
  [[nodiscard]] Result<std::uint64_t> write(std::string_view path, std::uint64_t offset,
                                            std::span<const std::byte> data,
                                            std::chrono::milliseconds timeout = std::chrono::seconds(30));
  [[nodiscard]] Result<void> truncate(std::string_view path, std::uint64_t size,
                                      std::chrono::milliseconds timeout = std::chrono::seconds(30));
  [[nodiscard]] Result<void> mkdir(std::string_view path, std::uint32_t mode = 0755,
                                   std::chrono::milliseconds timeout = std::chrono::seconds(30));
  [[nodiscard]] Result<void> unlink(std::string_view path,
                                    std::chrono::milliseconds timeout = std::chrono::seconds(30));
  [[nodiscard]] Result<void> rmdir(std::string_view path,
                                   std::chrono::milliseconds timeout = std::chrono::seconds(30));
  [[nodiscard]] Result<void> rename(std::string_view from, std::string_view to,
                                    std::chrono::milliseconds timeout = std::chrono::seconds(30));

  /// Read access to the mirrored tree. Hold the lock only briefly.
  template <class F>
  auto with_tree(F&& f) const -> decltype(f(std::declval<const MetadataTree&>())) {
    std::shared_lock lock(tree_mu_);
    return f(tree_);
  }

  void set_invalidation_hook(InvalidationHook hook);
  void set_invalidated_paths_hook(InvalidatedPathsHook hook);
  [[nodiscard]] Stats stats() const;
  [[nodiscard]] bool connected() const noexcept { return !closed_.load(); }
  void close();

 private:
  struct Pending {
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    proto::MsgType type{};
    std::vector<std::byte> payload;
    // Streaming (multi-frame) reply accumulation, used for snapshots.
    bool streaming = false;
    bool stream_ok = true;
    std::uint64_t snap_generation = 0;
    std::size_t snap_bytes = 0;
    std::vector<std::string> snap_names;      // owns entry names across frames
    std::vector<SnapshotEntry> snap_entries;  // .name filled in by the caller at the end
  };

  [[nodiscard]] Result<std::shared_ptr<Pending>> request(proto::MsgType type, std::span<const std::byte> payload,
                                                         std::chrono::milliseconds timeout, bool streaming = false);
  [[nodiscard]] Result<std::vector<std::byte>> read_remote(std::string_view path, std::uint64_t offset,
                                                           std::uint32_t length, std::chrono::milliseconds timeout);
  void reader_loop();
  void apply_invalidation(std::span<const std::byte> payload);
  void fail_all_pending();
  void drop_cached(std::string_view path);  // evict one read-cache entry
  // Evicts every cached entry at or below `dir`. A directory that is removed or
  // renamed away takes its subtree's cached contents with it; the cache is
  // path-keyed, so those entries would otherwise stay resident and unreachable
  // until LRU pressure happened to evict them.
  void drop_cached_prefix(std::string_view dir);
  void cache_put(std::string path, std::int64_t mtime_ns, std::uint64_t size, std::vector<std::byte> data);

  LookupMode mode() const noexcept { return lookup_mode_.load(std::memory_order_relaxed); }

  // What a read needs to know about a path: whether it is a cacheable file, the
  // version to validate against, and the key its cache entry should use.
  struct ReadTarget {
    bool is_file = false;
    std::int64_t mtime_ns = 0;
    std::uint64_t size = 0;
    // Set only when `path` was not already spelled the way the tree stores it,
    // so the common case costs no allocation.
    std::string canonical;
  };
  [[nodiscard]] ReadTarget resolve_for_read(std::string_view path) const;

  // A local mtime for an optimistic mirror update, strictly newer than
  // `previous`. Its only job is to differ from the value a concurrent read may
  // already have cached under, so that entry fails validation instead of
  // serving pre-write bytes; the watcher's event replaces it with the real one.
  [[nodiscard]] static std::int64_t bump_mtime(std::int64_t previous) noexcept;

  // Background read-ahead: on a read miss, the file's directory is queued and a
  // worker bulk-fetches its not-yet-cached siblings so later reads hit the cache.
  void enqueue_prefetch(std::string dir);
  void prefetch_loop();
  // Re-fetches the snapshot when the agent signals a Rescan (its watcher lost
  // events). Runs on its own thread: the request blocks, and the reader thread
  // that receives the Rescan is the one that would have to answer it.
  void rescan_loop();

  std::string token_;  // presented in Hello
  std::unique_ptr<net::FrameChannel> ch_;
  std::thread reader_;
  std::atomic<bool> closed_{false};  // connection is dead (set by reader on EOF or by close())
  std::once_flag cleanup_;           // ensures shutdown + reader join happen exactly once
  std::atomic<std::uint64_t> next_request_{1};

  std::mutex pending_mu_;
  std::unordered_map<std::uint64_t, std::shared_ptr<Pending>> pending_;

  // Atomic because the mount sets it after connect() has started the reader.
  std::atomic<LookupMode> lookup_mode_{LookupMode::Exact};

  mutable std::shared_mutex tree_mu_;
  MetadataTree tree_;
  // While a snapshot request is in flight, invalidations are applied to the old
  // tree AND kept here; load_snapshot() would otherwise discard them, and the
  // ones newer than the snapshot's generation are replayed on top. Both guarded
  // by tree_mu_ (write side).
  bool snapshot_in_flight_ = false;
  std::vector<proto::InvalidationBatch> snapshot_replay_;
  // Cap on buffered batches: a watcher that keeps overflowing would otherwise
  // grow this without bound for as long as the snapshot request is in flight.
  // Past the cap the replay is known to be incomplete, so the snapshot it feeds
  // cannot be trusted and another rescan is scheduled instead.
  static constexpr std::size_t kMaxSnapshotReplay = 4096;
  bool snapshot_replay_overflow_ = false;
  // One snapshot fetch at a time: the mount-time fetch and a Rescan-triggered
  // one overlapping would each end the other's replay recording.
  std::mutex snapshot_mu_;
  // Paths this client mutated, with the agent generation acknowledged for the
  // mutation. An invalidation op for such a path whose batch generation is not
  // newer was resolved before (or during) the mutation and is discarded; the
  // mutation's own watcher events follow in a newer batch. Guarded by tree_mu_.
  std::unordered_map<std::string, std::uint64_t, StringHash, std::equal_to<>> local_mutations_;
  void note_mutation(std::string_view path, std::uint64_t generation);  // call under tree_mu_ (write)
  // Reads the trailing ack of a mutation reply and records it for `paths`.
  Result<std::uint64_t> take_ack(proto::Reader& r) noexcept;

  std::thread rescan_;
  std::mutex rs_mu_;
  std::condition_variable rs_cv_;
  bool rs_requested_ = false;  // a Rescan arrived; coalesces any number of them
  bool rs_stop_ = false;
  // Back-off between full re-snapshots. Each load_snapshot holds tree_mu_
  // exclusively and blocks every mount operation behind it, so a watcher that
  // overflows repeatedly (one `npm install` fills the buffer) must not be able
  // to run them back to back. Doubles per consecutive rescan, reset by a quiet
  // spell.
  static constexpr std::chrono::milliseconds kRescanMinInterval{500};
  static constexpr std::chrono::milliseconds kRescanMaxInterval{30'000};
  std::chrono::steady_clock::time_point rs_last_{};
  std::chrono::milliseconds rs_backoff_{kRescanMinInterval};

  mutable std::mutex stats_mu_;
  Stats stats_;
  InvalidationHook hook_;
  InvalidatedPathsHook paths_hook_;

  // In-RAM read cache: path -> whole-file contents, validated against the tree's
  // (mtime, size) and dropped on invalidation. LRU-evicted to a byte budget.
  struct CacheEntry {
    std::int64_t mtime_ns = 0;
    std::uint64_t size = 0;
    // Position of this entry's key in `lru_` (front = most recently used), so a
    // hit reorders and an eviction picks a victim in O(1).
    std::list<std::string>::iterator lru{};
    // Shared so a reader can take a reference under the lock and copy its slice
    // after releasing it — the cache never mutates a buffer once published.
    std::shared_ptr<const std::vector<std::byte>> data;
  };
  mutable std::mutex rcache_mu_;
  std::unordered_map<std::string, CacheEntry, StringHash, std::equal_to<>> rcache_;
  std::list<std::string> lru_;  // most-recently-used first
  std::uint64_t rcache_bytes_ = 0;
  std::uint64_t rcache_cap_ = 256u << 20;  // 256 MiB
  // Symlink targets by path; tiny strings, so no budget. Also under rcache_mu_
  // and dropped by the same invalidation path as file contents.
  std::unordered_map<std::string, std::string, StringHash, std::equal_to<>> link_cache_;

  // Erases one entry, keeping `lru_` and the byte count in step. Call under
  // rcache_mu_.
  void cache_erase(std::unordered_map<std::string, CacheEntry, StringHash, std::equal_to<>>::iterator it);
  // Drops least-recently-used entries until the budget is met. Call under rcache_mu_.
  void evict_locked();

  std::thread prefetch_;
  std::mutex pf_mu_;
  std::condition_variable pf_cv_;
  std::condition_variable pf_done_cv_;       // signalled when the queue drains to empty
  std::deque<std::string> pf_queue_;
  std::unordered_set<std::string> pf_seen_;  // directories already queued/prefetched
  std::atomic<bool> pf_stop_{false};
  std::atomic<std::size_t> pf_pending_{0};   // dirs queued but not yet fully processed
  static constexpr std::size_t kPrefetchBatchBytes = 4u << 20;
  static constexpr std::size_t kPrefetchBatchCount = 512;
};

}  // namespace wsld::agent
