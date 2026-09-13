#include "core/protocol.hpp"

#include <algorithm>
#include <cstring>

namespace wsld::proto {

namespace {

// An element count read off the wire is attacker-controlled. It is already
// bounded by the bytes remaining in the frame, but a 64 MiB frame can still
// claim ~64M elements, and reserving that many multi-byte structs would commit
// gigabytes before a single element is validated. Reserve only a modest amount
// up front and let the container grow as elements actually decode — a truncated
// or lying count then fails on the first short read instead of on allocation.
constexpr std::uint64_t kMaxPreReserve = 4096;

template <class V>
void reserve_bounded(V& v, std::uint64_t claimed) {
  v.reserve(static_cast<std::size_t>(std::min(claimed, kMaxPreReserve)));
}

inline void put_le(std::byte* p, std::uint64_t v, std::size_t n) noexcept {
  for (std::size_t i = 0; i < n; ++i) p[i] = static_cast<std::byte>((v >> (8 * i)) & 0xFF);
}

inline std::uint64_t get_le(const std::byte* p, std::size_t n) noexcept {
  std::uint64_t v = 0;
  for (std::size_t i = 0; i < n; ++i) v |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(p[i])) << (8 * i);
  return v;
}

}  // namespace

// --- frame header --------------------------------------------------------------

void encode_header(const FrameHeader& h, std::span<std::byte, kHeaderSize> out) noexcept {
  put_le(out.data() + 0, kMagic, 4);
  put_le(out.data() + 4, kVersion, 2);
  put_le(out.data() + 6, static_cast<std::uint16_t>(h.type), 2);
  put_le(out.data() + 8, h.flags, 4);
  put_le(out.data() + 12, h.payload_len, 4);
  put_le(out.data() + 16, h.request_id, 8);
}

Result<FrameHeader> decode_header(std::span<const std::byte> in) noexcept {
  if (in.size() < kHeaderSize) return fail(Errc::Truncated);
  if (get_le(in.data(), 4) != kMagic) return fail(Errc::BadMagic);
  if (get_le(in.data() + 4, 2) != kVersion) return fail(Errc::UnsupportedVersion);
  FrameHeader h;
  h.type = static_cast<MsgType>(get_le(in.data() + 6, 2));
  h.flags = static_cast<std::uint32_t>(get_le(in.data() + 8, 4));
  h.payload_len = static_cast<std::uint32_t>(get_le(in.data() + 12, 4));
  h.request_id = get_le(in.data() + 16, 8);
  if (h.payload_len > kMaxPayload) return fail(Errc::TooLarge);
  return h;
}

// --- Writer ------------------------------------------------------------------------

void Writer::u16(std::uint16_t v) {
  const std::size_t p = out_.size();
  out_.resize(p + 2);
  put_le(out_.data() + p, v, 2);
}

void Writer::u32(std::uint32_t v) {
  const std::size_t p = out_.size();
  out_.resize(p + 4);
  put_le(out_.data() + p, v, 4);
}

void Writer::u64(std::uint64_t v) {
  const std::size_t p = out_.size();
  out_.resize(p + 8);
  put_le(out_.data() + p, v, 8);
}

void Writer::varint(std::uint64_t v) {
  while (v >= 0x80) {
    out_.push_back(static_cast<std::byte>((v & 0x7F) | 0x80));
    v >>= 7;
  }
  out_.push_back(static_cast<std::byte>(v));
}

void Writer::raw(std::span<const std::byte> b) { out_.insert(out_.end(), b.begin(), b.end()); }

void Writer::string(std::string_view s) {
  varint(s.size());
  const std::size_t p = out_.size();
  out_.resize(p + s.size());
  if (!s.empty()) std::memcpy(out_.data() + p, s.data(), s.size());
}

void Writer::blob(std::span<const std::byte> b) {
  varint(b.size());
  raw(b);
}

void Writer::patch_u32(std::size_t pos, std::uint32_t v) noexcept { put_le(out_.data() + pos, v, 4); }

// --- Reader ------------------------------------------------------------------------

Result<std::uint8_t> Reader::u8() noexcept {
  if (remaining() < 1) return fail(Errc::Truncated);
  return std::to_integer<std::uint8_t>(in_[pos_++]);
}

Result<std::uint16_t> Reader::u16() noexcept {
  if (remaining() < 2) return fail(Errc::Truncated);
  const auto v = static_cast<std::uint16_t>(get_le(in_.data() + pos_, 2));
  pos_ += 2;
  return v;
}

Result<std::uint32_t> Reader::u32() noexcept {
  if (remaining() < 4) return fail(Errc::Truncated);
  const auto v = static_cast<std::uint32_t>(get_le(in_.data() + pos_, 4));
  pos_ += 4;
  return v;
}

Result<std::uint64_t> Reader::u64() noexcept {
  if (remaining() < 8) return fail(Errc::Truncated);
  const std::uint64_t v = get_le(in_.data() + pos_, 8);
  pos_ += 8;
  return v;
}

Result<std::uint64_t> Reader::varint() noexcept {
  std::uint64_t v = 0;
  for (unsigned shift = 0; shift < 64; shift += 7) {
    if (pos_ >= in_.size()) return fail(Errc::Truncated);
    const auto b = std::to_integer<std::uint8_t>(in_[pos_++]);
    v |= static_cast<std::uint64_t>(b & 0x7F) << shift;
    if ((b & 0x80) == 0) {
      // The 10th byte may carry at most one payload bit.
      if (shift == 63 && b > 1) return fail(Errc::Corrupt);
      return v;
    }
  }
  return fail(Errc::Corrupt);
}

Result<std::span<const std::byte>> Reader::raw(std::size_t n) noexcept {
  if (remaining() < n) return fail(Errc::Truncated);
  auto s = in_.subspan(pos_, n);
  pos_ += n;
  return s;
}

Result<std::string_view> Reader::string() noexcept {
  auto len = varint();
  if (!len) return fail(len.error());
  if (*len > remaining()) return fail(Errc::Truncated);
  auto s = std::string_view(reinterpret_cast<const char*>(in_.data() + pos_), static_cast<std::size_t>(*len));
  pos_ += static_cast<std::size_t>(*len);
  return s;
}

Result<std::span<const std::byte>> Reader::blob() noexcept {
  auto len = varint();
  if (!len) return fail(len.error());
  if (*len > remaining()) return fail(Errc::Truncated);
  return raw(static_cast<std::size_t>(*len));
}

// --- messages ------------------------------------------------------------------------

void write_attributes(Writer& w, const Attributes& a) {
  w.u8(static_cast<std::uint8_t>(a.kind));
  w.varint(a.mode);
  w.varint(a.size);
  w.i64(a.mtime_ns);
}

Result<Attributes> read_attributes(Reader& r) noexcept {
  Attributes a;
  auto kind = r.u8();
  if (!kind) return fail(kind.error());
  if (*kind > static_cast<std::uint8_t>(NodeKind::Other)) return fail(Errc::Corrupt);
  a.kind = static_cast<NodeKind>(*kind);
  auto mode = r.varint();
  if (!mode) return fail(mode.error());
  if (*mode > 0xFFFFFFFFULL) return fail(Errc::Corrupt);
  a.mode = static_cast<std::uint32_t>(*mode);
  auto size = r.varint();
  if (!size) return fail(size.error());
  a.size = *size;
  auto mtime = r.i64();
  if (!mtime) return fail(mtime.error());
  a.mtime_ns = *mtime;
  return a;
}

void write_hello(Writer& w, const Hello& h) {
  w.u32(h.protocol_version);
  w.u64(h.capabilities);
  w.string(h.agent);
  w.string(h.nonce);
  w.string(h.proof);
}

Result<Hello> read_hello(Reader& r) noexcept {
  Hello h;
  auto v = r.u32();
  if (!v) return fail(v.error());
  h.protocol_version = *v;
  auto caps = r.u64();
  if (!caps) return fail(caps.error());
  h.capabilities = *caps;
  auto agent = r.string();
  if (!agent) return fail(agent.error());
  h.agent = *agent;
  auto nonce = r.string();
  if (!nonce) return fail(nonce.error());
  h.nonce = *nonce;
  auto proof = r.string();
  if (!proof) return fail(proof.error());
  h.proof = *proof;
  return h;
}

void write_snapshot_header(Writer& w, std::uint64_t generation, std::uint32_t count) {
  w.u64(generation);
  w.u32(count);
}

void write_snapshot_entry(Writer& w, const SnapshotEntry& e) {
  w.u32(e.parent);
  write_attributes(w, e.attr);
  w.string(e.name);
}

Result<SnapshotHeader> read_snapshot_header(Reader& r) noexcept {
  auto gen = r.u64();
  if (!gen) return fail(gen.error());
  auto count = r.u32();
  if (!count) return fail(count.error());
  // Each entry is at least 4 (parent) + 1 (kind) + 1 + 1 + 8 + 1 (empty name) bytes.
  if (static_cast<std::uint64_t>(*count) * 16 > r.remaining()) return fail(Errc::Corrupt);
  return SnapshotHeader{*gen, *count};
}

Result<SnapshotEntry> read_snapshot_entry(Reader& r) noexcept {
  SnapshotEntry e;
  auto parent = r.u32();
  if (!parent) return fail(parent.error());
  e.parent = *parent;
  auto attr = read_attributes(r);
  if (!attr) return fail(attr.error());
  e.attr = *attr;
  auto name = r.string();
  if (!name) return fail(name.error());
  e.name = *name;
  return e;
}

void write_invalidation(Writer& w, const InvalidationBatch& b) {
  w.u64(b.generation);
  w.varint(b.ops.size());
  for (const InvalidationOp& op : b.ops) {
    w.u8(static_cast<std::uint8_t>(op.kind));
    w.string(op.path);
    if (op.kind == InvalidationKind::Upsert) write_attributes(w, op.attr);
    // One extra byte per op in the common case; the cookie rides along only for
    // the two move kinds, which are a small minority of any real batch.
    w.u8(static_cast<std::uint8_t>(op.change));
    if (is_move(op.change)) w.u32(op.cookie);
  }
}

Result<InvalidationBatch> read_invalidation(Reader& r) noexcept {
  InvalidationBatch b;
  auto gen = r.u64();
  if (!gen) return fail(gen.error());
  b.generation = *gen;
  auto count = r.varint();
  if (!count) return fail(count.error());
  if (*count > r.remaining()) return fail(Errc::Corrupt);  // every op is >= 2 bytes
  reserve_bounded(b.ops, *count);
  for (std::uint64_t i = 0; i < *count; ++i) {
    InvalidationOp op;
    auto kind = r.u8();
    if (!kind) return fail(kind.error());
    if (*kind > static_cast<std::uint8_t>(InvalidationKind::Rescan)) return fail(Errc::Corrupt);
    op.kind = static_cast<InvalidationKind>(*kind);
    auto path = r.string();
    if (!path) return fail(path.error());
    op.path.assign(*path);
    if (op.kind == InvalidationKind::Upsert) {
      auto attr = read_attributes(r);
      if (!attr) return fail(attr.error());
      op.attr = *attr;
    }
    auto change = r.u8();
    if (!change) return fail(change.error());
    if (*change > kMaxChangeKind) return fail(Errc::Corrupt);
    op.change = static_cast<ChangeKind>(*change);
    if (is_move(op.change)) {
      auto cookie = r.u32();
      if (!cookie) return fail(cookie.error());
      op.cookie = *cookie;
    }
    b.ops.push_back(std::move(op));
  }
  return b;
}

void write_read_request(Writer& w, const ReadRequest& q) {
  w.u64(q.offset);
  w.u32(q.length);
  w.string(q.path);
}

Result<ReadRequest> read_read_request(Reader& r) noexcept {
  ReadRequest q;
  auto off = r.u64();
  if (!off) return fail(off.error());
  q.offset = *off;
  auto len = r.u32();
  if (!len) return fail(len.error());
  q.length = *len;
  auto path = r.string();
  if (!path) return fail(path.error());
  q.path = *path;
  return q;
}

void write_read_response(Writer& w, const ReadResponse& p) {
  w.u64(p.file_size);
  w.blob(p.data);
}

Result<ReadResponse> read_read_response(Reader& r) noexcept {
  ReadResponse p;
  auto size = r.u64();
  if (!size) return fail(size.error());
  p.file_size = *size;
  auto data = r.blob();
  if (!data) return fail(data.error());
  p.data = *data;
  return p;
}

void write_error(Writer& w, const ErrorMessage& e) {
  w.u32(e.code);
  w.string(e.detail);
}

Result<ErrorMessage> read_error(Reader& r) noexcept {
  ErrorMessage e;
  auto code = r.u32();
  if (!code) return fail(code.error());
  e.code = *code;
  auto detail = r.string();
  if (!detail) return fail(detail.error());
  e.detail = *detail;
  return e;
}

void write_write_request(Writer& w, const WriteRequest& q) {
  w.u64(q.offset);
  w.string(q.path);
  w.blob(q.data);
}

Result<WriteRequest> read_write_request(Reader& r) noexcept {
  WriteRequest q;
  auto off = r.u64();
  if (!off) return fail(off.error());
  q.offset = *off;
  auto path = r.string();
  if (!path) return fail(path.error());
  q.path = *path;
  auto data = r.blob();
  if (!data) return fail(data.error());
  q.data = *data;
  return q;
}

void write_write_response(Writer& w, const WriteResponse& p) {
  w.u64(p.written);
  w.u64(p.generation);
}

Result<WriteResponse> read_write_response(Reader& r) noexcept {
  auto n = r.u64();
  if (!n) return fail(n.error());
  auto g = r.u64();
  if (!g) return fail(g.error());
  return WriteResponse{*n, *g};
}

void write_mutation_ack(Writer& w, const MutationAck& a) { w.u64(a.generation); }

Result<MutationAck> read_mutation_ack(Reader& r) noexcept {
  auto g = r.u64();
  if (!g) return fail(g.error());
  return MutationAck{*g};
}

void write_create_request(Writer& w, const CreateRequest& q) {
  w.varint(q.mode);
  w.string(q.path);
}

Result<CreateRequest> read_create_request(Reader& r) noexcept {
  CreateRequest q;
  auto mode = r.varint();
  if (!mode) return fail(mode.error());
  if (*mode > 0xFFFFFFFFULL) return fail(Errc::Corrupt);
  q.mode = static_cast<std::uint32_t>(*mode);
  auto path = r.string();
  if (!path) return fail(path.error());
  q.path = *path;
  return q;
}

void write_mkdir_request(Writer& w, const MkdirRequest& q) {
  w.varint(q.mode);
  w.string(q.path);
}

Result<MkdirRequest> read_mkdir_request(Reader& r) noexcept {
  MkdirRequest q;
  auto mode = r.varint();
  if (!mode) return fail(mode.error());
  if (*mode > 0xFFFFFFFFULL) return fail(Errc::Corrupt);
  q.mode = static_cast<std::uint32_t>(*mode);
  auto path = r.string();
  if (!path) return fail(path.error());
  q.path = *path;
  return q;
}

void write_path_request(Writer& w, const PathRequest& q) { w.string(q.path); }

Result<PathRequest> read_path_request(Reader& r) noexcept {
  auto path = r.string();
  if (!path) return fail(path.error());
  return PathRequest{*path};
}

void write_truncate_request(Writer& w, const TruncateRequest& q) {
  w.u64(q.size);
  w.string(q.path);
}

Result<TruncateRequest> read_truncate_request(Reader& r) noexcept {
  TruncateRequest q;
  auto size = r.u64();
  if (!size) return fail(size.error());
  q.size = *size;
  auto path = r.string();
  if (!path) return fail(path.error());
  q.path = *path;
  return q;
}

void write_rename_request(Writer& w, const RenameRequest& q) {
  w.string(q.from);
  w.string(q.to);
}

Result<RenameRequest> read_rename_request(Reader& r) noexcept {
  RenameRequest q;
  auto from = r.string();
  if (!from) return fail(from.error());
  q.from = *from;
  auto to = r.string();
  if (!to) return fail(to.error());
  q.to = *to;
  return q;
}

void write_read_many_request(Writer& w, const ReadManyRequest& q) {
  w.varint(q.paths.size());
  for (const auto& p : q.paths) w.string(p);
}

Result<ReadManyRequest> read_read_many_request(Reader& r) {
  auto count = r.varint();
  if (!count) return fail(count.error());
  if (*count > r.remaining()) return fail(Errc::Corrupt);  // each path is >= 1 byte
  if (*count > kMaxReadManyPaths) return fail(Errc::InvalidArgument);
  ReadManyRequest q;
  reserve_bounded(q.paths, *count);
  for (std::uint64_t i = 0; i < *count; ++i) {
    auto p = r.string();
    if (!p) return fail(p.error());
    q.paths.push_back(*p);
  }
  return q;
}

void write_read_many_response(Writer& w, const ReadManyResponse& p) {
  w.varint(p.items.size());
  for (const auto& it : p.items) {
    w.u8(it.ok ? 1 : 0);
    if (it.ok) w.blob(it.data);
  }
}

Result<ReadManyResponse> read_read_many_response(Reader& r) {
  auto count = r.varint();
  if (!count) return fail(count.error());
  if (*count > r.remaining()) return fail(Errc::Corrupt);
  ReadManyResponse p;
  reserve_bounded(p.items, *count);
  for (std::uint64_t i = 0; i < *count; ++i) {
    auto ok = r.u8();
    if (!ok) return fail(ok.error());
    ReadManyItem item;
    item.ok = *ok != 0;
    if (item.ok) {
      auto data = r.blob();
      if (!data) return fail(data.error());
      item.data = *data;
    }
    p.items.push_back(item);
  }
  return p;
}

void write_readlink_response(Writer& w, const ReadlinkResponse& p) { w.string(p.target); }

Result<ReadlinkResponse> read_readlink_response(Reader& r) noexcept {
  auto t = r.string();
  if (!t) return fail(t.error());
  return ReadlinkResponse{*t};
}

}  // namespace wsld::proto
