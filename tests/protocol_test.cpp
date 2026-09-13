#include "core/protocol.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace wsld::proto {
namespace {

constexpr Attributes kFile{.size = 1234567, .mtime_ns = -5, .mode = 0644, .kind = NodeKind::File};
constexpr Attributes kDir{.size = 0, .mtime_ns = 1700000000123456789LL, .mode = 0755, .kind = NodeKind::Directory};

TEST(Frame, HeaderRoundTrip) {
  FrameHeader h;
  h.type = MsgType::Snapshot;
  h.flags = 0xABCD;
  h.payload_len = 77;
  h.request_id = 0x0102030405060708ULL;
  std::array<std::byte, kHeaderSize> buf{};
  encode_header(h, buf);
  EXPECT_EQ(std::to_integer<char>(buf[0]), 'W');
  EXPECT_EQ(std::to_integer<char>(buf[1]), 'S');
  EXPECT_EQ(std::to_integer<char>(buf[2]), 'L');
  EXPECT_EQ(std::to_integer<char>(buf[3]), 'D');
  const auto decoded = decode_header(buf);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(*decoded, h);
}

TEST(Frame, HeaderErrors) {
  std::array<std::byte, kHeaderSize> buf{};
  EXPECT_EQ(decode_header(std::span(buf).first(10)).error(), Errc::Truncated);
  EXPECT_EQ(decode_header(buf).error(), Errc::BadMagic);
  FrameHeader h;
  encode_header(h, buf);
  buf[4] = std::byte{99};  // version
  EXPECT_EQ(decode_header(buf).error(), Errc::UnsupportedVersion);
  h.payload_len = kMaxPayload + 1;
  encode_header(h, buf);
  EXPECT_EQ(decode_header(buf).error(), Errc::TooLarge);
}

TEST(Primitives, VarintRoundTrip) {
  std::vector<std::byte> buf;
  Writer w(buf);
  const std::uint64_t values[] = {0, 1, 127, 128, 300, 16383, 16384, 0xFFFFFFFFULL, ~0ULL};
  for (const auto v : values) w.varint(v);
  Reader r(buf);
  for (const auto v : values) {
    auto got = r.varint();
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, v);
  }
  EXPECT_TRUE(r.empty());
}

TEST(Primitives, VarintErrors) {
  const std::vector<std::byte> truncated{std::byte{0x80}, std::byte{0x80}};
  EXPECT_EQ(Reader(truncated).varint().error(), Errc::Truncated);
  std::vector<std::byte> too_long(11, std::byte{0x80});
  EXPECT_EQ(Reader(too_long).varint().error(), Errc::Corrupt);
  std::vector<std::byte> overflow(9, std::byte{0xFF});
  overflow.push_back(std::byte{0x02});  // 10th byte carries more than one bit
  EXPECT_EQ(Reader(overflow).varint().error(), Errc::Corrupt);
}

TEST(Primitives, FixedWidthAndStrings) {
  std::vector<std::byte> buf;
  Writer w(buf);
  w.u8(0xAB);
  w.u16(0xBEEF);
  w.u32(0xDEADBEEF);
  w.u64(0x0123456789ABCDEFULL);
  w.i64(-42);
  w.string("héllo");
  w.string("");
  const std::byte blob_bytes[] = {std::byte{1}, std::byte{2}, std::byte{3}};
  w.blob(blob_bytes);
  EXPECT_EQ(std::to_integer<int>(buf[1]), 0xEF);  // little-endian

  Reader r(buf);
  EXPECT_EQ(*r.u8(), 0xAB);
  EXPECT_EQ(*r.u16(), 0xBEEF);
  EXPECT_EQ(*r.u32(), 0xDEADBEEFu);
  EXPECT_EQ(*r.u64(), 0x0123456789ABCDEFULL);
  EXPECT_EQ(*r.i64(), -42);
  EXPECT_EQ(*r.string(), "héllo");
  EXPECT_EQ(*r.string(), "");
  auto blob = r.blob();
  ASSERT_TRUE(blob.has_value());
  EXPECT_EQ(blob->size(), 3u);
  EXPECT_EQ(std::to_integer<int>((*blob)[2]), 3);
  EXPECT_TRUE(r.empty());
  EXPECT_EQ(r.u8().error(), Errc::Truncated);
}

TEST(Primitives, StringLengthBeyondBufferIsTruncated) {
  std::vector<std::byte> buf;
  Writer w(buf);
  w.varint(1000);
  w.u8('x');
  EXPECT_EQ(Reader(buf).string().error(), Errc::Truncated);
}

TEST(Messages, Hello) {
  std::vector<std::byte> buf;
  Writer w(buf);
  write_hello(w, Hello{.protocol_version = 1, .capabilities = 0xF0F0, .agent = "wsldrived/0.1", .nonce = "n1", .proof = "p1"});
  Reader r(buf);
  auto h = read_hello(r);
  ASSERT_TRUE(h.has_value());
  EXPECT_EQ(h->protocol_version, 1u);
  EXPECT_EQ(h->capabilities, 0xF0F0u);
  EXPECT_EQ(h->agent, "wsldrived/0.1");
}

TEST(Messages, AttributesRoundTrip) {
  std::vector<std::byte> buf;
  Writer w(buf);
  write_attributes(w, kFile);
  write_attributes(w, kDir);
  Reader r(buf);
  EXPECT_EQ(*read_attributes(r), kFile);
  EXPECT_EQ(*read_attributes(r), kDir);

  buf.clear();
  w.u8(9);  // invalid kind
  Reader bad(buf);
  EXPECT_EQ(read_attributes(bad).error(), Errc::Corrupt);
}

TEST(Messages, SnapshotRoundTrip) {
  std::vector<std::byte> buf;
  Writer w(buf);
  write_snapshot_header(w, 7, 3);
  write_snapshot_entry(w, SnapshotEntry{0, "src", kDir});
  write_snapshot_entry(w, SnapshotEntry{1, "main.cpp", kFile});
  write_snapshot_entry(w, SnapshotEntry{0, "README", kFile});

  Reader r(buf);
  std::vector<std::string> names;
  std::vector<std::uint32_t> parents;
  auto gen = read_snapshot(r, [&](const SnapshotEntry& e) {
    names.emplace_back(e.name);
    parents.push_back(e.parent);
  });
  ASSERT_TRUE(gen.has_value());
  EXPECT_EQ(*gen, 7u);
  EXPECT_EQ(names, (std::vector<std::string>{"src", "main.cpp", "README"}));
  EXPECT_EQ(parents, (std::vector<std::uint32_t>{0, 1, 0}));
  EXPECT_TRUE(r.empty());
}

TEST(Messages, SnapshotFeedsTree) {
  MetadataTree src;
  const NodeId d = *src.insert(src.root(), "dir", kDir);
  (void)src.insert(d, "f", kFile);
  std::vector<std::byte> buf;
  Writer w(buf);
  write_snapshot_header(w, 1, static_cast<std::uint32_t>(src.size() - 1));
  src.export_snapshot([&](const SnapshotEntry& e) { write_snapshot_entry(w, e); });

  MetadataTree dst;
  std::vector<SnapshotEntry> entries;  // views into buf stay valid while buf lives
  Reader r(buf);
  ASSERT_TRUE(read_snapshot(r, [&](const SnapshotEntry& e) { entries.push_back(e); }).has_value());
  ASSERT_TRUE(dst.load_snapshot(entries).has_value());
  EXPECT_EQ(dst.node(*dst.lookup("dir/f")).attr, kFile);
}

TEST(Messages, SnapshotHeaderRejectsImpossibleCount) {
  std::vector<std::byte> buf;
  Writer w(buf);
  write_snapshot_header(w, 1, 1000000);
  Reader r(buf);
  EXPECT_EQ(read_snapshot_header(r).error(), Errc::Corrupt);
}

TEST(Messages, InvalidationRoundTrip) {
  InvalidationBatch b;
  b.generation = 99;
  b.ops.push_back(InvalidationOp{InvalidationKind::Upsert, "a/b.txt", kFile});
  b.ops.push_back(InvalidationOp{InvalidationKind::Remove, "gone", {}});
  b.ops.push_back(InvalidationOp{InvalidationKind::Rescan, "", {}});
  std::vector<std::byte> buf;
  Writer w(buf);
  write_invalidation(w, b);
  Reader r(buf);
  auto got = read_invalidation(r);
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got->generation, 99u);
  ASSERT_EQ(got->ops.size(), 3u);
  EXPECT_EQ(got->ops[0].kind, InvalidationKind::Upsert);
  EXPECT_EQ(got->ops[0].path, "a/b.txt");
  EXPECT_EQ(got->ops[0].attr, kFile);
  EXPECT_EQ(got->ops[1].kind, InvalidationKind::Remove);
  EXPECT_EQ(got->ops[1].path, "gone");
  EXPECT_EQ(got->ops[2].kind, InvalidationKind::Rescan);
  EXPECT_TRUE(r.empty());
  for (const auto& op : got->ops) {
    EXPECT_EQ(op.change, ChangeKind::Unknown);  // default when the sender says nothing
    EXPECT_EQ(op.cookie, 0u);
  }
}

TEST(Messages, InvalidationCarriesChangeKindAndMoveCookie) {
  InvalidationBatch b;
  b.generation = 7;
  b.ops.push_back(InvalidationOp{InvalidationKind::Upsert, "made", kFile, ChangeKind::Created, 0});
  b.ops.push_back(InvalidationOp{InvalidationKind::Upsert, "written", kFile, ChangeKind::Modified, 0});
  b.ops.push_back(InvalidationOp{InvalidationKind::Remove, "old", {}, ChangeKind::MovedFrom, 4242});
  b.ops.push_back(InvalidationOp{InvalidationKind::Upsert, "new", kFile, ChangeKind::MovedTo, 4242});
  b.ops.push_back(InvalidationOp{InvalidationKind::Remove, "deleted", {}, ChangeKind::Removed, 0});

  std::vector<std::byte> buf;
  Writer w(buf);
  write_invalidation(w, b);
  Reader r(buf);
  auto got = read_invalidation(r);
  ASSERT_TRUE(got.has_value());
  ASSERT_EQ(got->ops.size(), 5u);
  EXPECT_EQ(got->ops[0].change, ChangeKind::Created);
  EXPECT_EQ(got->ops[1].change, ChangeKind::Modified);
  EXPECT_EQ(got->ops[2].change, ChangeKind::MovedFrom);
  EXPECT_EQ(got->ops[3].change, ChangeKind::MovedTo);
  EXPECT_EQ(got->ops[4].change, ChangeKind::Removed);
  EXPECT_EQ(got->ops[2].cookie, 4242u);
  EXPECT_EQ(got->ops[3].cookie, 4242u);
  // The cookie rides along only for the two move kinds, so a batch without a
  // move costs exactly one byte per op more than it used to.
  EXPECT_EQ(got->ops[0].cookie, 0u);
  EXPECT_EQ(got->ops[4].cookie, 0u);
  EXPECT_TRUE(r.empty());
}

TEST(Messages, InvalidationRejectsAnUnknownChangeKind) {
  std::vector<std::byte> buf;
  Writer w(buf);
  w.u64(1);       // generation
  w.varint(1);    // one op
  w.u8(static_cast<std::uint8_t>(InvalidationKind::Remove));
  w.string("p");
  w.u8(200);      // not a ChangeKind
  Reader r(buf);
  EXPECT_EQ(read_invalidation(r).error(), Errc::Corrupt);
}

TEST(Messages, HugeClaimedCountsFailWithoutHugeAllocation) {
  // A decoder must not size a container from a count it has not validated: a
  // small frame claiming millions of elements would otherwise commit gigabytes
  // before the first element is read. Each of these lies about its count and
  // must fail cleanly (and promptly) instead.
  {  // ReadManyRequest: count far beyond the bytes that follow
    std::vector<std::byte> buf;
    Writer w(buf);
    w.varint(50'000'000);  // claimed paths
    w.string("only-one");
    Reader r(buf);
    EXPECT_FALSE(read_read_many_request(r).has_value());
  }
  {  // Invalidation: same shape, decoded by the *client*
    std::vector<std::byte> buf;
    Writer w(buf);
    w.u64(1);              // generation
    w.varint(40'000'000);  // claimed ops
    Reader r(buf);
    EXPECT_FALSE(read_invalidation(r).has_value());
  }
  {  // ReadManyResponse: also client-side
    std::vector<std::byte> buf;
    Writer w(buf);
    w.varint(40'000'000);  // claimed items
    Reader r(buf);
    EXPECT_FALSE(read_read_many_response(r).has_value());
  }
}

TEST(Messages, ReadManyRequestIsCappedEvenWhenTheBytesAreThere) {
  // The "count beyond the bytes that follow" check above is not enough on its
  // own: a full 64 MiB frame of one-byte paths carries its own bytes and would
  // decode to millions of entries, each of which the agent then stats. Bound
  // the count itself. The client's read-ahead never sends more than 512.
  const std::size_t over = kMaxReadManyPaths + 1;
  std::vector<std::byte> buf;
  Writer w(buf);
  w.varint(over);
  for (std::size_t i = 0; i < over; ++i) w.string("f");  // genuinely present
  Reader r(buf);
  auto q = read_read_many_request(r);
  EXPECT_FALSE(q.has_value()) << "a request past the cap must be refused, not decoded";

  // One at the cap still decodes, so the limit is not off by one.
  std::vector<std::byte> ok_buf;
  Writer ok_w(ok_buf);
  ok_w.varint(kMaxReadManyPaths);
  for (std::size_t i = 0; i < kMaxReadManyPaths; ++i) ok_w.string("f");
  Reader ok_r(ok_buf);
  auto good = read_read_many_request(ok_r);
  ASSERT_TRUE(good.has_value());
  EXPECT_EQ(good->paths.size(), kMaxReadManyPaths);
}

TEST(Messages, ReadRequestAndResponse) {
  std::vector<std::byte> buf;
  Writer w(buf);
  write_read_request(w, ReadRequest{.path = "x/y", .offset = 4096, .length = 65536});
  const std::string payload = "file contents";
  write_read_response(w, ReadResponse{.file_size = 13, .data = std::as_bytes(std::span{payload})});
  Reader r(buf);
  auto q = read_read_request(r);
  ASSERT_TRUE(q.has_value());
  EXPECT_EQ(q->path, "x/y");
  EXPECT_EQ(q->offset, 4096u);
  EXPECT_EQ(q->length, 65536u);
  auto p = read_read_response(r);
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->file_size, 13u);
  EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(p->data.data()), p->data.size()), payload);
}

TEST(Messages, ErrorRoundTrip) {
  std::vector<std::byte> buf;
  Writer w(buf);
  write_error(w, ErrorMessage{.code = static_cast<std::uint32_t>(Errc::NotFound), .detail = "no such file"});
  Reader r(buf);
  auto e = read_error(r);
  ASSERT_TRUE(e.has_value());
  EXPECT_EQ(e->code, static_cast<std::uint32_t>(Errc::NotFound));
  EXPECT_EQ(e->detail, "no such file");
}

// Feeds pseudo-random buffers to every decoder to prove they never read out of
// bounds or loop: each must return a value or an Errc, and consume no more than
// the input. Deterministic PRNG so failures reproduce.
TEST(Fuzz, DecodersAreBounded) {
  std::uint64_t state = 0x123456789abcdef0ULL;
  auto next = [&] {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
  };
  for (int iter = 0; iter < 20000; ++iter) {
    const std::size_t n = next() % 300;
    std::vector<std::byte> buf(n);
    for (auto& b : buf) b = static_cast<std::byte>(next() & 0xFF);

    (void)decode_header(buf);
    auto run = [&](auto fn) {
      Reader r(buf);
      auto res = fn(r);
      EXPECT_LE(r.position(), buf.size());
      (void)res;
    };
    run([](Reader& r) { return read_hello(r); });
    run([](Reader& r) { return read_attributes(r); });
    run([](Reader& r) { return read_snapshot_header(r); });
    run([](Reader& r) { return read_snapshot_entry(r); });
    run([](Reader& r) { return read_invalidation(r); });
    run([](Reader& r) { return read_read_request(r); });
    run([](Reader& r) { return read_read_response(r); });
    run([](Reader& r) { return read_write_request(r); });
    run([](Reader& r) { return read_create_request(r); });
    run([](Reader& r) { return read_rename_request(r); });
    run([](Reader& r) { return read_truncate_request(r); });
    run([](Reader& r) { return read_error(r); });
  }
}

TEST(Messages, MutationAckRoundTrip) {
  std::vector<std::byte> buf;
  Writer w(buf);
  write_write_response(w, WriteResponse{.written = 7, .generation = 42});
  write_mutation_ack(w, MutationAck{43});
  Reader r(buf);
  auto wr = read_write_response(r);
  ASSERT_TRUE(wr.has_value());
  EXPECT_EQ(wr->written, 7u);
  EXPECT_EQ(wr->generation, 42u);
  auto a = read_mutation_ack(r);
  ASSERT_TRUE(a.has_value());
  EXPECT_EQ(a->generation, 43u);
  EXPECT_TRUE(r.empty());
  EXPECT_EQ(read_mutation_ack(r).error(), Errc::Truncated);
}

TEST(Messages, ReadlinkRoundTrip) {
  std::vector<std::byte> buf;
  Writer w(buf);
  write_path_request(w, PathRequest{.path = "src/link"});
  write_readlink_response(w, ReadlinkResponse{.target = "../lib/target.so"});
  write_readlink_response(w, ReadlinkResponse{.target = ""});  // a link to nothing is still a link
  Reader r(buf);
  auto q = read_path_request(r);
  ASSERT_TRUE(q.has_value());
  EXPECT_EQ(q->path, "src/link");
  auto p = read_readlink_response(r);
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->target, "../lib/target.so");
  auto empty = read_readlink_response(r);
  ASSERT_TRUE(empty.has_value());
  EXPECT_EQ(empty->target, "");
  EXPECT_TRUE(r.empty());
  EXPECT_EQ(read_readlink_response(r).error(), Errc::Truncated);
}

TEST(Frame, WriteFrameFillsHeader) {
  std::vector<std::byte> buf;
  write_frame(buf, MsgType::Hello, 42, [](Writer& w) { write_hello(w, Hello{.agent = "t", .nonce = {}, .proof = {}}); });
  write_frame(buf, MsgType::Ping, 43, [](Writer&) {});
  auto h1 = decode_header(buf);
  ASSERT_TRUE(h1.has_value());
  EXPECT_EQ(h1->type, MsgType::Hello);
  EXPECT_EQ(h1->request_id, 42u);
  // version + capabilities + agent (len + "t") + nonce (len, empty) + proof (len, empty)
  EXPECT_EQ(h1->payload_len, 4u + 8u + 1u + 1u + 1u + 1u);
  Reader body{std::span<const std::byte>(buf).subspan(kHeaderSize, h1->payload_len)};
  EXPECT_EQ(read_hello(body)->agent, "t");
  auto h2 = decode_header(std::span(buf).subspan(kHeaderSize + h1->payload_len));
  ASSERT_TRUE(h2.has_value());
  EXPECT_EQ(h2->type, MsgType::Ping);
  EXPECT_EQ(h2->payload_len, 0u);
  EXPECT_EQ(buf.size(), 2 * kHeaderSize + h1->payload_len);
}

}  // namespace
}  // namespace wsld::proto
