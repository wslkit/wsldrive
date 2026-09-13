// Linux directory watcher.
//
// Primary implementation: inotify with recursive per-directory watches. It needs
// no privileges (unlike fanotify FAN_MARK_FILESYSTEM, which requires CAP_SYS_ADMIN
// and is the planned whole-filesystem upgrade — see issue #3). Newly created
// directories are watched on the fly; removed directories drop their watches.
//
// A self-pipe lets stop() wake the poll() loop for a clean, deterministic join.
#include "platform/watcher.hpp"

#include <sys/inotify.h>
#include <sys/stat.h>
#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

namespace wsld::platform {

namespace {

constexpr std::uint32_t kMask = IN_CREATE | IN_DELETE | IN_MODIFY | IN_CLOSE_WRITE | IN_ATTRIB | IN_MOVED_FROM |
                                IN_MOVED_TO | IN_MOVE_SELF | IN_DELETE_SELF | IN_ONLYDIR | IN_EXCL_UNLINK;

std::string join_rel(const std::string& dir, const char* name) {
  if (dir.empty()) return name;
  std::string out = dir;
  out.push_back('/');
  out += name;
  return out;
}

class InotifyWatcher final : public Watcher {
 public:
  InotifyWatcher(int fd, std::filesystem::path root, WatchCallback cb)
      : fd_(fd), root_(std::move(root)), cb_(std::move(cb)) {
    stop_pipe_[0] = stop_pipe_[1] = -1;
  }

  ~InotifyWatcher() override { stop(); }

  Result<void> start() {
    if (::pipe(stop_pipe_) != 0) return fail(Errc::IoError);
    root_dev_ = device_of(root_);
    add_watch_recursive(root_, "");
    if (wd_to_dir_.empty()) return fail(Errc::IoError);  // could not watch even the root
    thread_ = std::thread([this] { run(); });
    return {};
  }

  void stop() override {
    if (stopped_.exchange(true)) return;
    if (stop_pipe_[1] != -1) {
      const char b = 1;
      ssize_t n = ::write(stop_pipe_[1], &b, 1);
      (void)n;
    }
    if (thread_.joinable()) thread_.join();
    for (const auto& [wd, dir] : wd_to_dir_) ::inotify_rm_watch(fd_, wd);
    if (stop_pipe_[0] != -1) ::close(stop_pipe_[0]);
    if (stop_pipe_[1] != -1) ::close(stop_pipe_[1]);
    if (fd_ != -1) ::close(fd_);
    fd_ = -1;
  }

 private:
  // Filesystem a path lives on; 0 when it cannot be determined.
  static dev_t device_of(const std::filesystem::path& p) noexcept {
    struct ::stat st {};
    if (::stat(p.c_str(), &st) != 0) return 0;
    return st.st_dev;
  }

  void add_watch_recursive(const std::filesystem::path& abs, const std::string& rel) {
    const int wd = ::inotify_add_watch(fd_, abs.c_str(), kMask);
    if (wd < 0) return;
    wd_to_dir_[wd] = rel;
    dir_to_wd_[rel] = wd;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(abs, std::filesystem::directory_options::skip_permission_denied, ec)) {
      if (ec) break;
      std::error_code ec2;
      if (!e.is_directory(ec2) || e.is_symlink(ec2)) continue;
      // Stay on the root's filesystem, exactly like the scanner: descending into
      // mount points would walk /proc and /sys, and — far worse — recursively
      // enumerate /mnt/c (the whole Windows drive) back across the boundary.
      if (root_dev_ != 0 && device_of(e.path()) != root_dev_) continue;
      add_watch_recursive(e.path(), join_rel(rel, e.path().filename().c_str()));
    }
  }

  void drop_watch_subtree(const std::string& rel) {
    std::vector<std::string> gone;
    for (const auto& [dir, wd] : dir_to_wd_) {
      if (dir == rel || (dir.size() > rel.size() && dir.compare(0, rel.size(), rel) == 0 && dir[rel.size()] == '/')) {
        ::inotify_rm_watch(fd_, wd);
        wd_to_dir_.erase(wd);
        gone.push_back(dir);
      }
    }
    for (const auto& d : gone) dir_to_wd_.erase(d);
  }

  void run() {
    std::vector<char> buf(64 * 1024);
    pollfd fds[2];
    fds[0] = {fd_, POLLIN, 0};
    fds[1] = {stop_pipe_[0], POLLIN, 0};
    while (!stopped_.load()) {
      const int pr = ::poll(fds, 2, -1);
      if (pr < 0) {
        if (errno == EINTR) continue;
        break;
      }
      if (fds[1].revents & POLLIN) break;  // stop requested
      if (!(fds[0].revents & POLLIN)) continue;

      ssize_t len;
      do {
        len = ::read(fd_, buf.data(), buf.size());
      } while (len < 0 && errno == EINTR);
      if (len <= 0) continue;

      for (char* p = buf.data(); p < buf.data() + len;) {
        auto* ev = reinterpret_cast<inotify_event*>(p);
        handle_event(*ev);
        p += sizeof(inotify_event) + ev->len;
      }
    }
  }

  void handle_event(const inotify_event& ev) {
    if (ev.mask & IN_Q_OVERFLOW) {
      cb_(FsEvent{FsEventKind::Overflow, {}});
      return;
    }
    const auto it = wd_to_dir_.find(ev.wd);
    if (it == wd_to_dir_.end()) return;
    const std::string& dir = it->second;

    // Self events: the watched directory itself was removed or moved away.
    if (ev.mask & (IN_DELETE_SELF | IN_MOVE_SELF)) {
      if (!dir.empty()) cb_(FsEvent{FsEventKind::Removed, dir});
      return;
    }
    if (ev.len == 0) return;
    const std::string rel = join_rel(dir, ev.name);
    const bool is_dir = (ev.mask & IN_ISDIR) != 0;

    // inotify already pairs the two halves of a move with a cookie; pass it
    // straight through. It is only set on IN_MOVED_FROM/IN_MOVED_TO.
    if (ev.mask & (IN_CREATE | IN_MOVED_TO)) {
      if (is_dir) add_watch_recursive(root_ / std::filesystem::path(rel), rel);
      const bool moved = (ev.mask & IN_MOVED_TO) != 0;
      cb_(FsEvent{moved ? FsEventKind::RenamedTo : FsEventKind::Created, rel, moved ? ev.cookie : 0});
      // A directory may have been populated before we armed the watch; a scan/rescan
      // on the consumer side covers that. For files, CLOSE_WRITE will follow.
    } else if (ev.mask & (IN_DELETE | IN_MOVED_FROM)) {
      if (is_dir) drop_watch_subtree(rel);
      const bool moved = (ev.mask & IN_MOVED_FROM) != 0;
      cb_(FsEvent{moved ? FsEventKind::RenamedFrom : FsEventKind::Removed, rel, moved ? ev.cookie : 0});
    } else if (ev.mask & (IN_MODIFY | IN_CLOSE_WRITE | IN_ATTRIB)) {
      cb_(FsEvent{FsEventKind::Modified, rel});
    }
  }

  int fd_;
  std::filesystem::path root_;
  dev_t root_dev_ = 0;  // filesystem of root_; watches never leave it
  WatchCallback cb_;
  int stop_pipe_[2];
  std::unordered_map<int, std::string> wd_to_dir_;   // watch descriptor -> dir rel path
  std::unordered_map<std::string, int> dir_to_wd_;   // dir rel path -> watch descriptor
  std::thread thread_;
  std::atomic<bool> stopped_{false};
};

}  // namespace

Result<std::unique_ptr<Watcher>> make_watcher(const std::filesystem::path& root, WatchCallback cb) {
  std::error_code ec;
  if (!std::filesystem::is_directory(root, ec)) return fail(Errc::NotADirectory);
  const int fd = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (fd < 0) return fail(Errc::Unsupported);
  auto w = std::make_unique<InotifyWatcher>(fd, root, std::move(cb));
  if (auto r = w->start(); !r) return fail(r.error());
  return std::unique_ptr<Watcher>(std::move(w));
}

}  // namespace wsld::platform
