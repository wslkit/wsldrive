#include "mount/fsnotify_bridge.hpp"

#include <algorithm>
#include <cstdio>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef __linux__
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/vfs.h>
#include <unistd.h>

#ifndef FUSE_SUPER_MAGIC
#define FUSE_SUPER_MAGIC 0x65735546
#endif
#endif

namespace wsld::mount {

namespace {

const char* change_name(ChangeKind c) noexcept {
  switch (c) {
    case ChangeKind::Created: return "creation of";
    case ChangeKind::Modified: return "change to";
    case ChangeKind::Removed: return "removal of";
    case ChangeKind::MovedFrom: return "move away from";
    case ChangeKind::MovedTo: return "move onto";
    case ChangeKind::Unknown: break;
  }
  return "change to";
}

#ifdef __linux__
long this_tid() noexcept { return static_cast<long>(::syscall(SYS_gettid)); }
#endif

}  // namespace

FsNotifyBridge::~FsNotifyBridge() { stop(); }

bool FsNotifyBridge::supported() noexcept {
#ifdef __linux__
  return true;
#else
  return false;
#endif
}

std::string FsNotifyBridge::abs(std::string_view rel) const {
  std::string out = root_;
  if (!out.empty() && out.back() == '/') out.pop_back();
  out.push_back('/');
  out.append(rel);
  return out;
}

FsNotifyBridge::Stats FsNotifyBridge::stats() const {
  std::lock_guard lock(stats_mu_);
  return stats_;
}

bool FsNotifyBridge::is_self(int caller) const noexcept {
  const long c = static_cast<long>(caller);
  // FUSE reports the calling task's pid, which for a threaded caller is its
  // tid. Accept the process id as well: some paths report the thread group,
  // and either way it is this daemon and nothing else.
  return c != 0 && (c == poke_tid_.load(std::memory_order_relaxed) ||
                    c == self_pid_.load(std::memory_order_relaxed));
}

void FsNotifyBridge::set_active(Active a) {
  std::lock_guard lock(active_mu_);
  active_ = std::move(a);
}

void FsNotifyBridge::clear_active() {
  std::lock_guard lock(active_mu_);
  active_ = Active{};
}

FsNotifyBridge::Pretend FsNotifyBridge::pretend(int caller, std::string_view rel,
                                                NodeKind& kind) const noexcept {
  if (!is_self(caller)) return Pretend::Nothing;
  std::lock_guard lock(active_mu_);
  if (active_.pretend_path == Pretend::Nothing || active_.path != rel) return Pretend::Nothing;
  kind = active_.kind;
  return active_.pretend_path;
}

bool FsNotifyBridge::is_poke_thread(int caller) const noexcept { return is_self(caller); }

bool FsNotifyBridge::claim(int caller, Poke what, std::string_view rel, std::string_view rel2) {
  if (!is_self(caller)) return false;
  std::lock_guard lock(active_mu_);
  if (active_.what != what || active_.path != rel || active_.path2 != rel2) return false;
  // Retire the record. The syscall that raised this request is about to return,
  // and the pretence must be gone before libfuse re-reads the path to build its
  // reply — otherwise the reply would describe the fiction instead of the
  // mirror's real state.
  active_ = Active{};
  return true;
}

void FsNotifyBridge::post(std::span<const agent::RemoteRoot::AppliedChange> changes) {
  if (!running_.load(std::memory_order_relaxed)) return;

  // Pair the two halves of each move into one job before queueing, so the poke
  // loop only ever sees whole operations. Both sides upstream try to keep pairs
  // together, but a batch too large for one frame is broadcast in several and
  // the halves can still arrive apart; a half with no partner here degrades to
  // what it amounts to alone rather than being dropped.
  std::unordered_map<std::uint32_t, std::size_t> from_by_cookie;
  std::vector<Job> jobs;
  jobs.reserve(changes.size());
  for (const auto& c : changes) {
    if (c.rescan) {
      jobs.push_back(Job{ChangeKind::Unknown, {}, {}, NodeKind::Directory, true});
      continue;
    }
    if (c.change == ChangeKind::MovedFrom && c.cookie != 0) {
      from_by_cookie.emplace(c.cookie, jobs.size());
      jobs.push_back(Job{ChangeKind::MovedFrom, c.path, {}, c.kind, false});
      continue;
    }
    if (c.change == ChangeKind::MovedTo && c.cookie != 0) {
      if (const auto it = from_by_cookie.find(c.cookie); it != from_by_cookie.end()) {
        jobs[it->second].path2 = c.path;  // completes the pair in place, keeping its order
        from_by_cookie.erase(it);
        continue;
      }
      jobs.push_back(Job{ChangeKind::Created, c.path, {}, c.kind, false});  // no partner: it appeared
      continue;
    }
    jobs.push_back(Job{c.change, c.path, {}, c.kind, false});
  }
  // Sources still waiting for a destination lost it somewhere upstream; on
  // their own they are removals.
  for (const auto& [cookie, idx] : from_by_cookie) {
    (void)cookie;
    jobs[idx].change = ChangeKind::Removed;
  }

  std::size_t dropped = 0;
  {
    std::lock_guard lock(mu_);
    for (Job& j : jobs) {
      if (queue_.size() >= kQueueCap) {
        queue_.pop_front();
        ++dropped;
      }
      queue_.push_back(std::move(j));
    }
  }
  if (dropped != 0) {
    std::lock_guard lock(stats_mu_);
    stats_.dropped += dropped;
  }
  cv_.notify_one();
}

void FsNotifyBridge::run() {
  poke_tid_.store(this_tid_or_zero(), std::memory_order_relaxed);
  // Verified here rather than in start(): the check reads the mount, and the
  // mount is not answering until the FUSE loop is running, which start()'s
  // caller is in the middle of arranging. Blocking there would deadlock it.
  if (!verify_root()) {
    running_.store(false);
    return;
  }
  for (;;) {
    Job job;
    {
      std::unique_lock lock(mu_);
      cv_.wait(lock, [this] { return stop_.load() || !queue_.empty(); });
      if (stop_.load()) return;
      job = std::move(queue_.front());
      queue_.pop_front();
    }
    deliver(job);
  }
}

void FsNotifyBridge::deliver(const Job& job) {
  bool ok = false;
  {
    std::lock_guard lock(stats_mu_);
    ++stats_.events;
    if (job.rescan) ++stats_.rescans;
  }
  if (job.rescan) {
    // An overflow on the far side means no path can be named. Touch the mount
    // root so a watcher that re-walks on any event below its root gets the
    // chance to; one that waits for a specific path cannot be told.
    ok = poke_modify(Job{ChangeKind::Modified, {}, {}, NodeKind::Directory, false});
  } else if (!job.path2.empty()) {
    ok = poke_move(job);
  } else {
    switch (job.change) {
      case ChangeKind::Created: ok = poke_create(job); break;
      case ChangeKind::Modified: ok = poke_modify(job); break;
      case ChangeKind::Removed:
      case ChangeKind::MovedFrom: ok = poke_remove(job); break;
      case ChangeKind::MovedTo: ok = poke_create(job); break;
      case ChangeKind::Unknown: return;  // nothing to say about it
    }
  }
  std::lock_guard lock(stats_mu_);
  if (ok) {
    ++stats_.delivered;
    return;
  }
  ++stats_.failed;
  // A failed poke is a missed notification, which is silent by nature — the
  // mount stays correct, so nothing else would ever say it happened. Name the
  // first few, then stop: a storm of them must not become the log.
  if (stats_.failed <= kMaxReportedFailures)
    std::fprintf(stderr, "wsldrive: could not notify watchers of %s '%s'%s\n", change_name(job.change),
                 job.path.empty() ? "<root>" : job.path.c_str(),
                 stats_.failed == kMaxReportedFailures ? " (further failures not reported)" : "");
}

#ifdef __linux__

long FsNotifyBridge::this_tid_or_zero() noexcept { return this_tid(); }

bool FsNotifyBridge::inside_mount(std::string_view rel) const {
  // Check the parent, not the path itself: for a creation the path does not
  // exist yet, and for a removal it no longer does.
  const std::size_t slash = rel.rfind('/');
  const std::string parent = slash == std::string_view::npos ? root_ : abs(rel.substr(0, slash));
  struct ::stat st {};
  if (::lstat(parent.c_str(), &st) != 0) return false;
  return static_cast<std::uint64_t>(st.st_dev) == root_dev_;
}

bool FsNotifyBridge::verify_root() {
  // The pokes are ordinary filesystem calls, so pointing them anywhere but the
  // intended FUSE mount would create and delete real files in the wrong tree.
  // Two things are established here and relied on for the rest of the run: the
  // root is a FUSE mount, and which device that mount is — every poke re-checks
  // its target against the latter, so a path that resolves out of the mount is
  // refused rather than applied to whatever it landed on.
  struct ::statfs fs {};
  struct ::stat st {};
  if (::statfs(root_.c_str(), &fs) != 0 || ::lstat(root_.c_str(), &st) != 0) {
    std::fprintf(stderr, "wsldrive: inotify bridge disabled (cannot stat %s)\n", root_.c_str());
    return false;
  }
  if (static_cast<unsigned long>(fs.f_type) != FUSE_SUPER_MAGIC) {
    std::fprintf(stderr, "wsldrive: inotify bridge disabled (%s is not a FUSE mount)\n", root_.c_str());
    return false;
  }
  root_dev_ = static_cast<std::uint64_t>(st.st_dev);
  return true;
}

Result<void> FsNotifyBridge::start(const std::string& mount_root) {
  if (running_.load()) return {};
  root_ = mount_root;
  self_pid_.store(static_cast<long>(::getpid()), std::memory_order_relaxed);
  stop_.store(false);
  running_.store(true);
  thread_ = std::thread([this] { run(); });
  return {};
}

void FsNotifyBridge::stop() {
  if (!running_.exchange(false)) return;
  {
    std::lock_guard lock(mu_);
    stop_.store(true);
    queue_.clear();
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
  clear_active();
}

bool FsNotifyBridge::poke_create(const Job& job) {
  if (job.path.empty() || !inside_mount(job.path)) return false;
  const bool dir = job.kind == NodeKind::Directory;
  // The invalidation has already been applied, so the mirror knows this path
  // and the kernel's lookup would find it and never reach ->mknod/->mkdir.
  // Hide it from this thread alone for the length of the call.
  set_active(Active{dir ? Poke::Mkdir : Poke::Mknod, job.path, {}, Pretend::Absent, job.kind});
  const std::string p = abs(job.path);
  const int rc = dir ? ::mkdir(p.c_str(), 0755) : ::mknod(p.c_str(), S_IFREG | 0644, 0);
  clear_active();
  return rc == 0;
}

bool FsNotifyBridge::poke_modify(const Job& job) {
  if (!job.path.empty() && !inside_mount(job.path)) return false;
  // Only mtime. fsnotify_change() maps a change to both timestamps to
  // FS_ATTRIB (IN_ATTRIB) and one to mtime alone to FS_MODIFY (IN_MODIFY), and
  // IN_MODIFY is what a watch-mode tool is actually waiting for. No
  // interception is needed: the mount already accepts utimens without
  // forwarding it, and the mirror keeps reporting the agent's timestamps.
  const struct ::timespec times[2] = {{0, UTIME_OMIT}, {0, UTIME_NOW}};
  const std::string p = job.path.empty() ? root_ : abs(job.path);
  return ::utimensat(AT_FDCWD, p.c_str(), times, AT_SYMLINK_NOFOLLOW) == 0;
}

bool FsNotifyBridge::poke_remove(const Job& job) {
  if (job.path.empty() || !inside_mount(job.path)) return false;
  const bool dir = job.kind == NodeKind::Directory;
  // The mirror has already dropped the path, so the kernel would resolve it to
  // nothing and ->unlink would never run. Answer this thread's lookup with the
  // entry as it was. The ghost dentry does not outlive the call: the unlink
  // that follows drops it, and nothing else can see it in between, because
  // every other caller gets the mirror's real answer.
  set_active(Active{dir ? Poke::Rmdir : Poke::Unlink, job.path, {}, Pretend::Present, job.kind});
  const std::string p = abs(job.path);
  const int rc = dir ? ::rmdir(p.c_str()) : ::unlink(p.c_str());
  clear_active();
  return rc == 0;
}

bool FsNotifyBridge::poke_move(const Job& job) {
  if (job.path.empty() || job.path2.empty()) return false;
  if (!inside_mount(job.path) || !inside_mount(job.path2)) return false;
  // Letting the kernel do the rename is what makes this worth pairing the two
  // halves for: vfs_rename raises IN_MOVED_FROM and IN_MOVED_TO together, with
  // one cookie, which is exactly what a watcher needs to recognise a move
  // rather than an unrelated delete and create.
  //
  // The source is ghosted with the *destination's* type, because after the
  // rename that is the entry the kernel keeps.
  set_active(Active{Poke::Rename, job.path, job.path2, Pretend::Present, job.kind});
  const int rc = ::rename(abs(job.path).c_str(), abs(job.path2).c_str());
  clear_active();

  // The kernel does not build a fresh inode for the destination; `d_move` walks
  // the source's dentry over to the new name, and that dentry is the one holding
  // the placeholder attributes the ghost handed out. Left alone, the destination
  // would report a zero size for as long as the attribute cache holds it. Drop
  // the entry so the next lookup goes back to the mirror.
  //
  // The invalidation hook queues a punch for both paths anyway, but on another
  // thread and with no ordering against this one, so it cannot be relied on to
  // land after the rename.
  if (rc == 0 && invalidate_) invalidate_(job.path2);
  return rc == 0;
}

#else  // not Linux

long FsNotifyBridge::this_tid_or_zero() noexcept { return 0; }
bool FsNotifyBridge::verify_root() { return false; }
bool FsNotifyBridge::inside_mount(std::string_view) const { return false; }
Result<void> FsNotifyBridge::start(const std::string&) { return fail(Errc::Unsupported); }
void FsNotifyBridge::stop() {}
bool FsNotifyBridge::poke_create(const Job&) { return false; }
bool FsNotifyBridge::poke_modify(const Job&) { return false; }
bool FsNotifyBridge::poke_remove(const Job&) { return false; }
bool FsNotifyBridge::poke_move(const Job&) { return false; }

#endif

}  // namespace wsld::mount
