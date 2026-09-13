// ReadDirectoryChangesW + I/O completion port watcher.
//
// One directory handle, one notification buffer, one thread. The buffer is
// re-armed immediately after each completion; if the kernel ran out of room
// (zero-length completion or ERROR_NOTIFY_ENUM_DIR) an Overflow event is
// emitted so the coalescer schedules a rescan.
#include "platform/watcher.hpp"
#include "platform/win/wide.hpp"

#include <windows.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace wsld::platform {

namespace {

// Notification buffer size. The documented 64 KiB ceiling applies only to a
// directory on a network share; locally the buffer can be as large as we care
// to pin, and its size is what decides how often the kernel runs out of room.
// An overflow is expensive far out of proportion to the memory: it costs the
// client a full re-snapshot, which holds its tree lock against every mount
// operation. A single `npm install` overruns 64 KiB.
constexpr std::size_t kWatchBufferBytes = 1u << 20;

constexpr ULONG_PTR kKeyChanges = 1;
constexpr ULONG_PTR kKeyStop = 2;
constexpr DWORD kFilter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_ATTRIBUTES |
                          FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION |
                          FILE_NOTIFY_CHANGE_SECURITY;

class Win32Watcher final : public Watcher {
 public:
  Win32Watcher(HANDLE dir, HANDLE iocp, WatchCallback cb) : dir_(dir), iocp_(iocp), cb_(std::move(cb)) {}

  ~Win32Watcher() override { stop(); }

  Result<void> start() {
    if (!arm()) return fail(Errc::IoError);
    thread_ = std::thread([this] { run(); });
    return {};
  }

  void stop() override {
    if (stopped_.exchange(true)) return;
    ::PostQueuedCompletionStatus(iocp_, 0, kKeyStop, nullptr);
    ::CancelIoEx(dir_, &ov_);
    if (thread_.joinable()) thread_.join();
    ::CloseHandle(iocp_);
    ::CloseHandle(dir_);
  }

 private:
  bool arm() {
    ov_ = OVERLAPPED{};
    return ::ReadDirectoryChangesW(dir_, buf_.data(), static_cast<DWORD>(buf_.size()), TRUE, kFilter, nullptr, &ov_,
                                   nullptr) != 0;
  }

  void run() {
    std::string path;
    for (;;) {
      DWORD bytes = 0;
      ULONG_PTR key = 0;
      LPOVERLAPPED ov = nullptr;
      const BOOL ok = ::GetQueuedCompletionStatus(iocp_, &bytes, &key, &ov, INFINITE);
      if (key == kKeyStop || stopped_.load()) return;
      if (!ok) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_OPERATION_ABORTED) return;
        if (err == ERROR_NOTIFY_ENUM_DIR) {
          cb_(FsEvent{FsEventKind::Overflow, {}});
          if (!arm()) return;
          continue;
        }
        return;  // handle went away
      }
      if (bytes == 0) {
        cb_(FsEvent{FsEventKind::Overflow, {}});
      } else {
        dispatch(bytes, path);
      }
      if (!arm()) {
        cb_(FsEvent{FsEventKind::Overflow, {}});
        return;
      }
    }
  }

  void dispatch(DWORD bytes, std::string& path) {
    // Copy out first: the kernel may not touch buf_ until re-armed, but keeping
    // the parse and the callback off the live buffer is cheap insurance. The
    // scratch buffer is a member, not a local - at this size it has no business
    // on the stack.
    std::vector<std::byte>& local = scratch_;
    std::memcpy(local.data(), buf_.data(), bytes);
    std::size_t off = 0;
    for (;;) {
      const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(local.data() + off);
      const std::wstring_view wname(info->FileName, info->FileNameLength / sizeof(wchar_t));
      path = win::to_utf8(wname);
      for (char& c : path)
        if (c == '\\') c = '/';
      FsEventKind kind;
      // ReadDirectoryChangesW has no equivalent of inotify's rename cookie: it
      // reports the two halves as OLD_NAME immediately followed by NEW_NAME in
      // the same buffer. Mint a cookie on the OLD_NAME and hand the same one to
      // the NEW_NAME that follows, so a consumer can pair them the way it pairs
      // an inotify move. Anything other than a NEW_NAME clears the pending
      // cookie, which leaves a truncated pair unpaired rather than joined to
      // the wrong partner.
      std::uint32_t cookie = 0;
      switch (info->Action) {
        case FILE_ACTION_ADDED: kind = FsEventKind::Created; break;
        case FILE_ACTION_REMOVED: kind = FsEventKind::Removed; break;
        case FILE_ACTION_MODIFIED: kind = FsEventKind::Modified; break;
        case FILE_ACTION_RENAMED_OLD_NAME:
          kind = FsEventKind::RenamedFrom;
          cookie = next_cookie();
          pending_cookie_ = cookie;
          break;
        case FILE_ACTION_RENAMED_NEW_NAME:
          kind = FsEventKind::RenamedTo;
          cookie = pending_cookie_ != 0 ? pending_cookie_ : next_cookie();
          break;
        default: kind = FsEventKind::Modified; break;
      }
      if (info->Action != FILE_ACTION_RENAMED_OLD_NAME) pending_cookie_ = 0;
      cb_(FsEvent{kind, path, cookie});
      if (info->NextEntryOffset == 0) break;
      off += info->NextEntryOffset;
    }
  }

  // Never returns 0: zero is the "no pair" marker downstream.
  std::uint32_t next_cookie() noexcept {
    if (++cookie_seq_ == 0) ++cookie_seq_;
    return cookie_seq_;
  }

  HANDLE dir_;
  HANDLE iocp_;
  WatchCallback cb_;
  std::uint32_t cookie_seq_ = 0;
  std::uint32_t pending_cookie_ = 0;  // cookie of an OLD_NAME awaiting its NEW_NAME
  OVERLAPPED ov_{};
  // Heap-allocated, so `new` gives it stricter alignment than
  // FILE_NOTIFY_INFORMATION needs.
  std::vector<std::byte> buf_ = std::vector<std::byte>(kWatchBufferBytes);
  std::vector<std::byte> scratch_ = std::vector<std::byte>(kWatchBufferBytes);
  std::thread thread_;
  std::atomic<bool> stopped_{false};
};

}  // namespace

Result<std::unique_ptr<Watcher>> make_watcher(const std::filesystem::path& root, WatchCallback cb) {
  const HANDLE dir = ::CreateFileW(root.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
  if (dir == INVALID_HANDLE_VALUE) return fail(Errc::NotFound);
  const HANDLE iocp = ::CreateIoCompletionPort(dir, nullptr, kKeyChanges, 1);
  if (iocp == nullptr) {
    ::CloseHandle(dir);
    return fail(Errc::IoError);
  }
  auto w = std::make_unique<Win32Watcher>(dir, iocp, std::move(cb));
  if (auto r = w->start(); !r) return fail(r.error());
  return std::unique_ptr<Watcher>(std::move(w));
}

}  // namespace wsld::platform
