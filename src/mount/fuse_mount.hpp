#pragma once

#include "agent/client.hpp"
#include "core/error.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace wsld::mount {

/// Mounts a RemoteRoot as a filesystem via the FUSE3 API (WinFsp on Windows,
/// libfuse3 on Linux). Read-only for the Phase 1 MVP: metadata is served from
/// the client's in-RAM mirror (zero round-trips) and file contents are fetched
/// over the socket on demand.
///
/// The FUSE event loop runs on its own thread; `unmount()` stops it and joins.
/// A second thread drops the kernel's cached pages for paths the far side
/// changes, so the page cache can be trusted between opens.
class FuseMount {
 public:
  explicit FuseMount(agent::RemoteRoot& root) noexcept : root_(root) {}
  ~FuseMount();
  FuseMount(const FuseMount&) = delete;
  FuseMount& operator=(const FuseMount&) = delete;

  /// Mounts at `mountpoint` (a drive letter like "Z:" or a directory path) and
  /// starts serving. Returns once the volume is up. With `writeback`, writes to
  /// a file are buffered and coalesced, flushed on fsync/flush/release — fewer
  /// round-trips, at the cost of durability only at flush/close (opt-in).
  [[nodiscard]] Result<void> mount(const std::string& mountpoint, bool writeback = false);

  /// Signals the FUSE loop to exit, unmounts, and joins the loop thread.
  void unmount();

  [[nodiscard]] bool mounted() const noexcept { return mounted_.load(); }

 private:
  // Drains `inval_queue_`, dropping the kernel's cached pages for each path.
  void inval_loop();
  /// Queues one absolute FUSE path for a cache punch. Used both by the agent's
  /// invalidation hook and by the operation handlers, for mutations the mount
  /// performed itself.
  void enqueue_punch(std::string fuse_path);

  agent::RemoteRoot& root_;
  void* fuse_ = nullptr;  // struct fuse*
  std::string mountpoint_;
  std::thread loop_;
  std::atomic<bool> mounted_{false};

  // Paths the far side changed, awaiting a kernel page-cache punch. Fed by the
  // RemoteRoot invalidation hook, drained by `inval_`.
  std::thread inval_;
  std::mutex inval_mu_;
  std::condition_variable inval_cv_;
  std::deque<std::string> inval_queue_;
  bool inval_stop_ = false;
};

}  // namespace wsld::mount
