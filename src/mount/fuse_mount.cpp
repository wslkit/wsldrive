#include "mount/fuse_mount.hpp"

#include "core/metadata_tree.hpp"
#include "core/path.hpp"

// One implementation over the FUSE3 high-level API: WinFsp-FUSE on Windows
// (Direction A) and libfuse3 on Linux (Direction B). The API is source-
// compatible apart from a few types (fuse_stat vs stat) aliased below and the
// Windows-only DLL load and filename escaping.
#ifdef _WIN32
#include <fuse3/fuse.h>  // FUSE_USE_VERSION comes from the winfsp::fuse3 target
#include <windows.h>
#include "core/win_names.hpp"
using StatT = struct fuse_stat;
using StatvfsT = struct fuse_statvfs;
using OffT = fuse_off_t;
using ModeT = fuse_mode_t;
using TimespecT = struct fuse_timespec;
#ifndef S_IFLNK
#define S_IFLNK 0120000
#endif
#else
#define FUSE_USE_VERSION 31
#include <fuse3/fuse.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/statvfs.h>
using StatT = struct stat;
using StatvfsT = struct statvfs;
using OffT = off_t;
using ModeT = mode_t;
using TimespecT = struct timespec;
#endif

#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wsld::mount {

namespace {

struct Context {
  agent::RemoteRoot* root;
  bool writeback = false;
};

Context* ctx() { return static_cast<Context*>(fuse_get_context()->private_data); }

// Per-open-file write buffer used in write-back mode. Coalesces a contiguous run
// of writes and flushes it on fsync/flush/release (or when it grows past a cap).
struct WriteHandle {
  std::string path;
  std::uint64_t start = 0;  // offset of buf[0] within the file
  std::vector<std::byte> buf;
};

constexpr std::size_t kWriteBackCap = 8u << 20;

WriteHandle* handle_of(struct fuse_file_info* fi) {
  return fi != nullptr ? reinterpret_cast<WriteHandle*>(static_cast<std::uintptr_t>(fi->fh)) : nullptr;
}

// Flushes a write handle's buffer to the agent. Returns 0 or a negative errno.
int flush_handle(WriteHandle* h);

// Largest number of paths waiting for a kernel page-cache punch. Past this the
// mount stops queueing: `auto_cache` still revalidates each file on its next
// open, so the effect of dropping one is a late invalidation, not a stale read.
constexpr std::size_t kInvalQueueCap = 4096;

// Inverse of to_rel(): a tree key (relative, '/'-separated) as the absolute
// path FUSE uses. '/' is never escaped, so the whole path escapes in one go.
std::string to_fuse_path(std::string_view rel) {
#ifdef _WIN32
  return "/" + escape_for_windows(rel);
#else
  return "/" + std::string(rel);
#endif
}

#ifdef _WIN32
// winfsp-x64.dll is delay-loaded; load it (default search path, then the
// registry InstallDir) so the binary runs without WinFsp's bin dir on PATH.
bool load_winfsp_dll() {
  if (::LoadLibraryW(L"winfsp-x64.dll") != nullptr) return true;
  HKEY key{};
  if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\WinFsp", 0, KEY_READ | KEY_WOW64_32KEY, &key) !=
      ERROR_SUCCESS)
    return false;
  wchar_t dir[MAX_PATH]{};
  DWORD size = sizeof(dir);
  const LSTATUS rc = ::RegQueryValueExW(key, L"InstallDir", nullptr, nullptr, reinterpret_cast<LPBYTE>(dir), &size);
  ::RegCloseKey(key);
  if (rc != ERROR_SUCCESS) return false;
  std::wstring path = dir;
  if (!path.empty() && path.back() != L'\\') path.push_back(L'\\');
  path += L"bin\\winfsp-x64.dll";
  return ::LoadLibraryW(path.c_str()) != nullptr;
}
#endif

// Name shown in a directory listing: on Windows, escape characters illegal in
// NTFS names; on Linux the raw name is already valid.
std::string present_name(std::string_view raw) {
#ifdef _WIN32
  return escape_for_windows(raw);
#else
  return std::string(raw);
#endif
}

// FUSE paths are absolute ("/", "/a/b"); the tree uses relative '/'-separated
// keys. On Windows, decode the escaped form back to the raw name.
std::string to_rel(const char* path) {
  if (path == nullptr) return {};
  while (*path == '/') ++path;
#ifdef _WIN32
  return unescape_from_windows(path);
#else
  return std::string(path);
#endif
}

void fill_stat(const MetadataTree::Node& n, StatT* st) {
  std::memset(st, 0, sizeof(*st));
#ifndef _WIN32
  // Report the mounting user as the owner. Left at 0 the whole tree looks
  // root-owned, and tools that check ownership refuse to touch it — git aborts
  // with "detected dubious ownership" on any repo living on the mount. The
  // backing store (NTFS via the Windows agent) has no POSIX owner to preserve.
  st->st_uid = ::getuid();
  st->st_gid = ::getgid();
#endif
  if (n.attr.kind == NodeKind::Directory) {
    st->st_mode = static_cast<ModeT>(S_IFDIR | 0755);
    st->st_nlink = 2;
  } else if (n.attr.kind == NodeKind::Symlink) {
    st->st_mode = static_cast<ModeT>(S_IFLNK | 0777);
    st->st_nlink = 1;
    st->st_size = static_cast<OffT>(n.attr.size);
  } else {
    st->st_mode = static_cast<ModeT>(S_IFREG | 0644);
    st->st_nlink = 1;
    st->st_size = static_cast<OffT>(n.attr.size);
  }
  st->st_mtim.tv_sec = static_cast<decltype(st->st_mtim.tv_sec)>(n.attr.mtime_ns / 1'000'000'000);
  st->st_mtim.tv_nsec = static_cast<decltype(st->st_mtim.tv_nsec)>(n.attr.mtime_ns % 1'000'000'000);
  st->st_atim = st->st_mtim;
  st->st_ctim = st->st_mtim;
}

int op_getattr(const char* path, StatT* st, struct fuse_file_info*) {
  const std::string rel = to_rel(path);
  return ctx()->root->with_tree([&](const MetadataTree& t) -> int {
    const auto id = t.lookup(rel, LookupMode::CaseInsensitive);
    if (!id) return -ENOENT;
    fill_stat(t.node(*id), st);
    return 0;
  });
}

int op_readdir(const char* path, void* buf, fuse_fill_dir_t filler, OffT, struct fuse_file_info*,
               enum fuse_readdir_flags) {
  const std::string rel = to_rel(path);
  return ctx()->root->with_tree([&](const MetadataTree& t) -> int {
    const auto id = t.lookup(rel, LookupMode::CaseInsensitive);
    if (!id) return -ENOENT;
    if (!t.node(*id).is_dir()) return -ENOTDIR;
    filler(buf, ".", nullptr, 0, static_cast<fuse_fill_dir_flags>(0));
    filler(buf, "..", nullptr, 0, static_cast<fuse_fill_dir_flags>(0));
    int rc = 0;
    t.for_each_child(*id, [&](NodeId c) {
      if (rc != 0) return;
      const std::string name = present_name(t.name(c));
      StatT st;
      fill_stat(t.node(c), &st);
      rc = filler(buf, name.c_str(), &st, 0, static_cast<fuse_fill_dir_flags>(0));
    });
    return 0;
  });
}

int op_open(const char* path, struct fuse_file_info* fi) {
  const std::string rel = to_rel(path);
  const int rc = ctx()->root->with_tree([&](const MetadataTree& t) -> int {
    const auto id = t.lookup(rel, LookupMode::CaseInsensitive);
    if (!id) return -ENOENT;
    if (t.node(*id).is_dir()) return -EISDIR;
    return 0;
  });
  if (rc == 0 && ctx()->writeback && fi != nullptr)
    fi->fh = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(new WriteHandle{rel, 0, {}}));
  return rc;
}

int err_to_errno(Errc e) {
  switch (e) {
    case Errc::NotFound: return -ENOENT;
    case Errc::AlreadyExists: return -EEXIST;
    case Errc::NotADirectory: return -ENOTDIR;
    case Errc::IsADirectory: return -EISDIR;
    case Errc::InvalidPath:
    case Errc::InvalidArgument: return -EINVAL;
    case Errc::Timeout: return -ETIMEDOUT;
    case Errc::ConnectionClosed: return -EIO;
    default: return -EIO;
  }
}

int op_create(const char* path, ModeT mode, struct fuse_file_info* fi) {
  const std::string rel = to_rel(path);
  auto r = ctx()->root->create_file(rel, static_cast<std::uint32_t>(mode) & 0777u);
  if (!r) return err_to_errno(r.error());
  if (ctx()->writeback && fi != nullptr)
    fi->fh = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(new WriteHandle{rel, 0, {}}));
  return 0;
}

int flush_handle(WriteHandle* h) {
  if (h == nullptr || h->buf.empty()) return 0;
  auto r = ctx()->root->write(h->path, h->start, h->buf);
  if (!r) return err_to_errno(r.error());
  h->start += h->buf.size();
  h->buf.clear();
  return 0;
}

int op_write(const char* path, const char* buf, size_t size, OffT offset, struct fuse_file_info* fi) {
  WriteHandle* h = handle_of(fi);
  if (ctx()->writeback && h != nullptr) {
    const auto off = static_cast<std::uint64_t>(offset);
    // Only a write contiguous with the buffer can extend it; otherwise flush and restart.
    if (!h->buf.empty() && off != h->start + h->buf.size()) {
      if (int rc = flush_handle(h); rc != 0) return rc;
    }
    if (h->buf.empty()) h->start = off;
    const auto* p = reinterpret_cast<const std::byte*>(buf);
    h->buf.insert(h->buf.end(), p, p + size);
    if (h->buf.size() >= kWriteBackCap) {
      if (int rc = flush_handle(h); rc != 0) return rc;
    }
    return static_cast<int>(size);
  }
  auto r = ctx()->root->write(to_rel(path), static_cast<std::uint64_t>(offset),
                              std::as_bytes(std::span<const char>(buf, size)));
  if (!r) return err_to_errno(r.error());
  return static_cast<int>(*r);
}

int op_flush(const char*, struct fuse_file_info* fi) { return flush_handle(handle_of(fi)); }

int op_release(const char*, struct fuse_file_info* fi) {
  WriteHandle* h = handle_of(fi);
  const int rc = flush_handle(h);
  delete h;
  if (fi != nullptr) fi->fh = 0;
  return rc;
}

int op_truncate(const char* path, OffT size, struct fuse_file_info*) {
  auto r = ctx()->root->truncate(to_rel(path), static_cast<std::uint64_t>(size));
  return r ? 0 : err_to_errno(r.error());
}

int op_mkdir(const char* path, ModeT mode) {
  auto r = ctx()->root->mkdir(to_rel(path), static_cast<std::uint32_t>(mode) & 0777u);
  return r ? 0 : err_to_errno(r.error());
}

int op_unlink(const char* path) {
  auto r = ctx()->root->unlink(to_rel(path));
  return r ? 0 : err_to_errno(r.error());
}

int op_rmdir(const char* path) {
  auto r = ctx()->root->rmdir(to_rel(path));
  return r ? 0 : err_to_errno(r.error());
}

int op_rename(const char* from, const char* to, unsigned int) {
  auto r = ctx()->root->rename(to_rel(from), to_rel(to));
  return r ? 0 : err_to_errno(r.error());
}

int op_read(const char* path, char* buf, size_t size, OffT offset, struct fuse_file_info* fi) {
  if (ctx()->writeback) {
    if (int rc = flush_handle(handle_of(fi)); rc != 0) return rc;  // never read stale bytes
  }
  const std::string rel = to_rel(path);
  const std::size_t want = std::min<size_t>(size, 16u << 20);
  // Read straight into the FUSE buffer: on a cache hit this avoids allocating
  // and copying an intermediate vector for every chunk the kernel asks for.
  auto n = ctx()->root->read_into(rel, static_cast<std::uint64_t>(offset),
                                  std::span<std::byte>(reinterpret_cast<std::byte*>(buf), want));
  if (!n) return err_to_errno(n.error());
  return static_cast<int>(*n);
}

// The target is handed over as stored on the serving side. A relative target
// resolves against the link's directory on the mount, which is what makes
// `.cache -> ../var/cache` or `python -> python3.12` work from the other side.
// An absolute Linux target seen from Windows (or vice versa) names a path that
// does not exist there - the link is then a dangling one, which is honest.
int op_readlink(const char* path, char* buf, size_t size) {
  if (size == 0) return -EINVAL;
  auto target = ctx()->root->readlink(to_rel(path));
  if (!target) return err_to_errno(target.error());
  const std::size_t n = std::min(target->size(), size - 1);  // FUSE wants a NUL-terminated string
  std::memcpy(buf, target->data(), n);
  buf[n] = '\0';
  return 0;
}

int op_statfs(const char*, StatvfsT* st) {
  // WinFsp refuses writes ("device error") without free space reported; advertise
  // a large backing store (the real limit is the served volume).
  std::memset(st, 0, sizeof(*st));
  st->f_bsize = 4096;
  st->f_frsize = 4096;
  st->f_blocks = std::uint64_t{1} << 32;  // ~16 TiB
  st->f_bfree = std::uint64_t{1} << 31;
  st->f_bavail = std::uint64_t{1} << 31;
  st->f_files = std::uint64_t{1} << 20;
  st->f_ffree = std::uint64_t{1} << 19;
  st->f_namemax = 255;
  return 0;
}

int op_fsync(const char*, int, struct fuse_file_info* fi) { return flush_handle(handle_of(fi)); }

// Accepted but not carried across the boundary: the served tree may live on a
// filesystem with no POSIX mode or ns timestamps (NTFS, via the Windows agent),
// and the protocol has no message for either. Returning ENOSYS instead is worse
// than accepting: git probes `core.filemode` by chmod-ing .git/config.lock and
// aborts the whole clone if that fails, so a repo could not live on the mount at
// all. The mirror keeps reporting the attributes the agent reports.
int op_chmod(const char* path, ModeT, struct fuse_file_info*) {
  const std::string rel = to_rel(path);
  return ctx()->root->with_tree([&](const MetadataTree& t) {
    return t.lookup(rel, LookupMode::CaseInsensitive) ? 0 : -ENOENT;
  });
}

int op_utimens(const char* path, const TimespecT[2], struct fuse_file_info*) {
  const std::string rel = to_rel(path);
  return ctx()->root->with_tree([&](const MetadataTree& t) {
    return t.lookup(rel, LookupMode::CaseInsensitive) ? 0 : -ENOENT;
  });
}

// Cache policy and connection negotiation. Everything here is FUSE-level
// negotiation the mount previously left at its defaults; none of it needs a
// kernel change.
void* op_init(struct fuse_conn_info* conn, struct fuse_config* cfg) {
  // Let the kernel keep a file's pages across opens and revalidate them against
  // (mtime, size) on the next open, instead of dropping them unconditionally.
  // `kernel_cache` would skip the revalidation entirely; `auto_cache` is the
  // same win for repeat reads without trusting the cache blindly, and getattr
  // is answered from the in-RAM mirror, so the check costs no round-trip. The
  // revalidation happens on open; FuseMount's invalidation hook covers the gap
  // between opens and shortens the window (see the comment on that thread).
  cfg->auto_cache = 1;
  cfg->entry_timeout = 1.0;
  cfg->attr_timeout = 1.0;
  // Do NOT cache negative lookups. A tool that asks "does the destination
  // exist?" immediately before creating it - which is what Rename-Item and
  // Move-Item do - leaves a cached "no" behind, and with a timeout that answer
  // outlives the rename that follows it. The name then reads as missing for up
  // to a second while an enumeration of the same directory lists it: exactly
  // the split the #88 conformance failures showed, where the mount and the
  // backing store both already held the right thing and only a per-name lookup
  // disagreed.
  //
  // It costs nothing here. The point of a negative cache is to save round trips
  // for names that are not there, and this mount answers every lookup from the
  // in-RAM mirror without crossing the boundary at all. Measured on the
  // serving-floor benchmark: walk, stat and read all unchanged. Zeroing
  // entry_timeout as well is a different matter - that put stat up from 14 ms
  // to ~200 ms and cost most of the read win - so the positive caches stay.
  cfg->negative_timeout = 0.0;

  if (conn != nullptr) {
    // Bigger payload per upcall. The transport already moves whole files in one
    // bulk request, so per-request cost dominates and a small cap just
    // multiplies the crossings. These are requests: the kernel clamps them.
    // max_read is deliberately not set here: libfuse cross-checks it against the
    // `-o max_read=` mount option and aborts the mount if the two disagree
    // ("init() and fuse_session_new() requested different maximum read size").
    // Raising max_write lifts the negotiated page count, which raises the read
    // size with it, so setting it separately buys nothing.
    conn->max_write = 1u << 20;
    conn->max_readahead = 1u << 20;
    // How many async requests the kernel keeps in flight. The default bounds
    // the read-ahead this client already implements, so raise it.
    conn->max_background = 64;
    conn->congestion_threshold = 48;

    // Capabilities must be filtered through `capable`: the offered set differs
    // between libfuse and WinFsp-FUSE, and asking for one that is not on offer
    // is an error rather than a no-op.
    unsigned want = FUSE_CAP_ASYNC_READ | FUSE_CAP_PARALLEL_DIROPS | FUSE_CAP_SPLICE_READ |
                    FUSE_CAP_SPLICE_WRITE | FUSE_CAP_SPLICE_MOVE;
#ifdef FUSE_CAP_CACHE_SYMLINKS
    want |= FUSE_CAP_CACHE_SYMLINKS;  // libfuse only; WinFsp-FUSE does not offer it
#endif
    conn->want |= (want & conn->capable);
    // Deliberately not requested:
    //  - FUSE_CAP_ATOMIC_O_TRUNC would hand a truncating open to op_open, which
    //    ignores fi->flags, so the truncation would be silently lost. Without
    //    it the kernel sends a separate setattr and op_truncate runs.
    //  - FUSE_CAP_WRITEBACK_CACHE would buffer writes in the kernel behind the
    //    --writeback coalescing this mount already does, with different flush
    //    points for the two.
    //  - FUSE_CAP_AUTO_INVAL_DATA covers the same ground as auto_cache above.
  }
  return fuse_get_context()->private_data;
}

fuse_operations make_ops() {
  fuse_operations ops{};
  ops.init = op_init;
  ops.getattr = op_getattr;
  ops.readdir = op_readdir;
  ops.open = op_open;
  ops.read = op_read;
  ops.create = op_create;
  ops.write = op_write;
  ops.truncate = op_truncate;
  ops.mkdir = op_mkdir;
  ops.unlink = op_unlink;
  ops.rmdir = op_rmdir;
  ops.rename = op_rename;
  ops.statfs = op_statfs;
  ops.readlink = op_readlink;  // without it every symlink in the tree shows as broken
  ops.fsync = op_fsync;
  ops.chmod = op_chmod;      // accepted (not persisted) - git aborts a clone without it
  ops.utimens = op_utimens;  // likewise, so `touch` and build tools work
  ops.flush = op_flush;
  ops.release = op_release;
  return ops;
}

}  // namespace

FuseMount::~FuseMount() { unmount(); }

Result<void> FuseMount::mount(const std::string& mountpoint, bool writeback) {
#ifdef _WIN32
  if (!load_winfsp_dll()) return fail(Errc::Unsupported);
#endif

  static Context context;  // FUSE keeps a single mount per process here
  context.root = &root_;
  // Every handler below resolves case-insensitively (a Windows volume has to,
  // and Direction B keeps the same behaviour so both mounts agree). The client
  // has to resolve the same way, or a read of `readme.md` against a stored
  // `README.md` passes the lookup here and then misses the cache, the
  // read-ahead, and - against a case-sensitive agent - the file itself.
  root_.set_lookup_mode(LookupMode::CaseInsensitive);
  context.writeback = writeback;
  static fuse_operations ops = make_ops();

  struct fuse_args args = FUSE_ARGS_INIT(0, nullptr);
  fuse_opt_add_arg(&args, "wsldrive");
#ifdef _WIN32
  // uid/gid = -1 maps ownership to the caller; rellinks keeps symlinks relative.
  fuse_opt_add_arg(&args, "-ouid=-1,gid=-1,rellinks,FileSystemName=wsldrive,volname=wsldrive");
#endif
  if (std::getenv("WSLDRIVE_FUSE_DEBUG") != nullptr) fuse_opt_add_arg(&args, "-d");

  struct fuse* f = fuse_new(&args, &ops, sizeof(ops), &context);
  fuse_opt_free_args(&args);
  if (f == nullptr) return fail(Errc::IoError);

  if (fuse_mount(f, mountpoint.c_str()) != 0) {
    fuse_destroy(f);
    return fail(Errc::IoError);
  }
  fuse_ = f;
  mountpoint_ = mountpoint;
  mounted_.store(true);

  // Drop the kernel's cached pages for a path the far side changed. Without
  // this the change is still not lost - the kernel notices it when the
  // attribute cache expires - but it is late by up to attr_timeout: with a file
  // held open across a far-side rewrite, kernel 6.18 kept serving the old bytes
  // for 981 ms, against 5 ms with this punch in place.
  //
  // It runs on its own thread because fuse_invalidate_path() blocks until the
  // kernel has dropped the pages, and doing that on RemoteRoot's reader thread
  // would stall every other reply behind it.
  //
  // The 981ms-to-5ms figure is a libfuse measurement; WinFsp showed no
  // difference from the page-cache change either way, so this costs nothing
  // measurable there and is left on for both.
  {
    std::lock_guard lock(inval_mu_);
    inval_stop_ = false;
    inval_queue_.clear();
  }
  inval_ = std::thread([this] { inval_loop(); });
  // Only the paths the client actually applied: an op it discarded as stale
  // relative to its own mutation changed nothing, and punching for it would act
  // on an event already judged obsolete. This also leaves the raw-batch
  // InvalidationHook free for `wsldrive watch`.
  root_.set_invalidated_paths_hook([this](std::span<const std::string> paths) {
    std::lock_guard lock(inval_mu_);
    if (inval_stop_) return;
    for (const std::string& p : paths) {
      if (inval_queue_.size() >= kInvalQueueCap) break;
      inval_queue_.push_back(to_fuse_path(p));
    }
    inval_cv_.notify_one();
  });

  loop_ = std::thread([this] {
    // Deliberately single-threaded. fuse_loop_mt() has been measured against
    // this repeatedly (3000 files, agent and mount both in WSL over loopback,
    // so only the serving path is timed) and loses every time:
    //
    //                              single-threaded   fuse_loop_mt
    //   warm read                      210 ms           289 ms
    //   warm read, 8 in parallel        78 ms           117 ms
    //   COLD read, 8 in parallel       155 ms           222 ms
    //   COLD read, sequential          298 ms           380 ms
    //
    // The cold parallel row is the one that settles it. An earlier version of
    // this comment reserved judgement there, on the grounds that threads should
    // help when requests genuinely block on the far side rather than being
    // served from cache. They do not: the client below already pipelines its
    // boundary crossings (one bulk ReadMany per directory, plus the async
    // prefetcher), so the FUSE hop is not what limits concurrency, and adding
    // threads there buys dispatch overhead and lock contention with nothing to
    // overlap.
    //
    // Still untested, and the only reason to revisit: the same comparison
    // across the real VM boundary, where per-request latency is higher than
    // loopback. Higher latency favours overlapping, so it is the case most
    // likely to change the answer - though the client's batching means few
    // requests are ever in flight at the FUSE hop regardless.
    fuse_loop(static_cast<struct fuse*>(fuse_));
    mounted_.store(false);
  });
  return {};
}

void FuseMount::inval_loop() {
  for (;;) {
    std::string path;
    {
      std::unique_lock lock(inval_mu_);
      inval_cv_.wait(lock, [this] { return inval_stop_ || !inval_queue_.empty(); });
      if (inval_stop_) return;
      path = std::move(inval_queue_.front());
      inval_queue_.pop_front();
    }
    // Best effort: a failure here means the kernel had nothing cached for the
    // path, which is the common case and not worth reporting.
    (void)fuse_invalidate_path(static_cast<struct fuse*>(fuse_), path.c_str());
  }
}

void FuseMount::unmount() {
  if (fuse_ == nullptr) return;
  auto* f = static_cast<struct fuse*>(fuse_);
  // Stop feeding the invalidation thread, and join it, before the fuse handle
  // it dereferences goes away. Harmless where the thread was never started.
  root_.set_invalidated_paths_hook({});
  {
    std::lock_guard lock(inval_mu_);
    inval_stop_ = true;
    inval_queue_.clear();
  }
  inval_cv_.notify_all();
  if (inval_.joinable()) inval_.join();
  fuse_exit(f);
  fuse_unmount(f);
  if (loop_.joinable()) loop_.join();
  fuse_destroy(f);
  fuse_ = nullptr;
  mounted_.store(false);
}

}  // namespace wsld::mount
