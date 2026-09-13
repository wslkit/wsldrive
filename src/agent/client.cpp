#include "agent/client.hpp"

#include "core/auth_token.hpp"
#include "core/coalescer.hpp"
#include "core/path.hpp"
#include "core/version.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iterator>
#include <utility>

namespace wsld::agent {

RemoteRoot::RemoteRoot(std::unique_ptr<net::FrameChannel> ch) : ch_(std::move(ch)) {}

RemoteRoot::~RemoteRoot() { close(); }

void RemoteRoot::close() {
  // `closed_` may already be set by the reader thread when the peer disconnects,
  // so cleanup is guarded by its own once_flag rather than by `closed_` — the
  // destructor must always join the reader, never skip it.
  closed_.store(true);
  std::call_once(cleanup_, [this] {
    {
      std::lock_guard lock(pf_mu_);
      pf_stop_.store(true);
    }
    pf_cv_.notify_all();
    {
      std::lock_guard lock(rs_mu_);
      rs_stop_ = true;
    }
    rs_cv_.notify_all();
    ch_->shutdown();
    ch_->close();
    if (reader_.joinable()) reader_.join();
    fail_all_pending();  // releases any in-flight prefetch read_many or rescan snapshot request
    if (prefetch_.joinable()) prefetch_.join();
    if (rescan_.joinable()) rescan_.join();
  });
}

void RemoteRoot::set_invalidated_paths_hook(InvalidatedPathsHook hook) {
  std::lock_guard lock(stats_mu_);
  paths_hook_ = std::move(hook);
}

void RemoteRoot::set_invalidation_hook(InvalidationHook hook) {
  std::lock_guard lock(stats_mu_);
  hook_ = std::move(hook);
}

RemoteRoot::Stats RemoteRoot::stats() const {
  std::lock_guard lock(stats_mu_);
  return stats_;
}

Result<proto::Hello> RemoteRoot::connect(std::chrono::milliseconds timeout) {
  if (!reader_.joinable()) reader_ = std::thread([this] { reader_loop(); });
  if (!prefetch_.joinable()) prefetch_ = std::thread([this] { prefetch_loop(); });
  if (!rescan_.joinable()) rescan_ = std::thread([this] { rescan_loop(); });
  // Mutual handshake. The old exchange sent the shared secret to whoever
  // accepted on the endpoint, so anything that bound the port first collected
  // the secret and could then serve a tree of its own choosing into a drive the
  // user trusts. Now: we send a nonce, the agent proves it knows the secret,
  // and only if that checks out do we prove ourselves. The secret itself never
  // goes on the wire in either direction.
  const std::string client_nonce = token_.empty() ? std::string() : generate_auth_nonce();
  if (!token_.empty() && client_nonce.empty()) return fail(Errc::IoError);  // no entropy: refuse

  std::vector<std::byte> payload;
  proto::Writer w(payload);
  proto::write_hello(w, proto::Hello{.protocol_version = proto::kVersion,
                                     .capabilities = 0,
                                     .agent = agent_string("wsldrive"),
                                     .nonce = client_nonce,
                                     .proof = {}});
  auto p = request(proto::MsgType::Hello, payload, timeout);
  if (!p) return fail(p.error());
  // An Error reply (e.g. a rejected peer) is already decoded by request().
  if ((*p)->type != proto::MsgType::HelloAck) return fail(Errc::ProtocolError);
  proto::Reader r((*p)->payload);
  auto ack = proto::read_hello(r);
  if (!ack) return fail(ack.error());

  if (!token_.empty()) {
    // Check the agent BEFORE proving anything to it. A wrong or absent proof
    // means this is not the agent our launcher started.
    const std::string expected = auth_proof(token_, client_nonce, ack->nonce, true);
    if (!constant_time_equal(ack->proof, expected)) return fail(Errc::Unauthorized);

    std::vector<std::byte> ap;
    proto::Writer aw(ap);
    proto::write_hello(aw, proto::Hello{.protocol_version = proto::kVersion,
                                        .capabilities = 0,
                                        .agent = {},
                                        .nonce = {},
                                        .proof = auth_proof(token_, client_nonce, ack->nonce, false)});
    auto a = request(proto::MsgType::Auth, ap, timeout);
    if (!a) return fail(a.error());
    if ((*a)->type != proto::MsgType::Ok) return fail(Errc::Unauthorized);
  }

  proto::Hello out = *ack;
  out.agent = {};  // the views would dangle once `p` dies; callers only need version/caps
  out.nonce = {};
  out.proof = {};
  return out;
}

Result<void> RemoteRoot::fetch_snapshot(std::chrono::milliseconds timeout) {
  const auto t0 = std::chrono::steady_clock::now();
  bool incomplete = false;
  std::lock_guard serialise(snapshot_mu_);  // see snapshot_mu_
  {
    // From here until the tree is replaced, apply_invalidation() also records
    // every batch so the ones the snapshot does not include can be replayed.
    std::unique_lock lock(tree_mu_);
    snapshot_in_flight_ = true;
    snapshot_replay_.clear();
  }
  // Whatever happens below, the recording stops when this call ends.
  struct EndRecording {
    RemoteRoot& self;
    ~EndRecording() {
      std::unique_lock lock(self.tree_mu_);
      self.snapshot_in_flight_ = false;
      self.snapshot_replay_.clear();
      self.snapshot_replay_overflow_ = false;
    }
  } end_recording{*this};

  auto p = request(proto::MsgType::SnapshotRequest, {}, timeout, /*streaming=*/true);
  if (!p) return fail(p.error());
  if ((*p)->type != proto::MsgType::Snapshot) return fail(Errc::ProtocolError);
  if (!(*p)->stream_ok) return fail(Errc::Corrupt);

  // Names were copied per frame; point each entry at its owned string now that
  // the accumulation vector will not grow again.
  auto& names = (*p)->snap_names;
  auto& entries = (*p)->snap_entries;
  for (std::size_t i = 0; i < entries.size(); ++i) entries[i].name = names[i];
  const std::uint64_t snap_gen = (*p)->snap_generation;
  {
    std::unique_lock lock(tree_mu_);
    if (auto res = tree_.load_snapshot(entries); !res) return res;
    // The agent stamped the snapshot with its generation before scanning, so a
    // batch with a higher generation describes a change the scan may have
    // missed; one at or below it is already in the snapshot.
    for (const proto::InvalidationBatch& b : snapshot_replay_) {
      if (b.generation <= snap_gen) continue;
      for (const proto::InvalidationOp& op : b.ops) {
        if (op.kind == InvalidationKind::Upsert) (void)tree_.upsert_path(op.path, op.attr);
        else if (op.kind == InvalidationKind::Remove) (void)tree_.remove_path(op.path);
      }
    }
    snapshot_replay_.clear();
    snapshot_in_flight_ = false;
    incomplete = snapshot_replay_overflow_;
    snapshot_replay_overflow_ = false;
  }
  {
    std::lock_guard lock(stats_mu_);
    stats_.generation = (*p)->snap_generation;
    stats_.snapshot_bytes = (*p)->snap_bytes;
    stats_.last_snapshot_time = std::chrono::steady_clock::now() - t0;
  }
  // Changes that landed during the fetch were dropped, so this tree has holes.
  // Ask for another one; rescan_loop's back-off keeps that from spinning.
  if (incomplete) {
    std::lock_guard lock(rs_mu_);
    rs_requested_ = true;
    rs_cv_.notify_one();
  }
  return {};
}

Result<std::vector<std::byte>> RemoteRoot::read_remote(std::string_view path, std::uint64_t offset,
                                                       std::uint32_t length, std::chrono::milliseconds timeout) {
  std::vector<std::byte> payload;
  proto::Writer w(payload);
  proto::write_read_request(w, proto::ReadRequest{.path = path, .offset = offset, .length = length});
  auto p = request(proto::MsgType::ReadRequest, payload, timeout);
  if (!p) return fail(p.error());
  if ((*p)->type != proto::MsgType::ReadResponse) return fail(Errc::ProtocolError);
  proto::Reader r((*p)->payload);
  auto resp = proto::read_read_response(r);
  if (!resp) return fail(resp.error());
  return std::vector<std::byte>(resp->data.begin(), resp->data.end());
}

namespace {
std::vector<std::byte> slice(const std::vector<std::byte>& data, std::uint64_t offset, std::uint32_t length) {
  if (offset >= data.size()) return {};
  const auto end = std::min<std::uint64_t>(offset + length, data.size());
  return std::vector<std::byte>(data.begin() + static_cast<std::ptrdiff_t>(offset),
                                data.begin() + static_cast<std::ptrdiff_t>(end));
}
}  // namespace

std::int64_t RemoteRoot::bump_mtime(std::int64_t previous) noexcept {
  const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  return std::max(now, previous + 1);  // strictly newer even if the clock steps back
}

RemoteRoot::ReadTarget RemoteRoot::resolve_for_read(std::string_view path) const {
  ReadTarget t;
  const LookupMode m = mode();
  std::shared_lock lock(tree_mu_);
  auto id = tree_.lookup(path, LookupMode::Exact);
  if (!id && m != LookupMode::Exact) {
    id = tree_.lookup(path, m);
    // Only a differently-spelled path needs the copy. Everything downstream has
    // to use the spelling that actually exists: the cache key (or every open of
    // `readme.md` for `README.md` misses), the read-ahead batch, and the path
    // the agent has to resolve on a possibly case-sensitive filesystem.
    if (id) t.canonical = tree_.path_of(*id);
  }
  if (!id) return t;
  const auto& n = tree_.node(*id);
  t.is_file = n.attr.kind == NodeKind::File;
  t.mtime_ns = n.attr.mtime_ns;
  t.size = n.attr.size;
  return t;
}

Result<std::size_t> RemoteRoot::read_into(std::string_view path, std::uint64_t offset, std::span<std::byte> out,
                                          std::chrono::milliseconds timeout) {
  // Fast path: a cached file is copied straight into the caller's buffer. A
  // mount reads a file in chunks, so avoiding the intermediate vector saves one
  // allocation and one copy per chunk.
  const ReadTarget t = resolve_for_read(path);
  const std::string_view key = t.canonical.empty() ? path : std::string_view(t.canonical);
  if (t.is_file && t.size <= kMaxCacheableFile) {
    std::shared_ptr<const std::vector<std::byte>> hit;
    {
      std::lock_guard lock(rcache_mu_);
      if (const auto it = rcache_.find(key);
          it != rcache_.end() && it->second.mtime_ns == t.mtime_ns && it->second.size == t.size) {
        lru_.splice(lru_.begin(), lru_, it->second.lru);
        hit = it->second.data;
      }
    }
    if (hit) {
      const std::size_t n = offset >= hit->size()
                                ? 0
                                : std::min<std::size_t>(out.size(), hit->size() - static_cast<std::size_t>(offset));
      if (n > 0) std::memcpy(out.data(), hit->data() + offset, n);
      std::lock_guard s(stats_mu_);
      ++stats_.read_cache_hits;
      return n;
    }
  }
  // Miss (or an uncacheable file): reuse the read path with the canonical
  // spelling, so it caches under the key this function will look for next time.
  auto data = read(key, offset, static_cast<std::uint32_t>(std::min<std::size_t>(out.size(), 0xFFFFFFFFu)), timeout);
  if (!data) return fail(data.error());
  const std::size_t n = std::min(out.size(), data->size());
  if (n > 0) std::memcpy(out.data(), data->data(), n);
  return n;
}

Result<std::vector<std::byte>> RemoteRoot::read(std::string_view path, std::uint64_t offset, std::uint32_t length,
                                                std::chrono::milliseconds timeout) {
  // Version, cacheability and the canonical spelling come from the in-RAM mirror.
  const ReadTarget t = resolve_for_read(path);
  const std::string_view key = t.canonical.empty() ? path : std::string_view(t.canonical);
  const std::int64_t mtime = t.mtime_ns;
  const std::uint64_t size = t.size;
  if (!t.is_file || size > kMaxCacheableFile) return read_remote(key, offset, length, timeout);

  {
    // Take a reference to the buffer under the lock, then copy the requested
    // range after releasing it: the copy is the long part, and holding the cache
    // mutex across it serialised every concurrent reader of cached files.
    std::shared_ptr<const std::vector<std::byte>> hit;
    {
      std::lock_guard lock(rcache_mu_);
      if (const auto it = rcache_.find(key);
          it != rcache_.end() && it->second.mtime_ns == mtime && it->second.size == size) {
        lru_.splice(lru_.begin(), lru_, it->second.lru);  // most-recently-used
        hit = it->second.data;
      }
    }
    if (hit) {
      auto out = slice(*hit, offset, length);
      std::lock_guard s(stats_mu_);
      ++stats_.read_cache_hits;
      return out;
    }
  }

  // Miss: synchronous read-ahead. Fetch the target plus its uncached small
  // siblings in one bulk round-trip so a sequential reader over a directory pays
  // one request per directory, not one per file. The async prefetcher then picks
  // up any siblings beyond this batch.
  const std::string target(key);
  // `key` is canonical, so its parent is too and an Exact lookup is right below.
  const std::string dir(split_parent(key).first);
  std::vector<std::string> batch{target};
  std::vector<std::pair<std::int64_t, std::uint64_t>> meta{{mtime, size}};
  std::uint64_t bytes = size;
  {
    std::shared_lock lock(tree_mu_);
    if (const auto did = tree_.lookup(dir, LookupMode::Exact); did && tree_.node(*did).is_dir()) {
      tree_.for_each_child(*did, [&](NodeId c) {
        if (batch.size() >= 256 || bytes >= (2u << 20)) return;
        const auto& n = tree_.node(c);
        if (n.attr.kind != NodeKind::File || n.attr.size == 0 || n.attr.size > kMaxCacheableFile) return;
        std::string p = dir.empty() ? std::string(tree_.name(c)) : dir + "/" + std::string(tree_.name(c));
        if (p == target) return;
        batch.push_back(std::move(p));
        meta.push_back({n.attr.mtime_ns, n.attr.size});
        bytes += n.attr.size;
      });
    }
  }

  std::vector<std::byte> target_bytes;
  bool have_target = false;
  const auto fetch_t0 = std::chrono::steady_clock::now();  // measure the boundary fetch cost
  if (auto res = read_many(batch, timeout); res && (*res)[0].has_value()) {
    for (std::size_t i = 0; i < batch.size(); ++i)
      if ((*res)[i]) {
        if (i == 0) {
          target_bytes = *(*res)[0];
          have_target = true;
        }
        cache_put(batch[i], meta[i].first, meta[i].second, std::move(*(*res)[i]));
      }
  }
  if (!have_target) {  // bulk path unavailable; fetch the target directly
    auto whole = read_remote(key, 0, static_cast<std::uint32_t>(size), timeout);
    if (!whole) return whole;
    target_bytes = *whole;
    cache_put(target, mtime, size, std::move(*whole));
  }
  const auto fetch_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - fetch_t0)
                            .count();
  {
    std::lock_guard s(stats_mu_);
    ++stats_.read_cache_misses;
    stats_.read_miss_fetch_ns += static_cast<std::uint64_t>(fetch_ns);
  }
  if (batch.size() >= 256) enqueue_prefetch(dir);  // large dir: async-fetch the remainder
  return slice(target_bytes, offset, length);
}

void RemoteRoot::evict_locked() {
  // Evict least-recently-used first. The LRU list makes each victim O(1); a scan
  // for the minimum would make a full cache O(n) per insert (O(n^2) while
  // thrashing the cap).
  while (rcache_bytes_ > rcache_cap_ && rcache_.size() > 1) {
    const auto victim = rcache_.find(lru_.back());
    if (victim == rcache_.end()) {  // should not happen; keep the list consistent
      lru_.pop_back();
      continue;
    }
    cache_erase(victim);
  }
}

void RemoteRoot::set_read_cache_limit(std::uint64_t bytes) {
  std::lock_guard lock(rcache_mu_);
  rcache_cap_ = bytes;
  evict_locked();
  std::lock_guard s(stats_mu_);
  stats_.read_cache_bytes = rcache_bytes_;
}

void RemoteRoot::cache_erase(std::unordered_map<std::string, CacheEntry, StringHash, std::equal_to<>>::iterator it) {
  rcache_bytes_ -= it->second.data ? it->second.data->size() : 0;
  lru_.erase(it->second.lru);
  rcache_.erase(it);
}

void RemoteRoot::cache_put(std::string path, std::int64_t mtime_ns, std::uint64_t size, std::vector<std::byte> data) {
  auto buf = std::make_shared<const std::vector<std::byte>>(std::move(data));
  std::lock_guard lock(rcache_mu_);
  if (const auto it = rcache_.find(path); it != rcache_.end()) cache_erase(it);
  rcache_bytes_ += buf->size();
  lru_.push_front(path);
  rcache_.insert_or_assign(std::move(path), CacheEntry{mtime_ns, size, lru_.begin(), std::move(buf)});
  evict_locked();
  std::lock_guard s(stats_mu_);
  stats_.read_cache_bytes = rcache_bytes_;
}

Result<std::vector<std::optional<std::vector<std::byte>>>> RemoteRoot::read_many(const std::vector<std::string>& paths,
                                                                                 std::chrono::milliseconds timeout) {
  std::vector<std::byte> payload;
  proto::Writer w(payload);
  proto::ReadManyRequest req;
  req.paths.reserve(paths.size());
  for (const auto& p : paths) req.paths.emplace_back(p);
  proto::write_read_many_request(w, req);
  auto p = request(proto::MsgType::ReadManyRequest, payload, timeout);
  if (!p) return fail(p.error());
  if ((*p)->type != proto::MsgType::ReadManyResponse) return fail(Errc::ProtocolError);
  proto::Reader r((*p)->payload);
  auto resp = proto::read_read_many_response(r);
  if (!resp) return fail(resp.error());
  std::vector<std::optional<std::vector<std::byte>>> out(paths.size());
  for (std::size_t i = 0; i < resp->items.size() && i < out.size(); ++i)
    if (resp->items[i].ok) out[i] = std::vector<std::byte>(resp->items[i].data.begin(), resp->items[i].data.end());
  return out;
}

void RemoteRoot::enqueue_prefetch(std::string dir) {
  std::lock_guard lock(pf_mu_);
  if (pf_stop_.load()) return;
  if (!pf_seen_.insert(dir).second) return;  // already queued or done
  pf_pending_.fetch_add(1, std::memory_order_relaxed);
  pf_queue_.push_back(std::move(dir));
  pf_cv_.notify_one();
}

void RemoteRoot::prefetch_loop() {
  struct FileMeta {
    std::string path;
    std::int64_t mtime;
    std::uint64_t size;
  };
  for (;;) {
    std::string dir;
    {
      std::unique_lock lock(pf_mu_);
      pf_cv_.wait(lock, [&] { return pf_stop_.load() || !pf_queue_.empty(); });
      if (pf_stop_.load()) return;
      dir = std::move(pf_queue_.front());
      pf_queue_.pop_front();
    }

    // Enumerate this directory's small regular files from the mirror.
    std::vector<FileMeta> files;
    {
      std::shared_lock lock(tree_mu_);
      const auto id = tree_.lookup(dir, LookupMode::Exact);
      if (id && tree_.node(*id).is_dir()) {
        tree_.for_each_child(*id, [&](NodeId c) {
          const auto& n = tree_.node(c);
          if (n.attr.kind == NodeKind::File && n.attr.size > 0 && n.attr.size <= kMaxCacheableFile) {
            std::string p = dir.empty() ? std::string(tree_.name(c)) : dir + "/" + std::string(tree_.name(c));
            files.push_back(FileMeta{std::move(p), n.attr.mtime_ns, n.attr.size});
          }
        });
      }
    }

    std::vector<std::string> batch;
    std::vector<const FileMeta*> meta;
    std::uint64_t bytes = 0;
    std::uint64_t got_files = 0, got_bytes = 0;  // fetched this directory (for stats)
    auto flush = [&] {
      if (batch.empty() || pf_stop_.load()) {
        batch.clear();
        meta.clear();
        bytes = 0;
        return;
      }
      if (auto res = read_many(batch); res) {
        for (std::size_t i = 0; i < batch.size(); ++i)
          if ((*res)[i]) {
            got_files += 1;
            got_bytes += (*res)[i]->size();
            cache_put(batch[i], meta[i]->mtime, meta[i]->size, std::move(*(*res)[i]));
          }
      }
      batch.clear();
      meta.clear();
      bytes = 0;
    };

    for (const FileMeta& fm : files) {
      if (pf_stop_.load()) break;
      {  // skip files already cached at the current version
        std::lock_guard lock(rcache_mu_);
        const auto it = rcache_.find(fm.path);
        if (it != rcache_.end() && it->second.mtime_ns == fm.mtime && it->second.size == fm.size) continue;
      }
      batch.push_back(fm.path);
      meta.push_back(&fm);
      bytes += fm.size;
      if (batch.size() >= kPrefetchBatchCount || bytes >= kPrefetchBatchBytes) flush();
    }
    flush();

    if (got_files) {
      std::lock_guard s(stats_mu_);
      stats_.prefetch_files += got_files;
      stats_.prefetch_bytes += got_bytes;
    }
    // This directory is done; wake anyone waiting for the prefetcher to drain.
    if (pf_pending_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      std::lock_guard lock(pf_mu_);
      pf_done_cv_.notify_all();
    }
  }
}

std::size_t RemoteRoot::warm_cache() {
  std::vector<std::string> dirs;
  {
    std::shared_lock lock(tree_mu_);
    const std::uint64_t cap = rcache_cap_;
    std::uint64_t budget = 0;
    std::vector<NodeId> stack{tree_.root()};
    while (!stack.empty()) {
      const NodeId id = stack.back();
      stack.pop_back();
      if (!tree_.node(id).is_dir()) continue;
      std::uint64_t dir_bytes = 0;
      tree_.for_each_child(id, [&](NodeId c) {
        const auto& n = tree_.node(c);
        if (n.attr.kind == NodeKind::File && n.attr.size > 0 && n.attr.size <= kMaxCacheableFile)
          dir_bytes += n.attr.size;
        if (n.is_dir()) stack.push_back(c);  // descend regardless of this dir's budget
      });
      if (dir_bytes == 0) continue;                 // nothing cacheable here
      if (budget + dir_bytes > cap) continue;       // over budget: leave to lazy read-ahead
      budget += dir_bytes;
      dirs.push_back(tree_.path_of(id));
    }
  }
  // Enqueue without the tree lock held (prefetch_loop takes pf_mu_ then tree_mu_).
  for (auto& d : dirs) enqueue_prefetch(std::move(d));
  return dirs.size();
}

void RemoteRoot::rescan_loop() {
  for (;;) {
    {
      std::unique_lock lock(rs_mu_);
      rs_cv_.wait(lock, [&] { return rs_stop_ || rs_requested_; });
      if (rs_stop_) return;
      rs_requested_ = false;  // any Rescan arriving from here on schedules another pass

      // Back-off. A full re-snapshot holds tree_mu_ exclusively for as long as
      // it takes to rebuild the tree (~200 ms per million nodes) and every
      // mount operation waits behind it, so an agent whose watcher keeps
      // overflowing must not be able to trigger them back to back.
      const auto now = std::chrono::steady_clock::now();
      if (rs_last_.time_since_epoch() != std::chrono::steady_clock::duration::zero()) {
        // A quiet spell means the storm is over; start from the floor again.
        if (now - rs_last_ > kRescanMaxInterval) rs_backoff_ = kRescanMinInterval;
        const auto earliest = rs_last_ + rs_backoff_;
        if (now < earliest && rs_cv_.wait_until(lock, earliest, [&] { return rs_stop_; })) return;
        rs_backoff_ = std::min(rs_backoff_ * 2, kRescanMaxInterval);
      }
      rs_last_ = std::chrono::steady_clock::now();
    }
    if (closed_) return;
    // fetch_snapshot() replaces the mirror and replays whatever changed while
    // the request was in flight, so the tree is current again — not merely as
    // of the moment the agent started scanning.
    if (!fetch_snapshot()) {  // connection gone or a bad reply; nothing more to do here
      std::lock_guard s(stats_mu_);
      ++stats_.rescan_failures;
      continue;
    }
    {
      // Directories may have changed without per-path events, so let the
      // read-ahead visit them again on the next miss.
      std::lock_guard lock(pf_mu_);
      pf_seen_.clear();
    }
    // Tell the change-notification consumer that the mirror was replaced, now
    // that it has been. Nothing can name what changed - that is what an
    // overflow means - so this is the one signal it gets, and it goes out after
    // the new snapshot has landed rather than when the Rescan arrived.
    InvalidatedPathsHook paths_hook;
    {
      std::lock_guard s(stats_mu_);
      ++stats_.rescans;
      paths_hook = paths_hook_;
    }
    if (paths_hook) {
      const AppliedChange rescan_change{ChangeKind::Unknown, std::string{}, 0, NodeKind::Directory, true};
      paths_hook(std::span<const AppliedChange>(&rescan_change, 1));
    }
  }
}

void RemoteRoot::wait_prefetch_idle(std::chrono::milliseconds timeout) {
  std::unique_lock lock(pf_mu_);
  pf_done_cv_.wait_for(lock, timeout, [&] {
    return pf_stop_.load() || (pf_pending_.load(std::memory_order_acquire) == 0 && pf_queue_.empty());
  });
}

Result<std::chrono::nanoseconds> RemoteRoot::ping(std::chrono::milliseconds timeout) {
  const auto t0 = std::chrono::steady_clock::now();
  auto p = request(proto::MsgType::Ping, {}, timeout);
  if (!p) return fail(p.error());
  if ((*p)->type != proto::MsgType::Pong) return fail(Errc::ProtocolError);
  return std::chrono::steady_clock::now() - t0;
}

void RemoteRoot::drop_cached(std::string_view path) {
  std::lock_guard lock(rcache_mu_);
  if (const auto it = rcache_.find(path); it != rcache_.end()) cache_erase(it);
  if (const auto it = link_cache_.find(path); it != link_cache_.end()) link_cache_.erase(it);
  std::lock_guard s(stats_mu_);
  stats_.read_cache_bytes = rcache_bytes_;  // an eviction changes the total too
}

void RemoteRoot::drop_cached_prefix(std::string_view dir) {
  std::lock_guard lock(rcache_mu_);
  if (dir.empty()) {  // the whole served tree
    while (!rcache_.empty()) cache_erase(rcache_.begin());
    link_cache_.clear();
  } else {
    std::string prefix(dir);
    prefix.push_back('/');
    const auto under = [&](std::string_view k) { return k == dir || k.starts_with(prefix); };
    for (auto it = rcache_.begin(); it != rcache_.end();) {
      if (!under(it->first)) {
        ++it;
        continue;
      }
      const auto next = std::next(it);  // cache_erase invalidates only `it`
      cache_erase(it);
      it = next;
    }
    for (auto it = link_cache_.begin(); it != link_cache_.end();)
      it = under(it->first) ? link_cache_.erase(it) : std::next(it);
  }
  std::lock_guard s(stats_mu_);
  stats_.read_cache_bytes = rcache_bytes_;
}

Result<std::string> RemoteRoot::readlink(std::string_view path, std::chrono::milliseconds timeout) {
  {
    std::lock_guard lock(rcache_mu_);
    if (const auto it = link_cache_.find(path); it != link_cache_.end()) return it->second;
  }
  std::vector<std::byte> pl;
  proto::Writer w(pl);
  proto::write_path_request(w, proto::PathRequest{.path = path});
  auto p = request(proto::MsgType::ReadlinkRequest, pl, timeout);
  if (!p) return fail(p.error());
  if ((*p)->type != proto::MsgType::ReadlinkResponse) return fail(Errc::ProtocolError);
  proto::Reader r((*p)->payload);
  auto resp = proto::read_readlink_response(r);
  if (!resp) return fail(resp.error());
  std::string target(resp->target);
  std::lock_guard lock(rcache_mu_);
  link_cache_.insert_or_assign(std::string(path), target);
  return target;
}

void RemoteRoot::note_mutation(std::string_view path, std::uint64_t generation) {
  // Without a watcher nothing ever retires these; keep the table from growing
  // without bound on a long-lived mount that only writes.
  if (local_mutations_.size() > 65536) local_mutations_.clear();
  local_mutations_.insert_or_assign(normalize_path(path), generation);
}

Result<std::uint64_t> RemoteRoot::take_ack(proto::Reader& r) noexcept {
  auto a = proto::read_mutation_ack(r);
  if (!a) return fail(a.error());
  return a->generation;
}

Result<void> RemoteRoot::create_file(std::string_view path, std::uint32_t mode, std::chrono::milliseconds timeout) {
  std::vector<std::byte> pl;
  proto::Writer w(pl);
  proto::write_create_request(w, proto::CreateRequest{.path = path, .mode = mode});
  auto p = request(proto::MsgType::CreateRequest, pl, timeout);
  if (!p) return fail(p.error());
  if ((*p)->type != proto::MsgType::CreateResponse) return fail(Errc::ProtocolError);
  proto::Reader r((*p)->payload);
  auto attr = proto::read_attributes(r);
  if (!attr) return fail(attr.error());
  auto gen = take_ack(r);
  if (!gen) return fail(gen.error());
  std::unique_lock lock(tree_mu_);
  (void)tree_.upsert_path(path, *attr);
  note_mutation(path, *gen);
  return {};
}

Result<std::uint64_t> RemoteRoot::write(std::string_view path, std::uint64_t offset, std::span<const std::byte> data,
                                        std::chrono::milliseconds timeout) {
  std::vector<std::byte> pl;
  proto::Writer w(pl);
  proto::write_write_request(w, proto::WriteRequest{.path = path, .offset = offset, .data = data});
  auto p = request(proto::MsgType::WriteRequest, pl, timeout);
  if (!p) return fail(p.error());
  if ((*p)->type != proto::MsgType::WriteResponse) return fail(Errc::ProtocolError);
  proto::Reader r((*p)->payload);
  auto resp = proto::read_write_response(r);
  if (!resp) return fail(resp.error());
  const std::uint64_t end = offset + resp->written;
  drop_cached(path);
  std::unique_lock lock(tree_mu_);
  if (const auto id = tree_.lookup(path, mode())) {
    Attributes a = tree_.node(*id).attr;
    a.size = std::max(a.size, end);
    // Move the version on. A read-ahead that fetched this file just before the
    // write can still be in flight with the pre-write bytes, about to cache
    // them under the old (mtime, size) - which the tree would still agree with,
    // so validation would pass and serve stale content. With the mtime bumped
    // that entry fails validation and the next read refetches, instead of
    // waiting on the watcher's event (or staying stale for good without one).
    a.mtime_ns = bump_mtime(a.mtime_ns);
    (void)tree_.set_attributes(*id, a);
  } else {
    (void)tree_.upsert_path(
        path, Attributes{.size = end, .mtime_ns = bump_mtime(0), .mode = 0644, .kind = NodeKind::File});
  }
  note_mutation(path, resp->generation);
  return resp->written;
}

Result<void> RemoteRoot::truncate(std::string_view path, std::uint64_t size, std::chrono::milliseconds timeout) {
  std::vector<std::byte> pl;
  proto::Writer w(pl);
  proto::write_truncate_request(w, proto::TruncateRequest{.path = path, .size = size});
  auto p = request(proto::MsgType::TruncateRequest, pl, timeout);
  if (!p) return fail(p.error());
  if ((*p)->type != proto::MsgType::Ok) return fail(Errc::ProtocolError);
  proto::Reader r((*p)->payload);
  auto gen = take_ack(r);
  if (!gen) return fail(gen.error());
  drop_cached(path);
  std::unique_lock lock(tree_mu_);
  if (const auto id = tree_.lookup(path, mode())) {
    Attributes a = tree_.node(*id).attr;
    a.size = size;
    a.mtime_ns = bump_mtime(a.mtime_ns);  // same race as write(); same fix
    (void)tree_.set_attributes(*id, a);
  }
  note_mutation(path, *gen);
  return {};
}

Result<void> RemoteRoot::mkdir(std::string_view path, std::uint32_t mode, std::chrono::milliseconds timeout) {
  std::vector<std::byte> pl;
  proto::Writer w(pl);
  proto::write_mkdir_request(w, proto::MkdirRequest{.path = path, .mode = mode});
  auto p = request(proto::MsgType::MkdirRequest, pl, timeout);
  if (!p) return fail(p.error());
  if ((*p)->type != proto::MsgType::Ok) return fail(Errc::ProtocolError);
  proto::Reader r((*p)->payload);
  auto gen = take_ack(r);
  if (!gen) return fail(gen.error());
  std::unique_lock lock(tree_mu_);
  (void)tree_.upsert_path(path, Attributes{.size = 0, .mtime_ns = 0, .mode = mode, .kind = NodeKind::Directory});
  note_mutation(path, *gen);
  return {};
}

Result<void> RemoteRoot::unlink(std::string_view path, std::chrono::milliseconds timeout) {
  std::vector<std::byte> pl;
  proto::Writer w(pl);
  proto::write_path_request(w, proto::PathRequest{.path = path});
  auto p = request(proto::MsgType::UnlinkRequest, pl, timeout);
  if (!p) return fail(p.error());
  if ((*p)->type != proto::MsgType::Ok) return fail(Errc::ProtocolError);
  proto::Reader r((*p)->payload);
  auto gen = take_ack(r);
  if (!gen) return fail(gen.error());
  drop_cached(path);
  std::unique_lock lock(tree_mu_);
  (void)tree_.remove_path(path, mode());
  note_mutation(path, *gen);
  return {};
}

Result<void> RemoteRoot::rmdir(std::string_view path, std::chrono::milliseconds timeout) {
  std::vector<std::byte> pl;
  proto::Writer w(pl);
  proto::write_path_request(w, proto::PathRequest{.path = path});
  auto p = request(proto::MsgType::RmdirRequest, pl, timeout);
  if (!p) return fail(p.error());
  if ((*p)->type != proto::MsgType::Ok) return fail(Errc::ProtocolError);
  proto::Reader r((*p)->payload);
  auto gen = take_ack(r);
  if (!gen) return fail(gen.error());
  drop_cached_prefix(path);  // the directory's whole subtree leaves the cache with it
  std::unique_lock lock(tree_mu_);
  (void)tree_.remove_path(path, mode());
  note_mutation(path, *gen);
  return {};
}

Result<void> RemoteRoot::rename(std::string_view from, std::string_view to, std::chrono::milliseconds timeout) {
  std::vector<std::byte> pl;
  proto::Writer w(pl);
  proto::write_rename_request(w, proto::RenameRequest{.from = from, .to = to});
  auto p = request(proto::MsgType::RenameRequest, pl, timeout);
  if (!p) return fail(p.error());
  if ((*p)->type != proto::MsgType::Ok) return fail(Errc::ProtocolError);
  proto::Reader r((*p)->payload);
  auto gen = take_ack(r);
  if (!gen) return fail(gen.error());
  drop_cached(from);
  drop_cached(to);
  const std::string nfrom = normalize_path(from);
  const std::string nto = normalize_path(to);
  if (nfrom == nto) return {};  // the OS treated it as a no-op; so does the mirror
  // Moving a directory moves its whole subtree, and the content cache is keyed
  // by path: every entry under the old prefix is now unreachable, and anything
  // cached under the new one predates the move.
  {
    std::shared_lock probe(tree_mu_);
    if (const auto src = tree_.lookup(nfrom, mode()); src && tree_.node(*src).is_dir()) {
      probe.unlock();
      drop_cached_prefix(nfrom);
      drop_cached_prefix(nto);
    }
  }
  std::unique_lock lock(tree_mu_);
  note_mutation(nfrom, *gen);
  note_mutation(nto, *gen);
  const auto id = tree_.lookup(nfrom, mode());
  // A rename replaces whatever was at the destination - unless the destination
  // resolves to the source itself, which is what a case-only rename (`foo` ->
  // `FOO`) does once paths resolve case-insensitively. Removing it there would
  // delete the very thing being renamed, subtree and all.
  if (const auto victim = tree_.lookup(nto, mode()); victim && victim != id) (void)tree_.remove(*victim);
  if (!id) return {};  // not mirrored (yet); the watcher's invalidation will add it
  // Move the NODE, not just its attributes: a renamed directory keeps its whole
  // subtree, and re-creating it empty at the new path would hide everything in
  // it until the next remount.
  const auto [parent, leaf] = split_parent(nto);
  const auto new_parent = parent.empty() ? Result<NodeId>(tree_.root()) : tree_.ensure_directory_path(parent);
  if (!new_parent) return {};  // nowhere to put it; the watcher's event reconciles
  if (!tree_.rename(*id, *new_parent, leaf)) {
    // Something occupies the destination again. An invalidation describing this
    // very rename can be applied between the checks above and here, and it may
    // create the destination before the agent has expanded its children.
    //
    // The old fallback removed the source instead, which is the wrong trade in
    // both directions: the source node is the one actually holding the subtree
    // that was renamed, so dropping it loses every child until the next rescan,
    // while the destination it left behind may still be empty. Clear the
    // blocker and move the source over it.
    if (const auto blocker = tree_.lookup(nto, mode()); blocker && *blocker != *id) (void)tree_.remove(*blocker);
    // If even that does not take, leave the source alone: showing the tree
    // under its old name until the watcher corrects us beats deleting it.
    (void)tree_.rename(*id, *new_parent, leaf);
  }
  return {};
}

Result<std::shared_ptr<RemoteRoot::Pending>> RemoteRoot::request(proto::MsgType type, std::span<const std::byte> payload,
                                                                std::chrono::milliseconds timeout, bool streaming) {
  if (closed_) return fail(Errc::ConnectionClosed);
  const std::uint64_t id = next_request_.fetch_add(1);
  auto pending = std::make_shared<Pending>();
  pending->streaming = streaming;
  {
    std::lock_guard lock(pending_mu_);
    pending_.emplace(id, pending);
  }
  if (auto r = ch_->send(type, id, payload); !r) {
    std::lock_guard lock(pending_mu_);
    pending_.erase(id);
    return fail(r.error());
  }
  std::unique_lock lock(pending->mu);
  if (!pending->cv.wait_for(lock, timeout, [&] { return pending->done; })) {
    std::lock_guard plock(pending_mu_);
    pending_.erase(id);
    return fail(Errc::Timeout);
  }
  if (pending->type == proto::MsgType::Error) {
    proto::Reader r(pending->payload);
    auto e = proto::read_error(r);
    if (e && e->code != 0 && e->code <= static_cast<std::uint32_t>(kLastErrc))
      return fail(static_cast<Errc>(e->code));
    return fail(Errc::ProtocolError);
  }
  if (pending->payload.empty() && pending->type == proto::MsgType::Ping) return fail(Errc::ConnectionClosed);
  return pending;
}

void RemoteRoot::reader_loop() {
  while (!closed_) {
    auto f = ch_->receive();
    if (!f) break;
    if (f->header.type == proto::MsgType::Invalidation) {
      apply_invalidation(f->payload);
      continue;
    }
    const bool streamed_chunk = f->header.type == proto::MsgType::Snapshot;
    std::shared_ptr<Pending> pending;
    {
      std::lock_guard lock(pending_mu_);
      const auto it = pending_.find(f->header.request_id);
      if (it == pending_.end()) continue;  // late reply to a timed-out request
      pending = it->second;
      // A streaming snapshot stays registered until it completes (handled below);
      // every other reply completes on this single frame.
      if (!(pending->streaming && streamed_chunk)) pending_.erase(it);
    }

    if (pending->streaming && streamed_chunk) {
      bool complete = false;
      {
        std::lock_guard lock(pending->mu);
        pending->snap_bytes += f->payload.size();
        proto::Reader r(f->payload);
        // A server that never clears the more-frames flag would otherwise grow
        // these two vectors without limit.
        if (pending->snap_bytes > kMaxSnapshotBytes) {
          pending->stream_ok = false;
        } else if (auto hdr = proto::read_snapshot_header(r)) {
          pending->snap_generation = hdr->generation;
          for (std::uint32_t i = 0; i < hdr->count; ++i) {
            if (pending->snap_entries.size() >= kMaxSnapshotEntries) {
              pending->stream_ok = false;
              break;
            }
            auto e = proto::read_snapshot_entry(r);
            if (!e) {
              pending->stream_ok = false;
              break;
            }
            pending->snap_names.emplace_back(e->name);
            pending->snap_entries.push_back(SnapshotEntry{e->parent, {}, e->attr});
          }
        } else {
          pending->stream_ok = false;
        }
        const bool more = (f->header.flags & proto::kFlagMore) != 0;
        if (!more || !pending->stream_ok) {
          pending->type = proto::MsgType::Snapshot;
          pending->done = true;
          complete = true;
        }
      }
      if (complete) {
        {
          std::lock_guard lock(pending_mu_);
          pending_.erase(f->header.request_id);
        }
        pending->cv.notify_one();
      }
      continue;
    }

    {
      std::lock_guard lock(pending->mu);
      pending->type = f->header.type;
      pending->payload.assign(f->payload.begin(), f->payload.end());
      pending->done = true;
    }
    pending->cv.notify_one();
  }
  closed_ = true;
  fail_all_pending();
}

void RemoteRoot::fail_all_pending() {
  std::unordered_map<std::uint64_t, std::shared_ptr<Pending>> pending;
  {
    std::lock_guard lock(pending_mu_);
    pending.swap(pending_);
  }
  for (auto& [id, p] : pending) {
    std::lock_guard lock(p->mu);
    p->type = proto::MsgType::Ping;  // marker: no payload + Ping type == connection lost
    p->payload.clear();
    p->done = true;
    p->cv.notify_all();
  }
}

void RemoteRoot::apply_invalidation(std::span<const std::byte> payload) {
  proto::Reader r(payload);
  auto batch = proto::read_invalidation(r);
  if (!batch) return;
  bool rescan = false;
  // Directories the batch removes. Collected while the tree can still say they
  // were directories, and swept out of the content cache once the lock is free.
  std::vector<std::string> removed_dirs;
  // What this batch actually changed. An op discarded as stale relative to
  // this client's own mutation did not change the mirror, so it must not be
  // reported as invalidated either - a consumer that drops caches for it would
  // be acting on an event we already decided was obsolete.
  std::vector<AppliedChange> applied;
  {
    std::unique_lock lock(tree_mu_);
    for (const proto::InvalidationOp& op : batch->ops) {
      // An op for a path this client mutated is applied only if the batch is
      // newer than the mutation's acknowledgement; otherwise the agent resolved
      // it before (or while) the mutation happened and it would undo the
      // mirror's own, correct, update - e.g. resurrect a file just renamed away.
      if (op.kind != InvalidationKind::Rescan) {
        if (const auto it = local_mutations_.find(op.path); it != local_mutations_.end()) {
          if (batch->generation <= it->second) continue;  // stale relative to our mutation
          local_mutations_.erase(it);                      // caught up; back to normal
        }
      }
      switch (op.kind) {
        case InvalidationKind::Upsert: {
          // Created or modified is decided here, against the mirror, because
          // this is the only side that knows what it held a moment ago. The
          // agent cannot tell the two apart: both look like "the path exists
          // and something touched it" from a stat after the fact.
          const bool existed = tree_.lookup(op.path, mode()).has_value();
          ChangeKind change = op.change;
          if (!is_move(change)) change = existed ? ChangeKind::Modified : ChangeKind::Created;
          applied.push_back(AppliedChange{change, op.path, op.cookie, op.attr.kind, false});
          (void)tree_.upsert_path(op.path, op.attr);
          break;
        }
        case InvalidationKind::Remove: {
          const auto id = tree_.lookup(op.path, mode());
          if (id && tree_.node(*id).is_dir()) removed_dirs.push_back(op.path);
          // Nothing was there to remove. The mirror is already right, and a
          // consumer told a path it never saw has gone away would report a
          // deletion that never happened. The source half of a move is the
          // exception: its partner's arrival is real either way, and dropping
          // half of a pair costs the consumer the move.
          if (id || op.change == ChangeKind::MovedFrom) {
            const NodeKind kind = id ? tree_.node(*id).attr.kind : NodeKind::File;
            applied.push_back(AppliedChange{op.change == ChangeKind::MovedFrom ? ChangeKind::MovedFrom
                                                                               : ChangeKind::Removed,
                                            op.path, op.cookie, kind, false});
          }
          (void)tree_.remove_path(op.path, mode());
          break;
        }
        case InvalidationKind::Rescan: rescan = true; break;  // handled below, off this thread
      }
    }
    // A snapshot being fetched right now will replace this tree; keep the batch
    // so fetch_snapshot() can replay it if the snapshot pre-dates it. Bounded:
    // a watcher that keeps overflowing would otherwise grow this for as long as
    // the request is in flight. Past the cap the replay can no longer be
    // complete, so the snapshot it feeds is not trustworthy either and
    // fetch_snapshot() asks for another one.
    if (snapshot_in_flight_) {
      if (snapshot_replay_.size() < kMaxSnapshotReplay) snapshot_replay_.push_back(*batch);
      else snapshot_replay_overflow_ = true;
    }
  }
  // Dropping a removal for a path the mirror never had can strand the other
  // half of a move, and so can a frame boundary: a batch too large for one
  // frame is broadcast in several, and the two halves may land in different
  // ones. Either way the surviving half is degraded rather than left dangling.
  repair_move_pairs(applied);
  for (const std::string& d : removed_dirs) drop_cached_prefix(d);
  if (rescan) {
    // The agent's watcher overflowed: per-path events were lost, so the mirror
    // can no longer be trusted. Re-fetch the snapshot on the rescan thread —
    // this is the reader thread, and the request would wait on itself.
    std::lock_guard lock(rs_mu_);
    rs_requested_ = true;
    rs_cv_.notify_one();
  }
  // Drop cached contents (and link targets) for any changed path so the next
  // read refetches. A Rescan means anything may have changed: drop every target.
  {
    std::lock_guard lock(rcache_mu_);
    for (const proto::InvalidationOp& op : batch->ops) {
      if (const auto it = rcache_.find(op.path); it != rcache_.end()) cache_erase(it);
      if (const auto it = link_cache_.find(op.path); it != link_cache_.end()) link_cache_.erase(it);
    }
    if (rescan) link_cache_.clear();
  }
  // Let a changed directory be re-prefetched on the next read there.
  {
    std::lock_guard lock(pf_mu_);
    for (const proto::InvalidationOp& op : batch->ops) pf_seen_.erase(std::string(split_parent(op.path).first));
  }
  InvalidationHook hook;
  InvalidatedPathsHook paths_hook;
  {
    std::lock_guard lock(stats_mu_);
    ++stats_.invalidation_batches;
    stats_.invalidation_ops += batch->ops.size();
    stats_.generation = batch->generation;
    hook = hook_;
    paths_hook = paths_hook_;
  }
  if (paths_hook && !applied.empty()) paths_hook(applied);
  if (hook) hook(*batch);
}

}  // namespace wsld::agent
