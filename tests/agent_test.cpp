#include "agent/client.hpp"
#include "agent/scanner.hpp"
#include "core/auth_token.hpp"
#include "agent/server.hpp"
#include "platform/watcher.hpp"
#ifdef _WIN32
#include "platform/win/wsl_launch.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include "platform/linux/win_launch.hpp"
#endif

#include <gtest/gtest.h>

#include <limits>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <span>
#include <string>
#include <thread>

namespace wsld::agent {
namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

void write_file(const fs::path& p, std::string_view content) {
  std::ofstream(p, std::ios::binary) << content;
}

class AgentTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = fs::temp_directory_path() /
            ("wsldrive-agent-" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
    fs::remove_all(root_);
    fs::create_directories(root_ / "src" / "include");
    fs::create_directories(root_ / "Docs");
    write_file(root_ / "README.md", "# readme\n");
    write_file(root_ / "src" / "main.cpp", "int main() {}\n");
    write_file(root_ / "src" / "include" / "a.hpp", "#pragma once\n");
    write_file(root_ / "Docs" / "Guide.txt", std::string(100000, 'g'));
  }
  void TearDown() override { fs::remove_all(root_); }

  fs::path root_;
};

TEST_F(AgentTest, ScanTreeProducesLoadableSnapshot) {
  std::vector<std::string> names;
  std::vector<SnapshotEntry> entries;
  auto stats = scan_tree(root_, [&](const SnapshotEntry& e) {
    names.emplace_back(e.name);
    entries.push_back(e);
  });
  ASSERT_TRUE(stats.has_value());
  EXPECT_EQ(stats->files, 4u);
  EXPECT_EQ(stats->directories, 3u);
  for (std::size_t i = 0; i < entries.size(); ++i) entries[i].name = names[i];

  MetadataTree t;
  ASSERT_TRUE(t.load_snapshot(entries).has_value());
  EXPECT_EQ(t.size(), 8u);
  const auto a = t.lookup("src/include/a.hpp");
  ASSERT_TRUE(a.has_value());
  EXPECT_EQ(t.node(*a).attr.kind, NodeKind::File);
  EXPECT_EQ(t.node(*a).attr.size, 13u);
  EXPECT_GT(t.node(*a).attr.mtime_ns, 1'600'000'000'000'000'000LL);
  EXPECT_TRUE(t.node(*t.lookup("Docs")).is_dir());
  EXPECT_EQ(t.node(*t.lookup("Docs/Guide.txt")).attr.size, 100000u);
  EXPECT_TRUE(t.lookup("docs/guide.TXT", LookupMode::CaseInsensitive).has_value());
  EXPECT_EQ(scan_tree(root_ / "README.md", [](const SnapshotEntry&) {}).error(), Errc::NotADirectory);
}

TEST_F(AgentTest, SingleFileSystemScanDoesNotPruneOrdinaryTrees) {
  // Staying on one filesystem must not change the result for a normal tree that
  // has no mount points in it: on (the default) and off must agree.
  auto count = [&](bool one_fs) {
    std::size_t n = 0;
    auto st = scan_tree(root_, [&](const SnapshotEntry&) { ++n; }, {}, one_fs);
    EXPECT_TRUE(st.has_value());
    return n;
  };
  const std::size_t with_limit = count(true);
  const std::size_t without = count(false);
  EXPECT_EQ(with_limit, without);
  EXPECT_GT(with_limit, 0u);

  // The root's own filesystem id must be resolvable, or the limit silently
  // disables itself and mount points would be traversed after all.
  EXPECT_TRUE(device_id(root_).has_value());
  EXPECT_FALSE(device_id(root_ / "definitely-not-here").has_value());

#ifndef _WIN32
  // /proc is a pseudo-filesystem, so it must report a different id than / —
  // this is exactly the comparison that keeps it out of a `--wsl-root /` scan.
  const auto slash = device_id("/");
  const auto proc = device_id("/proc");
  if (slash && proc) { EXPECT_NE(*slash, *proc); }
#endif
}

TEST_F(AgentTest, ReadAttributes) {
  auto a = read_attributes(root_ / "README.md");
  ASSERT_TRUE(a.has_value());
  EXPECT_EQ(a->kind, NodeKind::File);
  EXPECT_EQ(a->size, 9u);
  EXPECT_NE(a->mode & 0444u, 0u);
  EXPECT_EQ(read_attributes(root_ / "nope").error(), Errc::NotFound);
  EXPECT_EQ(join_relative(root_, "src/main.cpp"), root_ / "src" / "main.cpp");
}

/// Runs a RootServer on a loopback TCP listener with one accept thread.
class LoopbackServer {
 public:
  explicit LoopbackServer(fs::path root, bool watch, std::size_t chunk_bytes = (4u << 20), std::string token = {},
                          std::chrono::milliseconds handshake_timeout = 10s, std::size_t max_expanded = 100000,
                          std::chrono::milliseconds broadcast_timeout = 5s)
      : server_({.root = std::move(root),
                 .watch = watch,
                 .coalescer = {},
                 .snapshot_chunk_bytes = chunk_bytes,
                 .token = std::move(token),
                 .handshake_timeout = handshake_timeout,
                 .broadcast_timeout = broadcast_timeout,
                 .max_expanded_entries = max_expanded}) {
    auto l = net::Listener::bind(*net::Endpoint::parse("tcp://127.0.0.1:0"));
    if (!l) throw std::runtime_error("bind failed");
    listener_ = std::move(*l);
    endpoint_ = listener_.local();
    if (auto r = server_.start(); !r) throw std::runtime_error("server start failed");
    acceptor_ = std::thread([this] {
      while (!stopping_.load()) {
        auto s = listener_.accept(std::chrono::milliseconds(50));
        if (!s) {
          if (s.error() == Errc::Timeout) continue;
          break;
        }
        sessions_.emplace_back([this, sock = std::move(*s)]() mutable {
          net::FrameChannel ch(std::move(sock));
          server_.serve(ch);
        });
      }
    });
  }
  ~LoopbackServer() {
    stopping_.store(true);  // acceptor polls this via accept(timeout); no accept-unblock races
    acceptor_.join();
    listener_.close();
    server_.stop();  // shuts down live sessions so their threads unwind
    for (auto& t : sessions_) t.join();
  }
  [[nodiscard]] net::Endpoint endpoint() const { return endpoint_; }
  [[nodiscard]] RootServer& server() { return server_; }

 private:
  RootServer server_;
  net::Listener listener_;
  net::Endpoint endpoint_;
  std::atomic<bool> stopping_{false};
  std::thread acceptor_;
  std::vector<std::thread> sessions_;
};

std::unique_ptr<RemoteRoot> connect_client(const net::Endpoint& ep) {
  auto sock = net::connect(ep);
  if (!sock) return nullptr;
  return std::make_unique<RemoteRoot>(std::make_unique<net::FrameChannel>(std::move(*sock)));
}

TEST_F(AgentTest, EndToEndSnapshotAndRead) {
  LoopbackServer srv(root_, /*watch=*/false);
  auto client = connect_client(srv.endpoint());
  ASSERT_NE(client, nullptr);

  auto hello = client->connect();
  ASSERT_TRUE(hello.has_value()) << to_string(hello.error());
  EXPECT_EQ(hello->protocol_version, proto::kVersion);
  EXPECT_EQ(hello->capabilities, 0u);  // no watcher requested

  auto rtt = client->ping();
  ASSERT_TRUE(rtt.has_value());
  EXPECT_LT(*rtt, 1s);

  ASSERT_TRUE(client->fetch_snapshot().has_value());
  EXPECT_EQ(client->with_tree([](const MetadataTree& t) { return t.size(); }), 8u);
  EXPECT_TRUE(client->with_tree([](const MetadataTree& t) { return t.lookup("src/include/a.hpp").has_value(); }));
  EXPECT_GT(client->stats().snapshot_bytes, 0u);

  auto data = client->read("src/main.cpp", 0, 1 << 20);
  ASSERT_TRUE(data.has_value()) << to_string(data.error());
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(data->data()), data->size()), "int main() {}\n");

  auto tail = client->read("Docs/Guide.txt", 99990, 1000);
  ASSERT_TRUE(tail.has_value());
  EXPECT_EQ(tail->size(), 10u);

  auto past_end = client->read("Docs/Guide.txt", 1 << 30, 10);
  ASSERT_TRUE(past_end.has_value());
  EXPECT_TRUE(past_end->empty());

  EXPECT_EQ(client->read("missing.txt", 0, 10).error(), Errc::NotFound);
  EXPECT_EQ(client->read("../etc/passwd", 0, 10).error(), Errc::InvalidPath);

  // A second client sees the same tree.
  auto client2 = connect_client(srv.endpoint());
  ASSERT_NE(client2, nullptr);
  ASSERT_TRUE(client2->connect().has_value());
  ASSERT_TRUE(client2->fetch_snapshot().has_value());
  EXPECT_EQ(client2->with_tree([](const MetadataTree& t) { return t.size(); }), 8u);

  client->close();
  EXPECT_FALSE(client->connected());
  EXPECT_EQ(client->ping().error(), Errc::ConnectionClosed);
}

std::string read_disk(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::span<const std::byte> as_bytes(std::string_view s) { return std::as_bytes(std::span{s.data(), s.size()}); }

// Counts entries under root (files + directories), matching what scan_tree emits
// for a tree with no sockets/fifos/devices.
std::size_t scan_count(const fs::path& root) {
  std::size_t n = 0;
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
    ++n;
  return n;
}

#ifndef _WIN32
TEST_F(AgentTest, ScannerReportsSymlinks) {
  std::error_code ec;
  fs::create_symlink("main.cpp", root_ / "src" / "link_to_main", ec);
  ASSERT_FALSE(ec);
  bool saw_symlink = false;
  std::vector<std::string> names;
  std::vector<SnapshotEntry> entries;
  auto stats = scan_tree(root_, [&](const SnapshotEntry& e) {
    names.emplace_back(e.name);
    entries.push_back(e);
    if (e.attr.kind == NodeKind::Symlink) saw_symlink = true;
  });
  ASSERT_TRUE(stats.has_value());
  EXPECT_TRUE(saw_symlink);
  EXPECT_EQ(stats->symlinks, 1u);
  for (std::size_t i = 0; i < entries.size(); ++i) entries[i].name = names[i];
  MetadataTree t;
  ASSERT_TRUE(t.load_snapshot(entries).has_value());
  const auto link = t.lookup("src/link_to_main");
  ASSERT_TRUE(link.has_value());
  EXPECT_EQ(t.node(*link).attr.kind, NodeKind::Symlink);
}

TEST_F(AgentTest, ReadlinkReturnsTheStoredTarget) {
  // A link that is listed but cannot be read makes every symlink in a served
  // home directory (.cache, toolchain links, node_modules) look broken.
  std::error_code ec;
  fs::create_symlink("main.cpp", root_ / "src" / "rel_link", ec);
  ASSERT_FALSE(ec);
  fs::create_symlink("/nowhere/at/all", root_ / "dangling", ec);
  ASSERT_FALSE(ec);

  LoopbackServer srv(root_, /*watch=*/false);
  auto c = connect_client(srv.endpoint());
  ASSERT_NE(c, nullptr);
  ASSERT_TRUE(c->connect().has_value());
  ASSERT_TRUE(c->fetch_snapshot().has_value());

  auto rel = c->readlink("src/rel_link");
  ASSERT_TRUE(rel.has_value()) << to_string(rel.error());
  EXPECT_EQ(*rel, "main.cpp");
  // A dangling link still reports its target - readlink() does not resolve it.
  auto dangling = c->readlink("dangling");
  ASSERT_TRUE(dangling.has_value());
  EXPECT_EQ(*dangling, "/nowhere/at/all");
  // Not a link, not there, not inside the root.
  EXPECT_EQ(c->readlink("README.md").error(), Errc::InvalidArgument);
  EXPECT_EQ(c->readlink("nope").error(), Errc::NotFound);
  EXPECT_EQ(c->readlink("../etc/passwd").error(), Errc::InvalidPath);

  // The second call is served from the cache: no round-trip. Prove it by
  // changing the link on disk behind the client's back.
  fs::remove(root_ / "src" / "rel_link");
  fs::create_symlink("../README.md", root_ / "src" / "rel_link", ec);
  ASSERT_FALSE(ec);
  EXPECT_EQ(*c->readlink("src/rel_link"), "main.cpp") << "expected the cached target";
  // ...and an invalidation for that path drops the cached target.
  srv.server().notify(FsEvent{FsEventKind::Modified, "src/rel_link"});
  bool refreshed = false;
  for (int i = 0; i < 100 && !refreshed; ++i) {
    std::this_thread::sleep_for(20ms);
    auto again = c->readlink("src/rel_link");
    refreshed = again.has_value() && *again == "../README.md";
  }
  EXPECT_TRUE(refreshed) << "the invalidation must evict the cached link target";
}
#endif

TEST_F(AgentTest, IgnoreRulesExcludeFromSnapshot) {
  write_file(root_ / ".wsldriveignore", "build/\n*.log\n");
  fs::create_directories(root_ / "build" / "obj");
  write_file(root_ / "build" / "obj" / "x.o", "obj");
  write_file(root_ / "app.log", "log");
  write_file(root_ / "keep.txt", "keep");

  LoopbackServer srv(root_, /*watch=*/false);  // loads .wsldriveignore at construction
  auto c = connect_client(srv.endpoint());
  ASSERT_NE(c, nullptr);
  ASSERT_TRUE(c->connect().has_value());
  ASSERT_TRUE(c->fetch_snapshot().has_value());

  c->with_tree([](const MetadataTree& t) {
    EXPECT_FALSE(t.lookup("build").has_value());
    EXPECT_FALSE(t.lookup("build/obj/x.o").has_value());
    EXPECT_FALSE(t.lookup("app.log").has_value());
    EXPECT_TRUE(t.lookup("keep.txt").has_value());
    EXPECT_TRUE(t.lookup("README.md").has_value());  // seeded, not ignored
  });
}

TEST_F(AgentTest, ChunkedSnapshotReassembles) {
  // Add enough entries that a tiny per-frame budget forces many frames.
  for (int i = 0; i < 200; ++i) write_file(root_ / ("chunk_" + std::to_string(i) + ".dat"), "x");
  fs::create_directories(root_ / "deep" / "a" / "b");
  write_file(root_ / "deep" / "a" / "b" / "leaf.txt", "found");
  const std::size_t expected = scan_count(root_);

  LoopbackServer srv(root_, /*watch=*/false, /*chunk_bytes=*/256);  // ~a handful of entries per frame
  auto c = connect_client(srv.endpoint());
  ASSERT_NE(c, nullptr);
  ASSERT_TRUE(c->connect().has_value());
  ASSERT_TRUE(c->fetch_snapshot().has_value());

  EXPECT_EQ(c->with_tree([](const MetadataTree& t) { return t.size(); }), expected + 1);  // +root
  EXPECT_TRUE(c->with_tree([](const MetadataTree& t) { return t.lookup("deep/a/b/leaf.txt").has_value(); }));
  EXPECT_TRUE(c->with_tree([](const MetadataTree& t) { return t.lookup("chunk_199.dat").has_value(); }));
  // The reply really was split across many frames.
  EXPECT_GT(c->stats().snapshot_bytes, 256u);
}

TEST_F(AgentTest, RejectsAPeerWithoutTheSharedSecret) {
  // The agent serves reads and mutations, so an unauthenticated peer could read
  // or destroy the whole tree. With a token configured, only a peer presenting
  // it gets past the handshake.
  LoopbackServer srv(root_, /*watch=*/false, 4u << 20, "s3cret-token");

  auto wrong = connect_client(srv.endpoint());
  ASSERT_NE(wrong, nullptr);
  wrong->set_auth_token("not-the-token");
  const auto rejected = wrong->connect();
  ASSERT_FALSE(rejected.has_value()) << "a bad token must not be accepted";
  EXPECT_EQ(rejected.error(), Errc::Unauthorized) << "the reason must be reported, not a generic error";

  auto absent = connect_client(srv.endpoint());
  ASSERT_NE(absent, nullptr);
  EXPECT_FALSE(absent->connect().has_value()) << "no token must not be accepted";

  auto good = connect_client(srv.endpoint());
  ASSERT_NE(good, nullptr);
  good->set_auth_token("s3cret-token");
  ASSERT_TRUE(good->connect().has_value());
  ASSERT_TRUE(good->fetch_snapshot().has_value());
  EXPECT_TRUE(good->read("README.md", 0, 16).has_value());
}

TEST_F(AgentTest, ServesNothingBeforeAuthentication) {
  // Checking the token inside the Hello handler is not enough on its own: a peer
  // that never sends a Hello must not reach the read or mutation handlers with
  // its first frame, or the token is defeated entirely.
  LoopbackServer srv(root_, /*watch=*/false, 4u << 20, "s3cret-token");

  auto sock = net::connect(srv.endpoint());
  ASSERT_TRUE(sock.has_value());
  net::FrameChannel ch(std::move(*sock));

  // First frame is a read — no handshake at all.
  std::vector<std::byte> payload;
  proto::Writer w(payload);
  proto::write_read_request(w, proto::ReadRequest{.path = "README.md", .offset = 0, .length = 64});
  ASSERT_TRUE(ch.send(proto::MsgType::ReadRequest, 1, payload).has_value());

  auto reply = ch.receive(5s);
  ASSERT_TRUE(reply.has_value()) << "expected a refusal, not silence";
  EXPECT_EQ(reply->header.type, proto::MsgType::Error) << "an unauthenticated read must not be served";
  proto::Reader r(reply->payload);
  auto err = proto::read_error(r);
  ASSERT_TRUE(err.has_value());
  EXPECT_EQ(static_cast<Errc>(err->code), Errc::Unauthorized);

  // The same applies to a destructive request.
  auto sock2 = net::connect(srv.endpoint());
  ASSERT_TRUE(sock2.has_value());
  net::FrameChannel ch2(std::move(*sock2));
  std::vector<std::byte> p2;
  proto::Writer w2(p2);
  proto::write_path_request(w2, proto::PathRequest{.path = "README.md"});
  ASSERT_TRUE(ch2.send(proto::MsgType::UnlinkRequest, 1, p2).has_value());
  auto reply2 = ch2.receive(5s);
  ASSERT_TRUE(reply2.has_value());
  EXPECT_EQ(reply2->header.type, proto::MsgType::Error) << "an unauthenticated unlink must not be served";
  EXPECT_TRUE(fs::exists(root_ / "README.md")) << "the file must still be there";
}

TEST_F(AgentTest, UnauthenticatedPeerReceivesNoInvalidations) {
  // Joining the broadcast set is a privilege too: invalidation frames carry the
  // path and attributes of everything changing under the root, so a peer that
  // connects and stays silent must not be fed that stream.
  LoopbackServer srv(root_, /*watch=*/true, 4u << 20, "s3cret-token");
  if (!srv.server().watching()) GTEST_SKIP() << "no filesystem watcher on this platform";

  auto sock = net::connect(srv.endpoint());
  ASSERT_TRUE(sock.has_value());
  net::FrameChannel silent(std::move(*sock));  // connects, never authenticates

  // Churn the tree so invalidations are generated and broadcast.
  for (int i = 0; i < 5; ++i) {
    write_file(root_ / ("churn" + std::to_string(i) + ".txt"), "x");
    std::this_thread::sleep_for(60ms);
  }
  std::this_thread::sleep_for(500ms);

  // The silent peer must have been sent nothing at all.
  auto f = silent.receive(300ms);
  if (f.has_value()) {
    ADD_FAILURE() << "unauthenticated peer received a frame of type " << static_cast<int>(f->header.type);
  } else {
    EXPECT_EQ(f.error(), Errc::Timeout) << "expected silence, got " << to_string(f.error());
  }
}

TEST_F(AgentTest, SilentPeerIsDroppedAfterTheHandshakeTimeout) {
  // The authentication gate only fires on an incoming frame, so a peer that
  // connects and never sends anything would otherwise hold one of the agent's
  // session slots forever; 32 of those lock every real client out.
  LoopbackServer srv(root_, /*watch=*/false, 4u << 20, "s3cret-token", /*handshake_timeout=*/400ms);

  auto sock = net::connect(srv.endpoint());
  ASSERT_TRUE(sock.has_value());
  net::FrameChannel silent(std::move(*sock));

  // The server says why, then hangs up.
  auto notice = silent.receive(5s);
  ASSERT_TRUE(notice.has_value()) << "expected the server to end the session, not keep waiting";
  EXPECT_EQ(notice->header.type, proto::MsgType::Error);
  proto::Reader r(notice->payload);
  auto err = proto::read_error(r);
  ASSERT_TRUE(err.has_value());
  EXPECT_EQ(static_cast<Errc>(err->code), Errc::Timeout);
  auto after = silent.receive(5s);
  EXPECT_FALSE(after.has_value()) << "the session must be closed after the notice";
  if (!after.has_value()) {
    EXPECT_NE(after.error(), Errc::Timeout) << "closed, not merely quiet";
  }

  // A peer that does authenticate in time is unaffected by the deadline.
  auto good = connect_client(srv.endpoint());
  ASSERT_NE(good, nullptr);
  good->set_auth_token("s3cret-token");
  ASSERT_TRUE(good->connect().has_value());
  std::this_thread::sleep_for(600ms);  // well past the handshake window
  EXPECT_TRUE(good->ping().has_value()) << "an authenticated session must not be timed out";
}

TEST_F(AgentTest, PartialPreAuthFrameCannotHoldASession) {
  // The handshake deadline used to be checked only between frames: a peer that
  // sent one byte, or a header announcing 64 MiB and nothing more, parked the
  // session thread in recv forever (and reserved the 64 MiB). 32 of those
  // locked every real client out; one hung an auto-launched agent for good.
  LoopbackServer srv(root_, /*watch=*/false, 4u << 20, "s3cret-token", /*handshake_timeout=*/400ms);

  {  // one byte, then silence
    auto sock = net::connect(srv.endpoint());
    ASSERT_TRUE(sock.has_value());
    const std::byte one{0x57};
    ASSERT_TRUE(sock->send_all(std::span<const std::byte>(&one, 1)).has_value());
    net::FrameChannel ch(std::move(*sock));
    const auto t0 = std::chrono::steady_clock::now();
    auto f = ch.receive(5s);  // the server may say why (Error) before hanging up
    if (f.has_value()) {
      EXPECT_EQ(f->header.type, proto::MsgType::Error);
      f = ch.receive(5s);
    }
    ASSERT_FALSE(f.has_value()) << "the session must be closed";
    EXPECT_NE(f.error(), Errc::Timeout) << "closed, not merely quiet";
    EXPECT_LT(std::chrono::steady_clock::now() - t0, 3s);
  }
  {  // a header claiming a huge payload, before authenticating
    auto sock = net::connect(srv.endpoint());
    ASSERT_TRUE(sock.has_value());
    std::array<std::byte, proto::kHeaderSize> hdr{};
    proto::FrameHeader h;
    h.type = proto::MsgType::Hello;
    h.payload_len = 32u << 20;
    proto::encode_header(h, hdr);
    ASSERT_TRUE(sock->send_all(hdr).has_value());
    net::FrameChannel ch(std::move(*sock));
    auto f = ch.receive(5s);
    if (f.has_value()) f = ch.receive(5s);
    ASSERT_FALSE(f.has_value()) << "an oversized pre-auth frame must end the session";
    EXPECT_NE(f.error(), Errc::Timeout);
  }
  // Authentication still works, and an authenticated session may send big frames.
  auto good = connect_client(srv.endpoint());
  ASSERT_NE(good, nullptr);
  good->set_auth_token("s3cret-token");
  ASSERT_TRUE(good->connect().has_value());
  ASSERT_TRUE(good->fetch_snapshot().has_value());
  std::vector<std::byte> big(1u << 20, std::byte{'b'});
  ASSERT_TRUE(good->create_file("big.bin").has_value());
  EXPECT_TRUE(good->write("big.bin", 0, big).has_value()) << "post-auth frames must not be capped";
}

TEST_F(AgentTest, RenamedDirectoryExpansionIsChunkedAndCapped) {
  // One renamed directory used to be described in ONE invalidation frame; a big
  // enough tree exceeded the frame limit and every peer dropped its connection.
  // Now the batch is split into frames, and past a budget the peers are told to
  // Rescan instead of being sent the world.
  {  // chunking: tiny frames, a directory with many entries
    LoopbackServer srv(root_, /*watch=*/false, /*chunk_bytes=*/256);
    auto c = connect_client(srv.endpoint());
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(c->connect().has_value());
    ASSERT_TRUE(c->fetch_snapshot().has_value());
    fs::create_directories(root_ / "moved");
    for (int i = 0; i < 60; ++i) write_file(root_ / "moved" / ("f" + std::to_string(i)), "x");
    const auto batches_before = c->stats().invalidation_batches;
    srv.server().notify(FsEvent{FsEventKind::RenamedTo, "moved"});
    bool complete = false;
    for (int i = 0; i < 200 && !complete; ++i) {
      std::this_thread::sleep_for(25ms);
      complete = c->with_tree([](const MetadataTree& t) {
        std::size_t n = 0;
        if (const auto d = t.lookup("moved")) t.for_each_child(*d, [&](NodeId) { ++n; });
        return n == 60;
      });
    }
    EXPECT_TRUE(complete) << "every entry of the renamed directory must arrive";
    EXPECT_GT(c->stats().invalidation_batches - batches_before, 1u) << "the expansion should have been split into several frames";
  }
  {  // cap: a budget of 5 entries turns the expansion into a Rescan
    fs::remove_all(root_ / "moved");
    LoopbackServer srv(root_, /*watch=*/false, 4u << 20, {}, 10s, /*max_expanded=*/5);
    auto c = connect_client(srv.endpoint());
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(c->connect().has_value());
    ASSERT_TRUE(c->fetch_snapshot().has_value());
    std::atomic<bool> saw_rescan{false};
    c->set_invalidation_hook([&](const proto::InvalidationBatch& b) {
      for (const auto& op : b.ops)
        if (op.kind == InvalidationKind::Rescan) saw_rescan = true;
    });
    fs::create_directories(root_ / "big");
    for (int i = 0; i < 20; ++i) write_file(root_ / "big" / ("f" + std::to_string(i)), "x");
    srv.server().notify(FsEvent{FsEventKind::RenamedTo, "big"});
    bool complete = false;
    for (int i = 0; i < 200 && !complete; ++i) {
      std::this_thread::sleep_for(25ms);
      complete = c->with_tree([](const MetadataTree& t) { return t.lookup("big/f19").has_value(); });
    }
    EXPECT_TRUE(saw_rescan) << "over budget, the peers must be told to rescan";
    EXPECT_TRUE(complete) << "the rescan must bring the contents in";
  }
}

TEST_F(AgentTest, LateRemoveForARecreatedPathDoesNotDeleteIt) {
  // git's lock-file dance: config.lock is renamed away and a NEW config.lock
  // is created at once. The watcher's Remove for the old one arrives after the
  // new one exists; applied blindly it deletes the new file from the mirror and
  // git's next chmod fails with ENOENT. The agent must resolve a Remove against
  // the disk at flush time, like it does an Upsert.
  LoopbackServer srv(root_, /*watch=*/false);
  auto c = connect_client(srv.endpoint());
  ASSERT_NE(c, nullptr);
  ASSERT_TRUE(c->connect().has_value());
  ASSERT_TRUE(c->fetch_snapshot().has_value());

  ASSERT_TRUE(c->create_file("config.lock").has_value());  // the "new" lock, mirrored optimistically
  ASSERT_TRUE(fs::exists(root_ / "config.lock"));
  srv.server().notify(FsEvent{FsEventKind::Removed, "config.lock"});  // the stale event for the old one
  // Give the batch time to arrive; whatever it says, the path exists on disk.
  std::this_thread::sleep_for(300ms);
  EXPECT_TRUE(c->with_tree([](const MetadataTree& t) { return t.lookup("config.lock").has_value(); }))
      << "a Remove for a path that exists again must not delete it from the mirror";

  // And a Remove for something really gone still removes it.
  fs::remove(root_ / "config.lock");
  srv.server().notify(FsEvent{FsEventKind::Removed, "config.lock"});
  bool gone = false;
  for (int i = 0; i < 100 && !gone; ++i) {
    std::this_thread::sleep_for(20ms);
    gone = !c->with_tree([](const MetadataTree& t) { return t.lookup("config.lock").has_value(); });
  }
  EXPECT_TRUE(gone);
}

TEST(AuthToken, GeneratesDistinctHexSecrets) {
  const std::string a = generate_auth_token();
  const std::string b = generate_auth_token();
  ASSERT_FALSE(a.empty()) << "no entropy source available";
  EXPECT_EQ(a.size(), 32u);  // 128 bits as hex
  EXPECT_NE(a, b);
  EXPECT_EQ(a.find_first_not_of("0123456789abcdef"), std::string::npos);
}

TEST_F(AgentTest, ReadIntoMatchesReadAndServesFromCache) {
  LoopbackServer srv(root_, /*watch=*/false);
  auto c = connect_client(srv.endpoint());
  ASSERT_NE(c, nullptr);
  ASSERT_TRUE(c->connect().has_value());
  ASSERT_TRUE(c->fetch_snapshot().has_value());

  std::array<std::byte, 64> buf{};
  auto n = c->read_into("README.md", 0, buf);  // miss: goes to the agent
  ASSERT_TRUE(n.has_value());
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(buf.data()), *n), "# readme\n");

  const auto hits_before = c->stats().read_cache_hits;
  auto again = c->read_into("README.md", 2, buf);  // hit: copied straight into buf
  ASSERT_TRUE(again.has_value());
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(buf.data()), *again), "readme\n");
  EXPECT_GT(c->stats().read_cache_hits, hits_before);

  // Past EOF yields nothing, and a missing file reports the same error as read().
  auto past = c->read_into("README.md", 1000, buf);
  ASSERT_TRUE(past.has_value());
  EXPECT_EQ(*past, 0u);
  EXPECT_EQ(c->read_into("nope.txt", 0, buf).error(), Errc::NotFound);

  // A short buffer must be honoured, never overrun.
  std::array<std::byte, 4> small{};
  auto few = c->read_into("README.md", 0, small);
  ASSERT_TRUE(few.has_value());
  EXPECT_EQ(*few, 4u);
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(small.data()), 4), "# re");
}

TEST_F(AgentTest, ReadCacheEvictsLeastRecentlyUsed) {
  // Eviction must drop the least-recently-*used* entry, not the oldest-inserted:
  // re-reading a file has to refresh its recency.
  // One file per directory on purpose: a read miss bulk-fetches the target's
  // siblings, which would otherwise re-cache everything and mask the ordering.
  const std::string body(1000, 'x');
  for (const char* d : {"d1", "d2", "d3"}) {
    fs::create_directories(root_ / "lru" / d);
    write_file(root_ / "lru" / d / "f", body);
  }

  LoopbackServer srv(root_, /*watch=*/false);
  auto c = connect_client(srv.endpoint());
  ASSERT_NE(c, nullptr);
  ASSERT_TRUE(c->connect().has_value());
  ASSERT_TRUE(c->fetch_snapshot().has_value());
  c->set_read_cache_limit(2500);  // room for two of these files, not three

  ASSERT_TRUE(c->read("lru/d1/f", 0, 16).has_value());
  ASSERT_TRUE(c->read("lru/d2/f", 0, 16).has_value());
  ASSERT_TRUE(c->read("lru/d1/f", 0, 16).has_value());  // refreshes d1; d2 is now LRU
  ASSERT_TRUE(c->read("lru/d3/f", 0, 16).has_value());  // pushes past the budget

  const auto hits_before = c->stats().read_cache_hits;
  ASSERT_TRUE(c->read("lru/d1/f", 0, 16).has_value());
  EXPECT_GT(c->stats().read_cache_hits, hits_before) << "recently used entry should have survived";

  const auto misses_before = c->stats().read_cache_misses;
  ASSERT_TRUE(c->read("lru/d2/f", 0, 16).has_value());
  EXPECT_GT(c->stats().read_cache_misses, misses_before) << "least-recently-used entry should have been evicted";
  EXPECT_LE(c->stats().read_cache_bytes, 2500u) << "cache must stay within its budget";

  // Lowering the budget to nothing must clear it down (one entry may remain).
  c->set_read_cache_limit(0);
  EXPECT_LE(c->stats().read_cache_bytes, body.size());
}

TEST_F(AgentTest, RejectsPathsOutsideTheServedRoot) {
  // A peer must not be able to name anything outside the root. `..` was already
  // refused, but an absolute/drive-letter path contains no `..` at all and
  // fs::path::operator/ REPLACES the left side when the right has a root name,
  // so `C:/Windows/...` used to escape the root completely.
  LoopbackServer srv(root_, /*watch=*/false);
  auto c = connect_client(srv.endpoint());
  ASSERT_NE(c, nullptr);
  ASSERT_TRUE(c->connect().has_value());
  ASSERT_TRUE(c->fetch_snapshot().has_value());

  for (const char* escape : {"../outside.txt", "a/../../outside.txt", "C:/Windows/win.ini",
                             "C:\\Windows\\win.ini", "D:/x", "sub:stream"}) {
    auto r = c->read(escape, 0, 64);
    EXPECT_FALSE(r.has_value()) << "escaped the root: " << escape;
  }
  // Bulk reads use the same gate: an escaping path yields no data, and a legal
  // sibling in the same request still succeeds.
  auto many = c->read_many({"C:/Windows/win.ini", "../outside.txt", "README.md"});
  ASSERT_TRUE(many.has_value());
  EXPECT_FALSE((*many)[0].has_value());
  EXPECT_FALSE((*many)[1].has_value());
  ASSERT_TRUE((*many)[2].has_value());

  // A name that merely contains ".." is legal and must still be served.
  write_file(root_ / "foo..bar", "dots");
  auto ok = c->read("foo..bar", 0, 64);
  ASSERT_TRUE(ok.has_value());
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(ok->data()), ok->size()), "dots");
}

TEST_F(AgentTest, WriteThroughMutations) {
  LoopbackServer srv(root_, /*watch=*/false);  // isolate the optimistic-update path
  auto c = connect_client(srv.endpoint());
  ASSERT_NE(c, nullptr);
  ASSERT_TRUE(c->connect().has_value());
  ASSERT_TRUE(c->fetch_snapshot().has_value());

  auto size_in_tree = [&](std::string_view path) -> std::int64_t {
    return c->with_tree([&](const MetadataTree& t) -> std::int64_t {
      const auto id = t.lookup(path, LookupMode::Exact);
      return id ? static_cast<std::int64_t>(t.node(*id).attr.size) : -1;
    });
  };

  // create + write + read back, verifying disk, mirror, and the read path.
  ASSERT_TRUE(c->create_file("top.txt").has_value());
  EXPECT_TRUE(fs::exists(root_ / "top.txt"));
  auto wrote = c->write("top.txt", 0, as_bytes("hello world"));
  ASSERT_TRUE(wrote.has_value());
  EXPECT_EQ(*wrote, 11u);
  EXPECT_EQ(read_disk(root_ / "top.txt"), "hello world");
  EXPECT_EQ(size_in_tree("top.txt"), 11);
  auto back = c->read("top.txt", 0, 100);
  ASSERT_TRUE(back.has_value());
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(back->data()), back->size()), "hello world");

  // write at an offset extends the file.
  ASSERT_TRUE(c->write("top.txt", 11, as_bytes("!!!")).has_value());
  EXPECT_EQ(read_disk(root_ / "top.txt"), "hello world!!!");
  EXPECT_EQ(size_in_tree("top.txt"), 14);

  // truncate.
  ASSERT_TRUE(c->truncate("top.txt", 5).has_value());
  EXPECT_EQ(read_disk(root_ / "top.txt"), "hello");
  EXPECT_EQ(size_in_tree("top.txt"), 5);

  // mkdir + nested create/write.
  ASSERT_TRUE(c->mkdir("sub2").has_value());
  EXPECT_TRUE(fs::is_directory(root_ / "sub2"));
  ASSERT_TRUE(c->create_file("sub2/x.txt").has_value());
  ASSERT_TRUE(c->write("sub2/x.txt", 0, as_bytes("data")).has_value());
  EXPECT_EQ(read_disk(root_ / "sub2" / "x.txt"), "data");
  EXPECT_TRUE(c->with_tree([](const MetadataTree& t) { return t.lookup("sub2/x.txt").has_value(); }));

  // rename.
  ASSERT_TRUE(c->rename("top.txt", "top2.txt").has_value());
  EXPECT_FALSE(fs::exists(root_ / "top.txt"));
  EXPECT_EQ(read_disk(root_ / "top2.txt"), "hello");
  EXPECT_TRUE(c->with_tree([](const MetadataTree& t) {
    return !t.lookup("top.txt").has_value() && t.lookup("top2.txt").has_value();
  }));
  // Renaming a directory must carry its subtree in the mirror - the file inside
  // is still there on disk, so it had better still be visible.
  ASSERT_TRUE(c->rename("sub2", "Docs/moved").has_value());
  EXPECT_EQ(read_disk(root_ / "Docs" / "moved" / "x.txt"), "data");
  EXPECT_TRUE(c->with_tree([](const MetadataTree& t) {
    return !t.lookup("sub2").has_value() && t.lookup("Docs/moved/x.txt").has_value();
  })) << "the renamed directory lost its contents in the mirror";
  ASSERT_TRUE(c->rename("Docs/moved", "sub2").has_value());  // put it back for the checks below

  // unlink + rmdir (rmdir refuses non-empty).
  EXPECT_EQ(c->rmdir("sub2").error(), Errc::AlreadyExists);  // directory_not_empty
  ASSERT_TRUE(c->unlink("sub2/x.txt").has_value());
  ASSERT_TRUE(c->rmdir("sub2").has_value());
  EXPECT_FALSE(fs::exists(root_ / "sub2"));
  ASSERT_TRUE(c->unlink("top2.txt").has_value());
  EXPECT_FALSE(fs::exists(root_ / "top2.txt"));

  // error cases.
  EXPECT_EQ(c->unlink("nope.txt").error(), Errc::NotFound);
  EXPECT_EQ(c->rmdir("README.md").error(), Errc::NotADirectory);
  EXPECT_EQ(c->unlink("src").error(), Errc::IsADirectory);
  EXPECT_EQ(c->rename("nope.txt", "whatever.txt").error(), Errc::NotFound);
  EXPECT_EQ(c->create_file("../escape.txt").error(), Errc::InvalidPath);
  EXPECT_EQ(c->write("../escape.txt", 0, as_bytes("x")).error(), Errc::InvalidPath);
}

TEST_F(AgentTest, ReadCacheServesRepeatReads) {
  LoopbackServer srv(root_, /*watch=*/false);
  auto c = connect_client(srv.endpoint());
  ASSERT_NE(c, nullptr);
  ASSERT_TRUE(c->connect().has_value());
  ASSERT_TRUE(c->fetch_snapshot().has_value());
  auto str = [](const std::vector<std::byte>& v) {
    return std::string(reinterpret_cast<const char*>(v.data()), v.size());
  };

  auto r1 = c->read("README.md", 0, 100);  // seeded "# readme\n"
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(str(*r1), "# readme\n");
  EXPECT_EQ(c->stats().read_cache_misses, 1u);
  EXPECT_EQ(c->stats().read_cache_hits, 0u);

  auto r2 = c->read("README.md", 0, 100);  // served from cache
  ASSERT_TRUE(r2.has_value());
  EXPECT_EQ(str(*r2), "# readme\n");
  EXPECT_EQ(c->stats().read_cache_hits, 1u);

  auto r3 = c->read("README.md", 2, 4);  // partial slice from cache
  ASSERT_TRUE(r3.has_value());
  EXPECT_EQ(str(*r3), "read");
  EXPECT_EQ(c->stats().read_cache_hits, 2u);

  // A write (even same-size) drops the cache entry; the next read refetches.
  ASSERT_TRUE(c->write("README.md", 0, as_bytes("XXXXX")).has_value());
  auto r4 = c->read("README.md", 0, 100);
  ASSERT_TRUE(r4.has_value());
  EXPECT_EQ(str(*r4).substr(0, 5), "XXXXX");
  EXPECT_EQ(c->stats().read_cache_misses, 2u);
}

TEST_F(AgentTest, ReadManyBulkFetch) {
  LoopbackServer srv(root_, /*watch=*/false);
  auto c = connect_client(srv.endpoint());
  ASSERT_NE(c, nullptr);
  ASSERT_TRUE(c->connect().has_value());
  ASSERT_TRUE(c->fetch_snapshot().has_value());
  auto res = c->read_many({"README.md", "src/main.cpp", "does/not/exist", "src/include/a.hpp"});
  ASSERT_TRUE(res.has_value());
  ASSERT_EQ(res->size(), 4u);
  ASSERT_TRUE((*res)[0].has_value());
  EXPECT_EQ(std::string(reinterpret_cast<const char*>((*res)[0]->data()), (*res)[0]->size()), "# readme\n");
  ASSERT_TRUE((*res)[1].has_value());
  EXPECT_EQ(std::string(reinterpret_cast<const char*>((*res)[1]->data()), (*res)[1]->size()), "int main() {}\n");
  EXPECT_FALSE((*res)[2].has_value());  // missing file reported as no-value
  ASSERT_TRUE((*res)[3].has_value());
}

TEST_F(AgentTest, PrefetchWarmsSiblings) {
  // Many files in one directory; reading one should prefetch the rest.
  fs::create_directories(root_ / "pf");
  for (int i = 0; i < 40; ++i) write_file(root_ / "pf" / ("f" + std::to_string(i) + ".txt"), "data" + std::to_string(i));
  LoopbackServer srv(root_, /*watch=*/false);
  auto c = connect_client(srv.endpoint());
  ASSERT_NE(c, nullptr);
  ASSERT_TRUE(c->connect().has_value());
  ASSERT_TRUE(c->fetch_snapshot().has_value());

  ASSERT_TRUE(c->read("pf/f0.txt", 0, 100).has_value());  // triggers directory prefetch
  // Wait for the background prefetch to populate the cache.
  bool warmed = false;
  for (int t = 0; t < 100 && !warmed; ++t) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    warmed = c->stats().read_cache_bytes > 0 &&
             c->with_tree([](const MetadataTree&) { return true; }) &&
             c->read("pf/f39.txt", 0, 100).has_value() && c->stats().read_cache_hits >= 1;
  }
  // A sibling we never explicitly read should now be a cache hit.
  const auto hits_before = c->stats().read_cache_hits;
  auto r = c->read("pf/f20.txt", 0, 100);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(r->data()), r->size()), "data20");
  EXPECT_GT(c->stats().read_cache_hits, hits_before) << "sibling should have been prefetched";
}

TEST_F(AgentTest, WarmCachePrefetchesWholeTree) {
  // A tree spread across several directories; warm_cache() should populate the
  // read cache for all of them so the first read of any file is a cache hit.
  for (int d = 0; d < 3; ++d) {
    fs::create_directories(root_ / ("wc" + std::to_string(d)));
    for (int i = 0; i < 10; ++i)
      write_file(root_ / ("wc" + std::to_string(d)) / ("f" + std::to_string(i) + ".txt"),
                 "d" + std::to_string(d) + "f" + std::to_string(i));
  }
  LoopbackServer srv(root_, /*watch=*/false);
  auto c = connect_client(srv.endpoint());
  ASSERT_NE(c, nullptr);
  ASSERT_TRUE(c->connect().has_value());
  ASSERT_TRUE(c->fetch_snapshot().has_value());

  const std::size_t queued = c->warm_cache();
  EXPECT_GT(queued, 0u);
  c->wait_prefetch_idle(10s);

  const auto st = c->stats();
  EXPECT_GT(st.prefetch_files, 0u) << "prefetcher should have fetched files";
  EXPECT_GT(st.prefetch_bytes, 0u);

  // A file we never explicitly read is served from the warmed cache (a hit, no
  // new miss).
  const auto misses_before = c->stats().read_cache_misses;
  auto r = c->read("wc2/f7.txt", 0, 100);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(r->data()), r->size()), "d2f7");
  EXPECT_EQ(c->stats().read_cache_misses, misses_before) << "warmed file should not miss";
  EXPECT_GE(c->stats().read_cache_hits, 1u);
}

TEST_F(AgentTest, ServerDisconnectFailsPendingRequests) {
  auto srv = std::make_unique<LoopbackServer>(root_, false);
  auto client = connect_client(srv->endpoint());
  ASSERT_NE(client, nullptr);
  ASSERT_TRUE(client->connect().has_value());
  srv.reset();  // closes the listener and session threads
  auto r = client->ping(2s);
  EXPECT_FALSE(r.has_value());
}

TEST_F(AgentTest, LiveInvalidationsReachTheClient) {
  LoopbackServer srv(root_, /*watch=*/true);
  if (!srv.server().watching()) GTEST_SKIP() << "no filesystem watcher on this platform/environment";
  auto client = connect_client(srv.endpoint());
  ASSERT_NE(client, nullptr);
  auto hello = client->connect();
  ASSERT_TRUE(hello.has_value());
  EXPECT_EQ(hello->capabilities, 1u);
  ASSERT_TRUE(client->fetch_snapshot().has_value());

  std::mutex mu;
  std::condition_variable cv;
  std::vector<proto::InvalidationOp> seen;
  client->set_invalidation_hook([&](const proto::InvalidationBatch& b) {
    std::lock_guard lock(mu);
    seen.insert(seen.end(), b.ops.begin(), b.ops.end());
    cv.notify_all();
  });
  auto wait_for = [&](auto pred) {
    std::unique_lock lock(mu);
    return cv.wait_for(lock, 10s, [&] { return pred(seen); });
  };

  // Create.
  write_file(root_ / "src" / "new_file.cpp", "// new\n");
  ASSERT_TRUE(wait_for([](const auto& ops) {
    for (const auto& op : ops)
      if (op.kind == InvalidationKind::Upsert && op.path == "src/new_file.cpp" && op.attr.size == 7) return true;
    return false;
  }));
  EXPECT_TRUE(client->with_tree([](const MetadataTree& t) {
    const auto n = t.lookup("src/new_file.cpp");
    return n.has_value() && t.node(*n).attr.size == 7;
  }));

  // Remove a directory subtree.
  fs::remove_all(root_ / "src" / "include");
  ASSERT_TRUE(wait_for([](const auto& ops) {
    for (const auto& op : ops)
      if (op.kind == InvalidationKind::Remove && op.path == "src/include") return true;
    return false;
  }));
  EXPECT_FALSE(client->with_tree([](const MetadataTree& t) { return t.lookup("src/include/a.hpp").has_value(); }));

  // Rename.
  fs::rename(root_ / "README.md", root_ / "README.rst");
  ASSERT_TRUE(wait_for([](const auto& ops) {
    bool removed = false, added = false;
    for (const auto& op : ops) {
      removed |= op.kind == InvalidationKind::Remove && op.path == "README.md";
      added |= op.kind == InvalidationKind::Upsert && op.path == "README.rst";
    }
    return removed && added;
  }));
  EXPECT_TRUE(client->with_tree([](const MetadataTree& t) {
    return !t.lookup("README.md").has_value() && t.lookup("README.rst").has_value();
  }));

  // Rename a DIRECTORY behind the client's back. The watcher reports only the
  // directory; the agent must enumerate what it brought along, or the mirror
  // shows an empty directory until remount.
  fs::rename(root_ / "src", root_ / "src_moved");
  ASSERT_TRUE(wait_for([](const auto& ops) {
    for (const auto& op : ops)
      if (op.kind == InvalidationKind::Upsert && op.path == "src_moved/main.cpp") return true;
    return false;
  })) << "the renamed directory's contents were never announced";
  EXPECT_TRUE(client->with_tree([](const MetadataTree& t) {
    return !t.lookup("src/main.cpp").has_value() && t.lookup("src_moved/main.cpp").has_value() &&
           t.lookup("src_moved/new_file.cpp").has_value();
  }));
  EXPECT_GE(client->stats().invalidation_batches, 1u);
  EXPECT_GT(client->stats().generation, 1u);
}

// The change kind is what a notification consumer keys off, and it is decided
// in three places: the watcher names the event, the agent re-stats the path and
// may overrule it, and the client compares against its own mirror to settle
// created-versus-modified. This drives events in directly (RootServer::notify)
// rather than through a platform watcher, so it exercises the whole chain
// wherever the suite runs.
TEST_F(AgentTest, ChangeKindsReachTheClient) {
  LoopbackServer srv(root_, /*watch=*/false);
  auto client = connect_client(srv.endpoint());
  ASSERT_NE(client, nullptr);
  ASSERT_TRUE(client->connect().has_value());
  ASSERT_TRUE(client->fetch_snapshot().has_value());

  std::mutex mu;
  std::condition_variable cv;
  std::vector<RemoteRoot::AppliedChange> seen;
  client->set_invalidated_paths_hook([&](std::span<const RemoteRoot::AppliedChange> changes) {
    std::lock_guard lock(mu);
    seen.insert(seen.end(), changes.begin(), changes.end());
    cv.notify_all();
  });
  auto wait_for = [&](auto pred) {
    std::unique_lock lock(mu);
    return cv.wait_for(lock, 10s, [&] { return pred(seen); });
  };
  auto change_of = [&](std::string_view path) {
    std::lock_guard lock(mu);
    for (const auto& c : seen)
      if (c.path == path) return c.change;
    return ChangeKind::Unknown;
  };
  auto forget = [&] {
    std::lock_guard lock(mu);
    seen.clear();
  };
  auto saw = [&](std::string_view path) {
    return [path](const auto& v) {
      for (const auto& c : v)
        if (c.path == path) return true;
      return false;
    };
  };

  // A path the mirror does not have yet is a creation.
  write_file(root_ / "src" / "fresh.cpp", "// fresh\n");
  srv.server().notify(FsEvent{FsEventKind::Created, "src/fresh.cpp"});
  ASSERT_TRUE(wait_for(saw("src/fresh.cpp")));
  EXPECT_EQ(change_of("src/fresh.cpp"), ChangeKind::Created);

  // The same path again, now that the mirror has it, is a modification — even
  // though the agent reports exactly the same thing about it either time.
  forget();
  write_file(root_ / "src" / "fresh.cpp", "// fresher\n");
  srv.server().notify(FsEvent{FsEventKind::Modified, "src/fresh.cpp"});
  ASSERT_TRUE(wait_for(saw("src/fresh.cpp")));
  EXPECT_EQ(change_of("src/fresh.cpp"), ChangeKind::Modified);

  // A rename keeps both halves and the cookie that ties them together.
  forget();
  fs::rename(root_ / "src" / "fresh.cpp", root_ / "src" / "renamed.cpp");
  srv.server().notify(FsEvent{FsEventKind::RenamedFrom, "src/fresh.cpp", 1234});
  srv.server().notify(FsEvent{FsEventKind::RenamedTo, "src/renamed.cpp", 1234});
  ASSERT_TRUE(wait_for([](const auto& v) {
    bool from = false, to = false;
    for (const auto& c : v) {
      from |= c.change == ChangeKind::MovedFrom;
      to |= c.change == ChangeKind::MovedTo;
    }
    return from && to;
  }));
  {
    std::lock_guard lock(mu);
    std::uint32_t from_cookie = 0, to_cookie = 0;
    for (const auto& c : seen) {
      if (c.change == ChangeKind::MovedFrom) {
        EXPECT_EQ(c.path, "src/fresh.cpp");
        from_cookie = c.cookie;
      }
      if (c.change == ChangeKind::MovedTo) {
        EXPECT_EQ(c.path, "src/renamed.cpp");
        to_cookie = c.cookie;
      }
    }
    EXPECT_NE(from_cookie, 0u);
    EXPECT_EQ(from_cookie, to_cookie);
  }

  // A deletion names what was removed, and says whether it was a directory —
  // the mirror is the only side that still knows, since the path is gone from
  // the disk by the time anything asks.
  forget();
  fs::remove_all(root_ / "Docs");
  srv.server().notify(FsEvent{FsEventKind::Removed, "Docs"});
  ASSERT_TRUE(wait_for(saw("Docs")));
  EXPECT_EQ(change_of("Docs"), ChangeKind::Removed);
  {
    std::lock_guard lock(mu);
    for (const auto& c : seen) {
      if (c.path == "Docs") EXPECT_EQ(c.kind, NodeKind::Directory);
    }
  }
}

// A removal for a path the mirror never had changed nothing, so reporting it
// would tell a consumer a file it never saw had just been deleted.
TEST_F(AgentTest, RemovalOfAnUnknownPathIsNotReported) {
  LoopbackServer srv(root_, /*watch=*/false);
  auto client = connect_client(srv.endpoint());
  ASSERT_NE(client, nullptr);
  ASSERT_TRUE(client->connect().has_value());
  ASSERT_TRUE(client->fetch_snapshot().has_value());

  std::mutex mu;
  std::condition_variable cv;
  std::vector<RemoteRoot::AppliedChange> seen;
  int batches = 0;
  client->set_invalidated_paths_hook([&](std::span<const RemoteRoot::AppliedChange> changes) {
    std::lock_guard lock(mu);
    seen.insert(seen.end(), changes.begin(), changes.end());
    cv.notify_all();
  });
  client->set_invalidation_hook([&](const proto::InvalidationBatch&) {
    std::lock_guard lock(mu);
    ++batches;
    cv.notify_all();
  });

  srv.server().notify(FsEvent{FsEventKind::Removed, "never-existed.txt"});
  // Wait on the raw batch, which does arrive, then check that nothing was
  // reported as applied: the two hooks differ exactly here.
  {
    std::unique_lock lock(mu);
    ASSERT_TRUE(cv.wait_for(lock, 10s, [&] { return batches > 0; }));
    EXPECT_TRUE(seen.empty());
  }
}

TEST_F(AgentTest, RescanRefetchesTheSnapshot) {
  // When the agent's watcher overflows it sends a single Rescan instead of the
  // events it lost. Ignoring it left the mirror stale until remount — exactly in
  // the heavy-churn workloads (a checkout, a package install) wsldrive targets.
  // No platform watcher here, so the Rescan is the ONLY way the client can learn
  // about the change made below.
  LoopbackServer srv(root_, /*watch=*/false);
  auto c = connect_client(srv.endpoint());
  ASSERT_NE(c, nullptr);
  ASSERT_TRUE(c->connect().has_value());
  ASSERT_TRUE(c->fetch_snapshot().has_value());
  auto has = [&](std::string_view p) {
    return c->with_tree([&](const MetadataTree& t) { return t.lookup(p).has_value(); });
  };

  write_file(root_ / "late.txt", "late");
  fs::remove(root_ / "README.md");
  EXPECT_FALSE(has("late.txt")) << "nothing told the client yet";
  EXPECT_TRUE(has("README.md"));

  srv.server().notify(FsEvent{FsEventKind::Overflow, ""});

  bool refreshed = false;
  for (int i = 0; i < 200 && !refreshed; ++i) {
    std::this_thread::sleep_for(50ms);
    refreshed = has("late.txt") && !has("README.md");
  }
  EXPECT_TRUE(refreshed) << "the Rescan must trigger a re-fetch that picks up both the new and the removed file";
  EXPECT_GE(c->stats().rescans, 1u);
  // The connection is still fully usable afterwards.
  auto r = c->read("late.txt", 0, 16);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(r->data()), r->size()), "late");
}

TEST_F(AgentTest, SnapshotReplaysInvalidationsThatArriveDuringTheFetch) {
  // A change reported while a snapshot is in flight, but after the agent
  // stamped the snapshot's generation, must survive the tree replacement.
  // The scan visits the root directory first, so a file created at the root
  // once the fetch has started is (almost always) absent from the snapshot and
  // reaches the client only through the replayed invalidation. A large tree in
  // tiny frames keeps the fetch in flight long enough for that to happen; if the
  // batch happens to land after the fetch, it is applied normally — the test can
  // only pass for the right reason, never fail for the wrong one.
  for (int d = 0; d < 20; ++d) {
    fs::create_directories(root_ / "bulk" / std::to_string(d));
    for (int i = 0; i < 100; ++i) write_file(root_ / "bulk" / std::to_string(d) / (std::to_string(i) + ".txt"), "x");
  }
  LoopbackServer srv(root_, /*watch=*/false, /*chunk_bytes=*/256);
  auto c = connect_client(srv.endpoint());
  ASSERT_NE(c, nullptr);
  ASSERT_TRUE(c->connect().has_value());
  ASSERT_TRUE(c->fetch_snapshot().has_value());
  EXPECT_FALSE(c->with_tree([](const MetadataTree& t) { return t.lookup("during.txt").has_value(); }));

  std::thread fetch([&] { EXPECT_TRUE(c->fetch_snapshot().has_value()); });
  write_file(root_ / "during.txt", "d");
  srv.server().notify(FsEvent{FsEventKind::Created, "during.txt"});
  fetch.join();
  // Whatever the interleaving, the batch has been delivered by the time both the
  // fetch and (at most) one more round-trip complete.
  bool present = false;
  for (int i = 0; i < 100 && !present; ++i) {
    present = c->with_tree([](const MetadataTree& t) { return t.lookup("during.txt").has_value(); });
    if (!present) std::this_thread::sleep_for(20ms);
  }
  EXPECT_TRUE(present) << "an invalidation delivered during the fetch was lost to the tree replacement";
}
TEST_F(AgentTest, APeerThatStopsReadingDoesNotStallInvalidationsForTheOthers) {
  // broadcast() ran on the single flusher thread with no send deadline. A peer
  // that authenticated and then stopped reading filled its socket buffer and
  // parked the flusher there for good: no other peer received another
  // invalidation, and stop() hung joining the flusher.
  //
  // End-to-end cover for the two user-visible properties: the other peers keep
  // receiving, and shutdown still returns. It does not guarantee the send
  // deadline fires on any given run - whether the churn fills the stuck peer's
  // buffer depends on how the coalescer batches it. The deadline itself is
  // covered deterministically by Socket.SendToAPeerThatStopsReadingTimesOut...
  LoopbackServer srv(root_, /*watch=*/true, 4u << 20, "s3cret-token", 10s, 100000,
                     /*broadcast_timeout=*/300ms);
  if (!srv.server().watching()) GTEST_SKIP() << "no filesystem watcher on this platform";

  // The stuck peer. A tiny receive buffer makes the agent's send block after
  // very little data, so the stall does not need megabytes of churn to provoke.
  auto stuck_sock = net::connect(srv.endpoint());
  ASSERT_TRUE(stuck_sock.has_value());
  stuck_sock->set_receive_buffer(2048);
  net::FrameChannel stuck(std::move(*stuck_sock));
  {
    // The full mutual handshake by hand: nonce out, check nothing (this peer is
    // only pretending to be well behaved), prove ourselves, then stop reading.
    const std::string nonce = "client-nonce-for-the-stuck-peer";
    std::vector<std::byte> pl;
    proto::Writer w(pl);
    proto::write_hello(w, proto::Hello{.protocol_version = proto::kVersion,
                                       .capabilities = 0,
                                       .agent = "stuck/1",
                                       .nonce = nonce,
                                       .proof = {}});
    ASSERT_TRUE(stuck.send(proto::MsgType::Hello, 1, pl).has_value());
    auto ack = stuck.receive(5s);
    ASSERT_TRUE(ack.has_value());
    ASSERT_EQ(ack->header.type, proto::MsgType::HelloAck);
    proto::Reader ar(ack->payload);
    auto server_hello = proto::read_hello(ar);
    ASSERT_TRUE(server_hello.has_value());

    std::vector<std::byte> ap;
    proto::Writer aw(ap);
    proto::write_hello(aw, proto::Hello{.protocol_version = proto::kVersion,
                                        .capabilities = 0,
                                        .agent = {},
                                        .nonce = {},
                                        .proof = auth_proof("s3cret-token", nonce, server_hello->nonce, false)});
    ASSERT_TRUE(stuck.send(proto::MsgType::Auth, 2, ap).has_value());
    auto ok = stuck.receive(5s);  // reads the Ok, then never reads again
    ASSERT_TRUE(ok.has_value());
    ASSERT_EQ(ok->header.type, proto::MsgType::Ok);
  }

  // A healthy peer, draining normally.
  auto good = connect_client(srv.endpoint());
  ASSERT_NE(good, nullptr);
  good->set_auth_token("s3cret-token");
  ASSERT_TRUE(good->connect().has_value());
  ASSERT_TRUE(good->fetch_snapshot().has_value());
  const std::size_t before = good->with_tree([](const MetadataTree& t) { return t.size(); });

  // Churn well past the stuck peer's buffer. Long names so each op carries real
  // bytes; whether these arrive as per-path ops or collapse into a Rescan, the
  // healthy peer's tree has to grow either way.
  const std::string pad(80, 'p');
  for (int i = 0; i < 500; ++i) write_file(root_ / ("churn-" + pad + std::to_string(i) + ".txt"), "x");

  bool progressed = false;
  for (int i = 0; i < 300 && !progressed; ++i) {
    std::this_thread::sleep_for(50ms);
    progressed = good->with_tree([](const MetadataTree& t) { return t.size(); }) > before + 400;
  }
  EXPECT_TRUE(progressed) << "a peer that stopped reading must not stop delivery to everyone else";
  // LoopbackServer's destructor calls stop(), which hung here before the fix -
  // reaching the end of this test is the other half of what is being asserted.
}

TEST_F(AgentTest, AbsurdOffsetsAndSizesAreRefusedRatherThanActedOn) {
  // The seek calls take a signed offset, so anything past the signed range
  // wraps negative and the seek fails - and an unchecked failure leaves the
  // stream at position 0, so a write meant for a huge offset silently lands at
  // the start of the file instead. A truncate has the matching problem in the
  // other direction: nothing stopped it asking for an 8 EiB sparse file.
  LoopbackServer srv(root_, /*watch=*/false);
  auto c = connect_client(srv.endpoint());
  ASSERT_NE(c, nullptr);
  ASSERT_TRUE(c->connect().has_value());
  ASSERT_TRUE(c->fetch_snapshot().has_value());

  const std::uint64_t absurd = std::numeric_limits<std::uint64_t>::max() - 4096;
  EXPECT_FALSE(c->write("README.md", absurd, as_bytes("boom")).has_value());
  EXPECT_FALSE(c->truncate("README.md", absurd).has_value());

  // The file is untouched: the rejected write must not have landed at offset 0.
  auto after = c->read("README.md", 0, 100);
  ASSERT_TRUE(after.has_value());
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(after->data()), after->size()), "# readme\n");

  // Ordinary offsets still work.
  ASSERT_TRUE(c->write("README.md", 2, as_bytes("XX")).has_value());
  auto edited = c->read("README.md", 0, 100);
  ASSERT_TRUE(edited.has_value());
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(edited->data()), edited->size()), "# XXadme\n");
}

TEST_F(AgentTest, ClientRefusesAnAgentThatCannotProveTheSharedSecret) {
  // The impostor case. Anything that binds the endpoint first used to be handed
  // the shared secret by the client, and could then serve a tree of its own
  // choosing into a drive the user trusts. The client must check the agent
  // before it says anything, and walk away when the proof does not hold up.
  LoopbackServer impostor(root_, /*watch=*/false, 4u << 20, "not-the-real-secret");
  auto c = connect_client(impostor.endpoint());
  ASSERT_NE(c, nullptr);
  c->set_auth_token("the-real-secret");

  auto hello = c->connect();
  ASSERT_FALSE(hello.has_value()) << "an agent that cannot prove the secret must be refused";
  EXPECT_EQ(hello.error(), Errc::Unauthorized);

  // And nothing is served over that session.
  EXPECT_FALSE(c->fetch_snapshot().has_value());
}

TEST_F(AgentTest, ClientProvesNothingToAnAgentThatFailedItsOwnProof) {
  // The ordering is the security property, not just the refusal. The client has
  // to conclude from the HelloAck alone that this is not our agent, and send
  // nothing further - the previous handshake handed over the shared secret in
  // the opening frame, before it knew anything at all about the peer.
  auto listener = net::Listener::bind(*net::Endpoint::parse("tcp://127.0.0.1:0"));
  ASSERT_TRUE(listener.has_value());
  const auto ep = listener->local();

  std::atomic<bool> saw_auth{false};
  std::thread impostor([&] {
    auto sock = listener->accept(std::chrono::seconds(5));
    if (!sock) return;
    net::FrameChannel ch(std::move(*sock));
    auto hello = ch.receive(std::chrono::seconds(5));
    if (!hello) return;
    // A proof we could not possibly compute without the secret.
    std::vector<std::byte> pl;
    proto::Writer w(pl);
    proto::write_hello(w, proto::Hello{.protocol_version = proto::kVersion,
                                       .capabilities = 0,
                                       .agent = "impostor/1",
                                       .nonce = "impostor-nonce",
                                       .proof = std::string(64, '0')});
    (void)ch.send(proto::MsgType::HelloAck, hello->header.request_id, pl);
    // Anything arriving now would be the client proving itself to an impostor.
    if (auto f = ch.receive(std::chrono::seconds(2)); f && f->header.type == proto::MsgType::Auth)
      saw_auth.store(true);
  });

  auto c = connect_client(ep);
  ASSERT_NE(c, nullptr);
  c->set_auth_token("the-real-secret");
  auto hello = c->connect();
  EXPECT_FALSE(hello.has_value());
  EXPECT_EQ(hello.error(), Errc::Unauthorized);
  impostor.join();
  EXPECT_FALSE(saw_auth.load()) << "the client must not prove itself to an agent it has not verified";
}

TEST_F(AgentTest, MatchingSecretsAuthenticateBothWays) {
  LoopbackServer srv(root_, /*watch=*/false, 4u << 20, "shared-secret");
  auto c = connect_client(srv.endpoint());
  ASSERT_NE(c, nullptr);
  c->set_auth_token("shared-secret");
  ASSERT_TRUE(c->connect().has_value());
  ASSERT_TRUE(c->fetch_snapshot().has_value());
  EXPECT_TRUE(c->with_tree([](const MetadataTree& t) { return t.lookup("README.md").has_value(); }));
}

TEST_F(AgentTest, TheSharedSecretIsNeverPutOnTheWire) {
  // The opening Hello used to carry the secret itself, which is how an impostor
  // collected it. What goes out now is a nonce and a derived proof.
  const std::string secret = "an-unmistakable-secret-value";
  std::vector<std::byte> pl;
  proto::Writer w(pl);
  proto::write_hello(w, proto::Hello{.protocol_version = proto::kVersion,
                                     .capabilities = 0,
                                     .agent = "wsldrive",
                                     .nonce = "client-nonce",
                                     .proof = auth_proof(secret, "client-nonce", "server-nonce", false)});
  const std::string bytes(reinterpret_cast<const char*>(pl.data()), pl.size());
  EXPECT_EQ(bytes.find(secret), std::string::npos);
}

TEST_F(AgentTest, ReadCacheHitsOnADifferentlyCasedSpelling) {
  // The mount resolves case-insensitively (a Windows volume has to), so a
  // program opening `readme.md` for a stored `README.md` reaches the client with
  // its own spelling. Keyed on that spelling the cache missed on every such
  // open, the read-ahead was bypassed, and the request went out for a path a
  // case-sensitive agent does not have.
  LoopbackServer srv(root_, /*watch=*/false);
  auto c = connect_client(srv.endpoint());
  ASSERT_NE(c, nullptr);
  c->set_lookup_mode(LookupMode::CaseInsensitive);
  ASSERT_TRUE(c->connect().has_value());
  ASSERT_TRUE(c->fetch_snapshot().has_value());
  const auto str = [](const std::vector<std::byte>& v) {
    return std::string(reinterpret_cast<const char*>(v.data()), v.size());
  };

  auto stored = c->read("README.md", 0, 100);  // seeded "# readme\n"
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(str(*stored), "# readme\n");
  EXPECT_EQ(c->stats().read_cache_misses, 1u);

  auto lower = c->read("readme.md", 0, 100);
  ASSERT_TRUE(lower.has_value()) << "a differently cased path must resolve to the stored file";
  EXPECT_EQ(str(*lower), "# readme\n");
  EXPECT_EQ(c->stats().read_cache_hits, 1u) << "and be served by the entry the first read cached";
  EXPECT_EQ(c->stats().read_cache_misses, 1u);

  // read_into() resolves the same way, so it reaches the same entry.
  std::vector<std::byte> buf(100);
  auto n = c->read_into("ReadMe.MD", 0, buf);
  ASSERT_TRUE(n.has_value());
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(buf.data()), *n), "# readme\n");
  EXPECT_EQ(c->stats().read_cache_hits, 2u);
}

TEST_F(AgentTest, WriteAndTruncateMoveTheMirrorVersionOn) {
  // A read-ahead already in flight when a write lands can be holding pre-write
  // bytes and about to cache them under the old (mtime, size). If the mirror
  // still agreed with that pair the entry would validate and serve stale
  // content - until the watcher's event, or for good without a watcher. Moving
  // the version on makes that entry fail validation instead.
  LoopbackServer srv(root_, /*watch=*/false);
  auto c = connect_client(srv.endpoint());
  ASSERT_NE(c, nullptr);
  ASSERT_TRUE(c->connect().has_value());
  ASSERT_TRUE(c->fetch_snapshot().has_value());
  const auto mtime_of = [&](std::string_view p) {
    return c->with_tree([&](const MetadataTree& t) -> std::int64_t {
      const auto id = t.lookup(p);
      return id ? t.node(*id).attr.mtime_ns : 0;
    });
  };

  const std::int64_t before = mtime_of("README.md");
  ASSERT_TRUE(c->write("README.md", 0, as_bytes("XXXXX")).has_value());  // same length on purpose
  const std::int64_t after_write = mtime_of("README.md");
  EXPECT_GT(after_write, before) << "a same-length write must still change the version";

  ASSERT_TRUE(c->truncate("README.md", 3).has_value());
  EXPECT_GT(mtime_of("README.md"), after_write);
}

TEST_F(AgentTest, ACaseOnlyRenameKeepsTheDirectoryAndItsSubtree) {
  // Once paths resolve case-insensitively, the destination of `src` -> `SRC`
  // resolves to the source. Treating that as "something is already there,
  // remove it" would delete the very thing being renamed, subtree and all.
  LoopbackServer srv(root_, /*watch=*/false);
  auto c = connect_client(srv.endpoint());
  ASSERT_NE(c, nullptr);
  c->set_lookup_mode(LookupMode::CaseInsensitive);
  ASSERT_TRUE(c->connect().has_value());
  ASSERT_TRUE(c->fetch_snapshot().has_value());
  const auto has = [&](std::string_view p) {
    return c->with_tree([&](const MetadataTree& t) { return t.lookup(p, LookupMode::Exact).has_value(); });
  };
  ASSERT_TRUE(has("src/include/a.hpp"));

  ASSERT_TRUE(c->rename("src", "SRC").has_value());
  EXPECT_TRUE(has("SRC/include/a.hpp")) << "the subtree must move with the directory, not be deleted";
  EXPECT_FALSE(has("src/include/a.hpp"));
}

// POSIX lets a directory be renamed onto an existing empty one; Windows does
// not, so the agent would fail the operation before the mirror is reached.
#ifndef _WIN32
TEST_F(AgentTest, RenamingADirectoryOntoAnExistingOneKeepsTheSubtree) {
  // The destination can already be present in the mirror - an invalidation
  // describing this very rename can be applied between the agent's reply and
  // the client's own update, and it may create the destination before the agent
  // has expanded its children. The mirror update has to move the source node
  // over it: the source is what actually holds the subtree, so dropping it (the
  // old fallback) lost every child until the next rescan.
  LoopbackServer srv(root_, /*watch=*/false);
  auto c = connect_client(srv.endpoint());
  ASSERT_NE(c, nullptr);
  ASSERT_TRUE(c->connect().has_value());
  ASSERT_TRUE(c->fetch_snapshot().has_value());
  const auto has = [&](std::string_view p) {
    return c->with_tree([&](const MetadataTree& t) { return t.lookup(p).has_value(); });
  };
  ASSERT_TRUE(has("src/include/a.hpp"));

  ASSERT_TRUE(c->mkdir("dst").has_value());  // destination exists, and is empty
  ASSERT_TRUE(has("dst"));

  ASSERT_TRUE(c->rename("src", "dst").has_value());
  EXPECT_TRUE(has("dst/include/a.hpp")) << "the subtree must survive landing on an existing destination";
  EXPECT_FALSE(has("src/include/a.hpp"));
}
#endif

TEST_F(AgentTest, RenamingADirectoryEvictsItsSubtreeFromTheReadCache) {
  // The content cache is keyed by path, so moving a directory leaves every
  // entry below the old prefix cached but unreachable: dead weight against the
  // byte budget until LRU pressure happened to evict it.
  LoopbackServer srv(root_, /*watch=*/false);
  auto c = connect_client(srv.endpoint());
  ASSERT_NE(c, nullptr);
  ASSERT_TRUE(c->connect().has_value());
  ASSERT_TRUE(c->fetch_snapshot().has_value());

  ASSERT_TRUE(c->read("src/main.cpp", 0, 100).has_value());
  ASSERT_TRUE(c->read("src/include/a.hpp", 0, 100).has_value());
  EXPECT_GT(c->stats().read_cache_bytes, 0u);

  ASSERT_TRUE(c->rename("src", "src2").has_value());
  EXPECT_EQ(c->stats().read_cache_bytes, 0u) << "the subtree's contents must leave the cache with it";

  auto r = c->read("src2/main.cpp", 0, 100);  // still readable under the new path
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(r->data()), r->size()), "int main() {}\n");
}

TEST_F(AgentTest, RemovingADirectoryEvictsItsSubtreeFromTheReadCache) {
  LoopbackServer srv(root_, /*watch=*/false);
  auto c = connect_client(srv.endpoint());
  ASSERT_NE(c, nullptr);
  ASSERT_TRUE(c->connect().has_value());
  ASSERT_TRUE(c->fetch_snapshot().has_value());

  ASSERT_TRUE(c->read("src/include/a.hpp", 0, 100).has_value());
  EXPECT_GT(c->stats().read_cache_bytes, 0u);

  ASSERT_TRUE(c->unlink("src/include/a.hpp").has_value());
  ASSERT_TRUE(c->rmdir("src/include").has_value());
  EXPECT_EQ(c->stats().read_cache_bytes, 0u);
}

// Creating a symlink needs a privilege (or Developer Mode) on Windows, so this
// runs where the rest of the symlink coverage does - see ScannerReportsSymlinks.
#ifndef _WIN32
TEST_F(AgentTest, SymlinkSizeIsTheTargetLength) {
  // POSIX says st_size of a symlink is its target's length, and the readlink
  // idiom is malloc(st_size + 1): a reported 0 hands the caller a buffer too
  // small for any target at all.
  std::error_code ec;
  constexpr std::string_view kTarget = "main.cpp";
  fs::create_symlink(kTarget, root_ / "src" / "link_to_main", ec);
  ASSERT_FALSE(ec);
  std::vector<std::string> names;
  std::vector<SnapshotEntry> entries;
  auto stats = scan_tree(root_, [&](const SnapshotEntry& e) {
    names.emplace_back(e.name);
    entries.push_back(e);
  });
  ASSERT_TRUE(stats.has_value());
  for (std::size_t i = 0; i < entries.size(); ++i) entries[i].name = names[i];
  MetadataTree t;
  ASSERT_TRUE(t.load_snapshot(entries).has_value());
  const auto link = t.lookup("src/link_to_main");
  ASSERT_TRUE(link.has_value());
  ASSERT_EQ(t.node(*link).attr.kind, NodeKind::Symlink);
  EXPECT_EQ(t.node(*link).attr.size, kTarget.size());
}
#endif


#ifdef _WIN32
TEST_F(AgentTest, MutationsRetryWhileAnotherProcessBrieflyHoldsTheFile) {
  // Windows: the search indexer or an antivirus scanner opens a just-written
  // file for a moment without FILE_SHARE_DELETE. A rename or delete landing in
  // that window fails with a sharing violation - which is precisely git's
  // lock-file dance, and it made `git init` abort on the mount one run in three.
  // The agent must ride it out the way Git for Windows does.
  LoopbackServer srv(root_, /*watch=*/false);
  auto c = connect_client(srv.endpoint());
  ASSERT_NE(c, nullptr);
  ASSERT_TRUE(c->connect().has_value());
  ASSERT_TRUE(c->fetch_snapshot().has_value());
  write_file(root_ / "config.lock", "new");
  write_file(root_ / "config", "old");
  write_file(root_ / "doomed", "x");

  // Hold both targets open with read sharing only, then let go after a while.
  auto hold = [&](const fs::path& p) {
    return ::CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  };
  HANDLE h1 = hold(root_ / "config");
  HANDLE h2 = hold(root_ / "doomed");
  ASSERT_NE(h1, INVALID_HANDLE_VALUE);
  ASSERT_NE(h2, INVALID_HANDLE_VALUE);
  std::thread releaser([&] {
    std::this_thread::sleep_for(150ms);
    ::CloseHandle(h1);
    ::CloseHandle(h2);
  });

  const auto t0 = std::chrono::steady_clock::now();
  auto renamed = c->rename("config.lock", "config");
  auto unlinked = c->unlink("doomed");
  releaser.join();
  EXPECT_TRUE(renamed.has_value()) << "rename over a briefly-held file must succeed: " << (renamed ? "" : to_string(renamed.error()));
  EXPECT_TRUE(unlinked.has_value()) << "delete of a briefly-held file must succeed";
  EXPECT_EQ(read_disk(root_ / "config"), "new");
  EXPECT_FALSE(fs::exists(root_ / "doomed"));
  EXPECT_LT(std::chrono::steady_clock::now() - t0, 5s) << "retry must give up within a reasonable time";

  // A READ-ONLY target is a permanent refusal, not a held-open file: it must
  // fail fast, not stall the mount for the whole retry budget (git marks its
  // pack files read-only, so this is a common case).
  write_file(root_ / "ro.txt", "ro");
  write_file(root_ / "over.lock", "n");
  ASSERT_TRUE(::SetFileAttributesW((root_ / "ro.txt").c_str(), FILE_ATTRIBUTE_READONLY));
  const auto t1 = std::chrono::steady_clock::now();
  auto refused = c->rename("over.lock", "ro.txt");
  const auto took = std::chrono::steady_clock::now() - t1;
  EXPECT_FALSE(refused.has_value()) << "renaming over a read-only file is refused by Windows";
  EXPECT_LT(took, 150ms) << "a permanent refusal must not be retried for the full budget";
  ::SetFileAttributesW((root_ / "ro.txt").c_str(), FILE_ATTRIBUTE_NORMAL);
}

TEST(WslLaunch, BuildsCommand) {
  const std::string cmd = platform::build_wsl_command(
      {.distro = "Ubuntu", .agent_path = "/opt/wsldrived", .wsl_root = "/home/u/proj", .port = 51789});
  EXPECT_NE(cmd.find("wsl.exe -d Ubuntu -- "), std::string::npos);
  EXPECT_NE(cmd.find("'/opt/wsldrived'"), std::string::npos);
  EXPECT_NE(cmd.find("--root '/home/u/proj'"), std::string::npos);
  EXPECT_NE(cmd.find("--listen tcp://127.0.0.1:51789"), std::string::npos);
  EXPECT_NE(cmd.find("--exit-when-idle"), std::string::npos);

  // Default distro is omitted; default agent falls back to PATH lookup.
  const std::string dflt = platform::build_wsl_command({.distro = "", .agent_path = "", .wsl_root = "/x", .port = 1});
  EXPECT_EQ(dflt.find(" -d "), std::string::npos);
  EXPECT_NE(dflt.find("-- 'wsldrived'"), std::string::npos);

  // A single quote in a path is escaped so the distro shell keeps it literal.
  const std::string q = platform::build_wsl_command({.distro = "", .agent_path = "", .wsl_root = "/a'b", .port = 1});
  EXPECT_NE(q.find("'/a'\\''b'"), std::string::npos);

  // An explicit listen endpoint (e.g. vsock for the Hyper-V socket transport) is used verbatim.
  const std::string v = platform::build_wsl_command(
      {.distro = "Ubuntu", .agent_path = "", .wsl_root = "/x", .port = 5700, .listen = "vsock://any:5700"});
  EXPECT_NE(v.find("--listen vsock://any:5700"), std::string::npos);
  EXPECT_EQ(v.find("tcp://"), std::string::npos);
}

#else
TEST(WinLaunch, BuildsCommand) {
  const std::string cmd = platform::build_win_agent_command(
      {.exe = "/mnt/c/wsldrived.exe", .win_root = "C:/proj", .connect = "hv://{guid}:5700", .token = {}});
  EXPECT_NE(cmd.find("/mnt/c/wsldrived.exe"), std::string::npos);
  EXPECT_NE(cmd.find("--root C:/proj"), std::string::npos);
  EXPECT_NE(cmd.find("--connect hv://{guid}:5700"), std::string::npos);
  EXPECT_NE(cmd.find("--exit-when-idle"), std::string::npos);
}
#endif

#ifdef _WIN32
TEST(Win32Watcher, ReportsEventsAndStopsCleanly) {
  const fs::path root = fs::temp_directory_path() / "wsldrive-watcher-test";
  fs::remove_all(root);
  fs::create_directories(root / "sub");

  std::mutex mu;
  std::condition_variable cv;
  std::vector<std::pair<FsEventKind, std::string>> events;
  auto w = platform::make_watcher(root, [&](const FsEvent& ev) {
    std::lock_guard lock(mu);
    events.emplace_back(ev.kind, std::string(ev.path));
    cv.notify_all();
  });
  ASSERT_TRUE(w.has_value());

  write_file(root / "sub" / "f.txt", "x");
  {
    std::unique_lock lock(mu);
    ASSERT_TRUE(cv.wait_for(lock, 5s, [&] {
      for (const auto& [k, p] : events)
        if (k == FsEventKind::Created && p == "sub/f.txt") return true;
      return false;
    }));
  }
  (*w)->stop();
  (*w)->stop();  // idempotent
  w->reset();
  fs::remove_all(root);
  EXPECT_FALSE(platform::make_watcher(root / "does-not-exist", [](const FsEvent&) {}).has_value());
}
#endif

}  // namespace
}  // namespace wsld::agent
