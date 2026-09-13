#include "agent/server.hpp"

#include "agent/scanner.hpp"
#include "core/auth_token.hpp"
#include "core/path.hpp"
#include "core/version.hpp"
#ifdef _WIN32
#include "platform/win/wide.hpp"
#endif

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <system_error>
#include <thread>
#include <unordered_set>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace wsld::agent {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {
}  // namespace

RootServer::RootServer(Options opts) : opts_(std::move(opts)), coalescer_(opts_.coalescer) {
  // Load .wsldriveignore from the served root, if present.
  std::error_code ec;
  const fs::path ignore_file = opts_.root / ".wsldriveignore";
  if (fs::is_regular_file(ignore_file, ec)) {
    if (std::FILE* fp =
#ifdef _WIN32
            _wfopen(ignore_file.c_str(), L"rb")
#else
            std::fopen(ignore_file.c_str(), "rb")
#endif
    ) {
      std::string text;
      char buf[4096];
      std::size_t n;
      while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0) text.append(buf, n);
      std::fclose(fp);
      ignore_ = IgnoreRules::parse(text);
    }
  }
}

RootServer::~RootServer() { stop(); }

Result<void> RootServer::start() {
  if (opts_.watch) {
    auto w = platform::make_watcher(opts_.root, [this](const FsEvent& ev) { on_event(ev); });
    if (!w && w.error() != Errc::Unsupported) return fail(w.error());
    if (w) watcher_ = std::move(*w);
  }
  // The flusher runs even without a platform watcher, so events fed through
  // notify() still reach the peers; parked on a condition variable it costs
  // nothing.
  flusher_ = std::thread([this] { flush_loop(); });
  return {};
}

void RootServer::stop() {
  if (stopping_.exchange(true)) return;
  if (watcher_) watcher_->stop();
  {
    std::lock_guard lock(coalescer_mu_);
    coalescer_cv_.notify_all();
  }
  // Shut the peers down first, then join. A flusher parked in a send to a peer
  // that stopped reading would otherwise be joined while it is still blocked;
  // shutdown() fails that send and releases it. This also unblocks every
  // session thread parked in receive(), so serve() returns.
  {
    std::lock_guard lock(peers_mu_);
    for (net::FrameChannel* peer : peers_) peer->shutdown();
  }
  if (flusher_.joinable()) flusher_.join();
}

void RootServer::on_event(const FsEvent& ev) {
  std::lock_guard lock(coalescer_mu_);
  coalescer_.push(ev, Coalescer::clock::now());
  coalescer_cv_.notify_one();
}

void RootServer::flush_loop() {
  std::unique_lock lock(coalescer_mu_);
  while (!stopping_) {
    if (coalescer_.empty()) {
      coalescer_cv_.wait(lock, [&] { return stopping_ || !coalescer_.empty(); });
      continue;
    }
    const auto now = Coalescer::clock::now();
    if (!coalescer_.ready(now)) {
      coalescer_cv_.wait_until(lock, *coalescer_.deadline());
      continue;
    }
    std::vector<PlannedOp> planned = coalescer_.take();
    lock.unlock();

    // Resolve attributes outside the lock: one stat per touched path.
    proto::InvalidationBatch batch;
    batch.generation = generation_.fetch_add(1) + 1;
    batch.ops.reserve(planned.size());
    std::vector<std::string> expand_dirs;
    for (PlannedOp& op : planned) {
      // Ignored paths are absent from the client's tree; skip their events. (Kind
      // does not tell us dir vs file for removes, so test both interpretations.)
      if (!ignore_.empty() && op.kind != InvalidationKind::Rescan &&
          (ignore_.ignored(op.path, /*is_dir=*/false) || ignore_.ignored(op.path, /*is_dir=*/true)))
        continue;
      proto::InvalidationOp out;
      out.kind = op.kind;
      out.path = std::move(op.path);
      out.change = op.change;
      out.cookie = op.cookie;
      if (out.kind != InvalidationKind::Rescan) {
        // Both kinds are resolved against the disk NOW, not against the event.
        // A Remove that is not re-checked is a real hazard: git renames
        // config.lock away and immediately creates a new config.lock; the
        // watcher's Remove for the old one arrives late and would delete the
        // NEW one from every mirror, and the chmod git issues next fails with
        // ENOENT ("could not set core.filemode") - seen in CI.
        auto attr = read_attributes(join_relative(opts_.root, out.path));
        if (!attr) {
          out.kind = InvalidationKind::Remove;  // gone (or vanished between the event and now)
          // The disk overrules the event for the change kind too. Gone means
          // removed, unless the event already said this is the source half of a
          // move — which is the one reading of "gone" that carries more
          // information, so it survives with its cookie.
          if (out.change != ChangeKind::MovedFrom) {
            out.change = ChangeKind::Removed;
            out.cookie = 0;
          }
        } else if (attr->kind == NodeKind::Other) {
          continue;
        } else {
          out.kind = InvalidationKind::Upsert;  // present, whatever the event said
          out.attr = *attr;
          // Present, so it is not gone and not the source of a move. Say only
          // what is still true: something changed here. A peer that tracks
          // change notifications decides between "created" and "modified" from
          // whether its own mirror already had the path — it is the only side
          // that knows, and it has to make that call regardless.
          if (out.change == ChangeKind::Removed || out.change == ChangeKind::MovedFrom) {
            out.change = ChangeKind::Modified;
            out.cookie = 0;
          }
        }
      }
      // A directory that has just appeared (mkdir, or - the case that matters -
      // a rename) is reported by the watcher as one entry, yet a renamed one
      // brings its whole subtree along. Without this the peers would show it
      // empty until the next remount.
      if (op.appeared && out.kind == InvalidationKind::Upsert && out.attr.kind == NodeKind::Directory)
        expand_dirs.push_back(out.path);
      batch.ops.push_back(std::move(out));
    }
    if (!expand_dirs.empty()) append_subtrees(std::move(expand_dirs), batch.ops);
    // Re-stating every path above can reclassify one half of a move (its source
    // exists again, or its destination is already gone) and leave the other
    // half pointing at a partner this batch no longer contains.
    repair_move_pairs(batch.ops);
    if (!batch.ops.empty()) broadcast_batch(batch);
    lock.lock();
  }
}

void RootServer::append_subtrees(std::vector<std::string> dirs, std::vector<proto::InvalidationOp>& ops) {
  // A `cp -r` reports every new directory, nested ones included; enumerate only
  // the topmost of each nest (the scan below descends anyway) and never repeat a
  // path that already has its own op in this batch.
  std::sort(dirs.begin(), dirs.end());
  std::unordered_set<std::string_view> present;
  for (const proto::InvalidationOp& o : ops) present.insert(o.path);
  const std::size_t before = ops.size();
  std::size_t appended = 0;
  bool over = false;
  std::string last_root;
  for (const std::string& rel : dirs) {
    if (!last_root.empty() && wsld::path_is_under(std::string_view(rel), std::string_view(last_root)))
      continue;  // covered by its ancestor's scan
    last_root = rel;
    // scan_tree() hands out entries with a parent INDEX (0 = the scanned root, k
    // = the k-th entry), so keep every entry's path by index.
    std::vector<std::string> rels{rel};
    const std::string prefix = rel + "/";
    (void)scan_tree(
        join_relative(opts_.root, rel),
        [&](const SnapshotEntry& e) {
          std::string p = rels[e.parent] + "/" + std::string(e.name);
          if (++appended > opts_.max_expanded_entries) over = true;
          // The whole subtree is arriving at once, so every entry in it is new
          // to the peer even though no watcher event named it.
          if (!over && !present.contains(p))
            ops.push_back(proto::InvalidationOp{InvalidationKind::Upsert, p, e.attr, ChangeKind::Created, 0});
          rels.push_back(std::move(p));
        },
        // Once over budget, decline every subdirectory so the scan winds down
        // quickly. Ignore rules are relative to the served root, not the subtree.
        [&](std::string_view sub, bool is_dir) {
          if (over) return true;
          return !ignore_.empty() && ignore_.ignored(prefix + std::string(sub), is_dir);
        },
        opts_.one_file_system);
    if (over) break;
  }
  if (over) {
    // Too much to describe entry by entry: drop the partial expansion and have
    // the peers take a fresh snapshot instead - the same path the watcher's own
    // overflow takes, and bounded on both ends.
    ops.resize(before);
    ops.push_back(proto::InvalidationOp{InvalidationKind::Rescan, std::string{}, {}});
  }
}

void RootServer::broadcast_batch(const proto::InvalidationBatch& batch) {
  // Every frame is a complete InvalidationBatch (same generation); the client
  // applies them in order, so splitting is invisible to it. Chunk by the
  // encoded size so one big rename cannot produce a frame above kMaxPayload.
  const std::size_t budget = std::min<std::size_t>(opts_.snapshot_chunk_bytes, proto::kMaxPayload / 2);
  std::vector<std::byte> frame;
  std::size_t first = 0;
  while (first < batch.ops.size()) {
    proto::InvalidationBatch chunk;
    chunk.generation = batch.generation;
    std::size_t bytes = 0;
    std::size_t last = first;
    for (; last < batch.ops.size(); ++last) {
      bytes += batch.ops[last].path.size() + 32;  // kind + length + attrs, generously
      if (bytes > budget && last > first) break;
    }
    chunk.ops.assign(batch.ops.begin() + static_cast<std::ptrdiff_t>(first),
                     batch.ops.begin() + static_cast<std::ptrdiff_t>(last));
    frame.clear();
    proto::write_frame(frame, proto::MsgType::Invalidation, 0, [&](proto::Writer& w) { proto::write_invalidation(w, chunk); });
    broadcast(frame);
    first = last;
  }
}

bool RootServer::add_peer(net::FrameChannel& ch) {
  std::lock_guard lock(peers_mu_);
  if (stopping_) return false;
  ch.set_send_timeout(opts_.broadcast_timeout);  // see Options::broadcast_timeout
  peers_.push_back(&ch);
  return true;
}

void RootServer::broadcast(const std::vector<std::byte>& frame) {
  // Copy the peer list, then send outside the lock: holding peers_mu_ across
  // socket writes let one slow or stuck peer stall invalidation delivery to
  // everyone and block new sessions registering in serve(). Each channel
  // serialises its own writes, so per-peer sends stay safe.
  std::vector<net::FrameChannel*> targets;
  {
    std::lock_guard lock(peers_mu_);
    targets = peers_;
    ++broadcasts_in_flight_;  // keeps a departing session from destroying a channel mid-send
  }
  // A peer that has stopped draining its socket must not hold this thread: the
  // send is bounded (SO_SNDTIMEO, set in add_peer), and one that runs out of
  // time gets shut down. That unblocks its session thread in receive(), which
  // ends the session and deregisters it - this thread cannot remove it here,
  // because the session owns the channel.
  for (net::FrameChannel* peer : targets)
    if (auto r = peer->send_raw(frame); !r && r.error() == Errc::Timeout) peer->shutdown();
  {
    std::lock_guard lock(peers_mu_);
    if (--broadcasts_in_flight_ == 0) peers_cv_.notify_all();
  }
}

void RootServer::serve(net::FrameChannel& ch) {
  // Per-session authentication state. With no token configured the operator has
  // explicitly opted out (--insecure-no-auth), so the session starts open.
  bool authed = opts_.token.empty();
  // Joining the broadcast set is itself a privilege: invalidation frames carry
  // the path and attributes of everything changing under the served root, so an
  // unauthenticated peer that simply connects and stays silent must not receive
  // them. Register only once the session is authenticated (immediately, when the
  // operator opted out of authentication entirely).
  bool registered = false;
  Handshake hs;
  {
    std::lock_guard lock(peers_mu_);
    if (stopping_) return;
  }
  if (authed && !(registered = add_peer(ch))) return;

  // A silent peer never trips the gate in handle(), so it is dropped on a clock
  // instead: it must have authenticated by this deadline or the session ends.
  // Until then the channel also refuses anything but a small frame, and a frame
  // that starts arriving must finish within the same window - a Hello is a few
  // hundred bytes, and a peer that sends one byte and stops, or a header
  // claiming 64 MiB and nothing more, must not hold this thread or that memory.
  const auto handshake_deadline = std::chrono::steady_clock::now() + opts_.handshake_timeout;
  constexpr std::uint32_t kPreAuthMaxPayload = 4096;
  if (!authed) ch.set_receive_limits(kPreAuthMaxPayload, opts_.handshake_timeout);

  for (;;) {
    auto f = ch.receive(std::chrono::milliseconds(200));
    if (!f) {
      if (f.error() == Errc::Timeout) {
        // Either nothing arrived in this poll, or a frame started and did not
        // finish within the handshake window (only possible while unauthed).
        if (stopping_) break;
        if (!authed && std::chrono::steady_clock::now() >= handshake_deadline) {
          (void)send_error(0, Errc::Timeout, "handshake timeout", ch);
          break;
        }
        continue;
      }
      break;  // peer disconnected, oversized pre-auth frame, or I/O error
    }
    const bool was_authed = authed;
    if (auto r = handle(*f, ch, authed, hs); !r) break;
    // A successful Hello promotes the session: full-size frames from here on,
    // and only then does it start receiving invalidations.
    if (authed && !was_authed) ch.set_receive_limits(proto::kMaxPayload, std::chrono::milliseconds(0));
    if (authed && !registered && !(registered = add_peer(ch))) break;
  }
  {
    // Deregister, then wait for any in-flight broadcast to finish: it holds a
    // raw pointer to this channel, which the caller destroys once serve()
    // returns.
    std::unique_lock lock(peers_mu_);
    peers_.erase(std::remove(peers_.begin(), peers_.end(), &ch), peers_.end());
    peers_cv_.wait(lock, [this] { return broadcasts_in_flight_ == 0; });
  }
}

Result<void> RootServer::handle(const net::Frame& f, net::FrameChannel& ch, bool& authed, Handshake& hs) {
  // Nothing is served before the peer has authenticated. Checking the token only
  // inside the Hello handler is not enough on its own: a peer that never sends a
  // Hello would otherwise reach the read and mutation handlers with its first
  // frame, which defeats the token entirely.
  if (f.header.type != proto::MsgType::Hello && f.header.type != proto::MsgType::Auth && !authed) {
    (void)send_error(f.header.request_id, Errc::Unauthorized, "authenticate first", ch);
    return fail(Errc::Unauthorized);  // drops the session
  }
  switch (f.header.type) {
    case proto::MsgType::Hello: {
      proto::Reader r(f.payload);
      auto hello = proto::read_hello(r);
      if (!hello) return fail(hello.error());
      if (hello->protocol_version != proto::kVersion) {
        (void)send_error(f.header.request_id, Errc::UnsupportedVersion, "protocol version mismatch", ch);
        return fail(Errc::UnsupportedVersion);
      }
      // Answer with our own nonce and a proof the client can check BEFORE it
      // sends anything of its own. That is the half that was missing: the client
      // used to hand the shared secret to whoever accepted on the endpoint, so
      // anything that bound the port first got the secret and could serve a
      // tree of its own choosing.
      //
      // An empty configured token means the operator opted out explicitly
      // (--insecure-no-auth); the session is open from here and the proof is
      // empty, which a client holding a real token will correctly refuse.
      hs.client_nonce = std::string(hello->nonce);
      if (opts_.token.empty()) {
        authed = true;
        return ch.send_with(proto::MsgType::HelloAck, f.header.request_id, [&](proto::Writer& w) {
          proto::write_hello(w, proto::Hello{.protocol_version = proto::kVersion,
                                             .capabilities = watching() ? 1u : 0u,
                                             .agent = agent_string("wsldrived"), .nonce = {}, .proof = {}});
        });
      }
      if (hs.client_nonce.empty()) {  // a peer that skipped its nonce cannot be authenticated
        (void)send_error(f.header.request_id, Errc::Unauthorized, "authentication failed", ch);
        return fail(Errc::Unauthorized);
      }
      hs.server_nonce = generate_auth_nonce();
      if (hs.server_nonce.empty()) {  // no entropy source: refuse rather than authenticate weakly
        (void)send_error(f.header.request_id, Errc::IoError, "no entropy source for the handshake", ch);
        return fail(Errc::IoError);
      }
      const std::string server_proof = auth_proof(opts_.token, hs.client_nonce, hs.server_nonce, true);
      hs.greeted = true;
      return ch.send_with(proto::MsgType::HelloAck, f.header.request_id, [&](proto::Writer& w) {
        proto::write_hello(w, proto::Hello{.protocol_version = proto::kVersion,
                                           .capabilities = watching() ? 1u : 0u,
                                           .agent = agent_string("wsldrived"),
                                           .nonce = hs.server_nonce, .proof = server_proof});
      });
    }
    case proto::MsgType::Auth: {
      // The client has checked our proof and is now proving itself. Nothing is
      // served until this succeeds.
      if (!hs.greeted) {
        (void)send_error(f.header.request_id, Errc::Unauthorized, "Auth before Hello", ch);
        return fail(Errc::Unauthorized);
      }
      proto::Reader r(f.payload);
      auto msg = proto::read_hello(r);
      if (!msg) return fail(msg.error());
      const std::string expected = auth_proof(opts_.token, hs.client_nonce, hs.server_nonce, false);
      if (!constant_time_equal(msg->proof, expected)) {
        (void)send_error(f.header.request_id, Errc::Unauthorized, "authentication failed", ch);
        return fail(Errc::Unauthorized);  // drops the session
      }
      authed = true;  // only now may this session issue anything else
      return ch.send(proto::MsgType::Ok, f.header.request_id, {});
    }
    case proto::MsgType::Ping: return ch.send(proto::MsgType::Pong, f.header.request_id, {});
    case proto::MsgType::SnapshotRequest: return send_snapshot(f.header.request_id, ch);
    case proto::MsgType::ReadRequest: return send_read(f, ch);
    case proto::MsgType::ReadManyRequest: return send_read_many(f, ch);
    case proto::MsgType::ReadlinkRequest: return send_readlink(f, ch);
    case proto::MsgType::WriteRequest:
    case proto::MsgType::CreateRequest:
    case proto::MsgType::MkdirRequest:
    case proto::MsgType::UnlinkRequest:
    case proto::MsgType::RmdirRequest:
    case proto::MsgType::RenameRequest:
    case proto::MsgType::TruncateRequest:
      return handle_mutation(f, ch);
    default: return send_error(f.header.request_id, Errc::ProtocolError, "unexpected message type", ch);
  }
}

namespace {

// True when `p` is lexically inside `root`. Compared component-wise on already
// normalised paths, so it never touches the filesystem (no TOCTOU, no cost).
bool path_is_under(const std::filesystem::path& root, const std::filesystem::path& p) {
  const auto r = root.lexically_normal();
  const auto q = p.lexically_normal();
  auto ri = r.begin();
  auto qi = q.begin();
  for (; ri != r.end(); ++ri, ++qi) {
    if (ri->empty()) continue;  // trailing separator yields an empty component
    if (qi == q.end() || *qi != *ri) return false;
  }
  return true;
}

// A peer-supplied path must stay inside the served root. Rejecting the ".."
// component (not the substring, so `foo..bar` stays legal) is not enough on
// Windows: `C:/Windows/...` has no ".." at all, yet fs::path::operator/ REPLACES
// the left side when the right has a root name, so the join would escape the
// root entirely. Reject anything that is not a pure relative path.
bool is_contained_relative(std::string_view norm) {
  if (norm.empty()) return true;  // the root itself
  std::size_t i = 0;
  while (i <= norm.size()) {
    const std::size_t j = norm.find('/', i);
    const std::string_view comp = norm.substr(i, (j == std::string_view::npos ? norm.size() : j) - i);
    if (comp == "..") return false;
    // A component carrying a root name (drive letter, and on Windows also an
    // alternate data stream) would re-anchor the join.
    if (comp.find(':') != std::string_view::npos) return false;
#ifdef _WIN32
    // Reserved DOS device names resolve to devices, not files, whatever
    // directory they appear in. Only on Windows: these are legal filenames on
    // Linux, and rejecting them there would deny access to real files.
    {
      // Win32 strips trailing dots and spaces before resolving, so "CON " is
      // still the console device; trim them before comparing.
      std::string_view trimmed = comp;
      while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '.')) trimmed.remove_suffix(1);
      std::string stem(trimmed.substr(0, trimmed.find('.')));
      for (char& c : stem) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      static constexpr std::string_view kReserved[] = {"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4",
                                                       "COM5", "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3",
                                                       "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
      for (const std::string_view r : kReserved)
        if (stem == r) return false;
    }
#endif
    if (j == std::string_view::npos) break;
    i = j + 1;
  }
  return true;
}

}  // namespace

Result<std::filesystem::path> RootServer::resolve(std::string_view rel) const {
  const std::string norm = normalize_path(rel);
  if (!is_contained_relative(norm)) return fail(Errc::InvalidPath);
  std::filesystem::path full = join_relative(opts_.root, norm);
  // Defence in depth: whatever the join produced must still be under the root.
  if (!path_is_under(opts_.root, full)) return fail(Errc::InvalidPath);
  return full;
}

namespace {
Errc map_fs_error(const std::error_code& ec) {
  // Compare against std::errc conditions (not error_codes): this uses the
  // category's equivalence, so it works whether ec is generic (POSIX) or
  // system_category (Win32) as std::filesystem produces on Windows.
  if (ec == std::errc::no_such_file_or_directory) return Errc::NotFound;
  if (ec == std::errc::file_exists) return Errc::AlreadyExists;
  if (ec == std::errc::not_a_directory) return Errc::NotADirectory;
  if (ec == std::errc::is_a_directory) return Errc::IsADirectory;
  if (ec == std::errc::directory_not_empty) return Errc::AlreadyExists;
  if (ec == std::errc::invalid_argument) return Errc::InvalidArgument;
  return Errc::IoError;
}

// On Windows a file that was just written is often held open for a moment by
// software that is not the writer - the search indexer, an antivirus scanner -
// without FILE_SHARE_DELETE. A rename, delete or truncate arriving right behind
// the write then fails with a sharing violation, which is exactly the shape of
// git's lock-file dance (write config.lock, rename over config): git init aborted
// mid-way on the mount about one run in three. Git for Windows retries this
// itself; so does the agent, for about a quarter of a second (1+2+...+128 ms).
//
// Which errors are worth waiting out: a sharing/lock violation always is. Windows
// reports a rename OVER a held-open file as ERROR_ACCESS_DENIED too, so that
// code cannot simply be excluded - but it is also the permanent answer for a
// read-only file (git marks its pack files read-only) or a directory, and
// retrying those would freeze the whole mount for the duration on every such
// file. The attribute check tells the two apart.
constexpr int kShareRetryMaxDelayMs = 128;

#ifdef _WIN32
bool is_readonly_or_dir(const std::filesystem::path& p) noexcept {
  const DWORD a = ::GetFileAttributesW(p.c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_DIRECTORY)) != 0;
}
bool transient_share_error(DWORD err, const std::filesystem::path& target) noexcept {
  if (err == ERROR_SHARING_VIOLATION || err == ERROR_LOCK_VIOLATION) return true;
  return err == ERROR_ACCESS_DENIED && !is_readonly_or_dir(target);
}
#endif

bool is_transient_share_error(const std::error_code& ec, const std::filesystem::path& target) noexcept {
#ifdef _WIN32
  return ec.category() == std::system_category() && transient_share_error(static_cast<DWORD>(ec.value()), target);
#else
  (void)ec;
  (void)target;
  return false;
#endif
}

// `target` is the file whose state decides whether waiting can help (for a
// rename, the destination being replaced).
template <class Op>
std::error_code with_share_retry(const std::filesystem::path& target, Op&& op) {
  std::error_code ec = op();
  for (int delay_ms = 1; ec && is_transient_share_error(ec, target) && delay_ms <= kShareRetryMaxDelayMs;
       delay_ms *= 2) {
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    ec = op();
  }
  return ec;
}

// fopen() folds every Win32 failure into errno EACCES; the OS error is still in
// _doserrno, so the same distinction applies.
bool fopen_failed_transiently(const std::filesystem::path& p) noexcept {
#ifdef _WIN32
  return errno == EACCES && transient_share_error(static_cast<DWORD>(_doserrno), p);
#else
  (void)p;
  return false;
#endif
}

// Seeks to an absolute byte offset, reporting whether it worked. The platform
// calls take a signed offset, so anything past the signed range wraps negative
// and the seek fails - and left unchecked, the stream simply stays where it was
// (position 0 on a fresh handle), so the caller reads or writes the wrong part
// of the file rather than getting an error. Callers bound the offset first;
// this is the check that makes a bound failure loud.
bool seek_to(std::FILE* fp, std::uint64_t offset) noexcept {
  if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return false;
#ifdef _WIN32
  return _fseeki64(fp, static_cast<long long>(offset), SEEK_SET) == 0;
#else
  return fseeko(fp, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}

std::FILE* fopen_with_retry(const std::filesystem::path& p, const char* mode) {

  std::FILE* fp = nullptr;
  for (int delay_ms = 1;; delay_ms *= 2) {
#ifdef _WIN32
    const std::wstring wmode(mode, mode + std::strlen(mode));
    fp = _wfopen(p.c_str(), wmode.c_str());
#else
    fp = std::fopen(p.c_str(), mode);
#endif
    if (fp != nullptr || !fopen_failed_transiently(p) || delay_ms > kShareRetryMaxDelayMs) return fp;
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
  }
}

// Failures the peer cannot reason about (not "missing", not "exists") are worth
// a line here: they are what an operator sees when a tool gives up on the mount.
void log_io_failure(const char* op, std::string_view path, const std::error_code& ec) {
  if (map_fs_error(ec) == Errc::IoError)
    std::fprintf(stderr, "wsldrived: %s '%.*s' failed: %s (%d)\n", op, static_cast<int>(path.size()), path.data(),
                 ec.message().c_str(), ec.value());
}
}  // namespace

Result<void> RootServer::handle_mutation(const net::Frame& f, net::FrameChannel& ch) {
  namespace fs = std::filesystem;
  const std::uint64_t id = f.header.request_id;
  proto::Reader r(f.payload);
  auto reject = [&](Errc e, std::string_view d) { return send_error(id, e, d, ch); };
  // Every acknowledgement carries the generation as of now - i.e. after the
  // filesystem change - so the client can discard an invalidation resolved
  // before its mutation (see proto::MutationAck).
  auto ack = [&](proto::Writer& w) { proto::write_mutation_ack(w, proto::MutationAck{generation_.load()}); };
  auto send_ok = [&] { return ch.send_with(proto::MsgType::Ok, id, ack); };

  switch (f.header.type) {
    case proto::MsgType::CreateRequest: {
      auto q = proto::read_create_request(r);
      if (!q) return fail(q.error());
      auto full = resolve(q->path);
      if (!full) return reject(full.error(), q->path);
      {
        std::FILE* fp = fopen_with_retry(*full, "wb");
        if (fp == nullptr) return reject(Errc::IoError, q->path);
        std::fclose(fp);
      }
      auto attr = read_attributes(*full);
      if (!attr) return reject(attr.error(), q->path);
      return ch.send_with(proto::MsgType::CreateResponse, id, [&](proto::Writer& w) {
        proto::write_attributes(w, *attr);
        ack(w);
      });
    }
    case proto::MsgType::WriteRequest: {
      auto q = proto::read_write_request(r);
      if (!q) return fail(q.error());
      auto full = resolve(q->path);
      if (!full) return reject(full.error(), q->path);
      // An offset past the ceiling would wrap negative in the seek call below,
      // which then fails - and an unchecked failure leaves the stream at 0, so
      // the write silently lands at the start of the file instead.
      if (q->offset > proto::kMaxFileOffset) return reject(Errc::InvalidArgument, q->path);
      std::FILE* fp = fopen_with_retry(*full, "r+b");
      if (fp == nullptr && errno == ENOENT) fp = fopen_with_retry(*full, "w+b");
      if (fp == nullptr) return reject(Errc::IoError, q->path);
      if (!seek_to(fp, q->offset)) {
        std::fclose(fp);
        return reject(Errc::IoError, q->path);
      }
      const std::size_t n = q->data.empty() ? 0 : std::fwrite(q->data.data(), 1, q->data.size(), fp);
      std::fclose(fp);
      if (n != q->data.size()) return reject(Errc::IoError, q->path);
      return ch.send_with(proto::MsgType::WriteResponse, id, [&](proto::Writer& w) {
        proto::write_write_response(w, proto::WriteResponse{n, generation_.load()});
      });
    }
    case proto::MsgType::TruncateRequest: {
      auto q = proto::read_truncate_request(r);
      if (!q) return fail(q.error());
      auto full = resolve(q->path);
      if (!full) return reject(full.error(), q->path);
      // Without a bound this happily asks the filesystem for an 8 EiB sparse
      // file.
      if (q->size > proto::kMaxFileOffset) return reject(Errc::InvalidArgument, q->path);
      const std::error_code ec = with_share_retry(*full, [&] {
        std::error_code e;
        fs::resize_file(*full, q->size, e);
        return e;
      });
      if (ec) {
        log_io_failure("truncate", q->path, ec);
        return reject(map_fs_error(ec), q->path);
      }
      return send_ok();
    }
    case proto::MsgType::MkdirRequest: {
      auto q = proto::read_mkdir_request(r);
      if (!q) return fail(q.error());
      auto full = resolve(q->path);
      if (!full) return reject(full.error(), q->path);
      std::error_code ec;
      if (!fs::create_directory(*full, ec) || ec) return reject(ec ? map_fs_error(ec) : Errc::AlreadyExists, q->path);
      return send_ok();
    }
    case proto::MsgType::UnlinkRequest: {
      auto q = proto::read_path_request(r);
      if (!q) return fail(q.error());
      auto full = resolve(q->path);
      if (!full) return reject(full.error(), q->path);
      std::error_code ec;
      if (fs::is_directory(*full, ec)) return reject(Errc::IsADirectory, q->path);
      bool removed = false;
      ec = with_share_retry(*full, [&] {
        std::error_code e;
        removed = fs::remove(*full, e);
        return e;
      });
      if (ec) {
        log_io_failure("unlink", q->path, ec);
        return reject(map_fs_error(ec), q->path);
      }
      if (!removed) return reject(Errc::NotFound, q->path);
      return send_ok();
    }
    case proto::MsgType::RmdirRequest: {
      auto q = proto::read_path_request(r);
      if (!q) return fail(q.error());
      auto full = resolve(q->path);
      if (!full) return reject(full.error(), q->path);
      std::error_code ec;
      if (!fs::is_directory(*full, ec)) return reject(Errc::NotADirectory, q->path);
      if (!fs::remove(*full, ec) || ec) return reject(ec ? map_fs_error(ec) : Errc::NotFound, q->path);
      return send_ok();
    }
    case proto::MsgType::RenameRequest: {
      auto q = proto::read_rename_request(r);
      if (!q) return fail(q.error());
      auto from = resolve(q->from);
      auto to = resolve(q->to);
      if (!from) return reject(from.error(), q->from);
      if (!to) return reject(to.error(), q->to);
      const std::error_code ec = with_share_retry(*to, [&] {
        std::error_code e;
        fs::rename(*from, *to, e);
        return e;
      });
      if (ec) {
        log_io_failure("rename", q->to, ec);
        return reject(map_fs_error(ec), q->to);
      }
      return send_ok();
    }
    default:
      return reject(Errc::ProtocolError, "not a mutation");
  }
}

Result<void> RootServer::send_snapshot(std::uint64_t request_id, net::FrameChannel& ch) {
  const std::uint64_t gen = generation_.load();
  const std::size_t budget = opts_.snapshot_chunk_bytes;

  // Each frame is self-describing: SnapshotHeader{gen, entries-in-this-frame}
  // followed by that many entries. The kFlagMore flag is set on every frame but
  // the last, so the client accumulates until it sees a frame without it.
  std::vector<std::byte> entries;  // encoded entries buffered for the current frame
  std::uint32_t count = 0;
  Result<void> send_rc{};

  auto flush = [&](bool more) -> Result<void> {
    std::vector<std::byte> frame;
    proto::write_frame(
        frame, proto::MsgType::Snapshot, request_id,
        [&](proto::Writer& w) {
          proto::write_snapshot_header(w, gen, count);
          w.raw(entries);
        },
        more ? proto::kFlagMore : 0u);
    entries.clear();
    count = 0;
    return ch.send_raw(frame);
  };

  auto scanned = scan_tree(
      opts_.root,
      [&](const SnapshotEntry& e) {
        if (!send_rc) return;  // a previous flush failed; stop buffering
        proto::Writer w(entries);
        proto::write_snapshot_entry(w, e);
        ++count;
        if (entries.size() >= budget) send_rc = flush(/*more=*/true);
      },
      ignore_.empty() ? agent::SkipPredicate{}
                      : [this](std::string_view rel, bool is_dir) { return ignore_.ignored(rel, is_dir); },
      opts_.one_file_system);
  if (!scanned) return send_error(request_id, scanned.error(), "scan failed", ch);
  if (!send_rc) return send_rc;
  return flush(/*more=*/false);  // final frame (possibly empty) terminates the stream
}

Result<void> RootServer::send_read(const net::Frame& f, net::FrameChannel& ch) {
  proto::Reader r(f.payload);
  auto q = proto::read_read_request(r);
  if (!q) return fail(q.error());
  const std::string rel = normalize_path(q->path);
  auto resolved = resolve(rel);  // single gate: rejects .., absolute and escaping paths
  if (!resolved) return send_error(f.header.request_id, Errc::InvalidPath, "path outside the served root", ch);
  const fs::path full = *resolved;

  std::error_code ec;
  const auto size = fs::file_size(full, ec);
  if (ec) return send_error(f.header.request_id, Errc::NotFound, rel, ch);

  const std::uint64_t offset = std::min<std::uint64_t>(q->offset, size);
  const std::uint64_t want = std::min<std::uint64_t>(q->length, size - offset);
  if (want > proto::kMaxPayload - 64) return send_error(f.header.request_id, Errc::TooLarge, "read too large", ch);

  std::vector<std::byte> data(static_cast<std::size_t>(want));
  if (want != 0) {
#ifdef _WIN32
    std::FILE* fp = _wfopen(full.c_str(), L"rb");
#else
    std::FILE* fp = std::fopen(full.c_str(), "rb");
#endif
    if (fp == nullptr) return send_error(f.header.request_id, Errc::IoError, rel, ch);
    if (!seek_to(fp, offset)) {  // unchecked, this would read from 0 and answer with the wrong bytes
      std::fclose(fp);
      return send_error(f.header.request_id, Errc::IoError, rel, ch);
    }
    const std::size_t got = std::fread(data.data(), 1, data.size(), fp);
    std::fclose(fp);
    data.resize(got);
  }
  return ch.send_with(proto::MsgType::ReadResponse, f.header.request_id, [&](proto::Writer& w) {
    proto::write_read_response(w, proto::ReadResponse{.file_size = size, .data = data});
  });
}

Result<void> RootServer::send_read_many(const net::Frame& f, net::FrameChannel& ch) {
  proto::Reader r(f.payload);
  auto q = proto::read_read_many_request(r);
  if (!q) return fail(q.error());

  const std::size_t n = q->paths.size();
  std::vector<std::vector<std::byte>> datas(n);
  std::vector<char> oks(n, 0);
  std::size_t total = 0;
  const std::size_t cap = proto::kMaxPayload - (64u << 10);  // headroom for framing

  for (std::size_t i = 0; i < n; ++i) {
    const std::string rel = normalize_path(q->paths[i]);
    auto resolved = resolve(rel);  // same gate as the single read
    if (!resolved) continue;
    const fs::path full = *resolved;
    std::error_code ec;
    const auto sz = fs::file_size(full, ec);
    if (ec || sz > (16u << 20) || total + sz > cap) continue;  // skip; client fetches individually
    std::FILE* fp =
#ifdef _WIN32
        _wfopen(full.c_str(), L"rb");
#else
        std::fopen(full.c_str(), "rb");
#endif
    if (fp == nullptr) continue;
    std::vector<std::byte> buf(static_cast<std::size_t>(sz));
    const std::size_t got = buf.empty() ? 0 : std::fread(buf.data(), 1, buf.size(), fp);
    std::fclose(fp);
    buf.resize(got);
    total += got;
    datas[i] = std::move(buf);
    oks[i] = 1;
  }

  return ch.send_with(proto::MsgType::ReadManyResponse, f.header.request_id, [&](proto::Writer& w) {
    proto::ReadManyResponse resp;
    resp.items.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
      resp.items[i].ok = oks[i] != 0;
      if (oks[i]) resp.items[i].data = datas[i];
    }
    proto::write_read_many_response(w, resp);
  });
}

Result<void> RootServer::send_readlink(const net::Frame& f, net::FrameChannel& ch) {
  proto::Reader r(f.payload);
  auto q = proto::read_path_request(r);
  if (!q) return fail(q.error());
  auto resolved = resolve(q->path);  // same gate as every other path
  if (!resolved) return send_error(f.header.request_id, Errc::InvalidPath, "path outside the served root", ch);
  std::error_code ec;
  const fs::path target = fs::read_symlink(*resolved, ec);
  if (ec) {
    // EINVAL is "exists but is not a symlink"; everything else is treated as absent.
    return send_error(f.header.request_id,
                      ec == std::errc::invalid_argument ? Errc::InvalidArgument : map_fs_error(ec), q->path, ch);
  }
  // The target is sent as stored, only with the separator the protocol uses
  // everywhere; the mount decides what it means on its side.
#ifdef _WIN32
  std::string text = platform::win::to_utf8(target.native());
  for (char& c : text)
    if (c == '\\') c = '/';
#else
  const std::string& text = target.native();
#endif
  return ch.send_with(proto::MsgType::ReadlinkResponse, f.header.request_id,
                      [&](proto::Writer& w) { proto::write_readlink_response(w, proto::ReadlinkResponse{text}); });
}

Result<void> RootServer::send_error(std::uint64_t request_id, Errc code, std::string_view detail, net::FrameChannel& ch) {
  return ch.send_with(proto::MsgType::Error, request_id, [&](proto::Writer& w) {
    proto::write_error(w, proto::ErrorMessage{.code = static_cast<std::uint32_t>(code), .detail = detail});
  });
}

}  // namespace wsld::agent
