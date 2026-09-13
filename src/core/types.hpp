#pragma once

#include <cstdint>
#include <limits>

namespace wsld {

/// Index of a node inside a MetadataTree. Stable for the lifetime of the node.
using NodeId = std::uint32_t;
/// Index of an interned name inside a StringPool.
using NameId = std::uint32_t;

inline constexpr NodeId kInvalidNode = std::numeric_limits<NodeId>::max();
inline constexpr NameId kInvalidName = std::numeric_limits<NameId>::max();

enum class NodeKind : std::uint8_t {
  File = 0,
  Directory = 1,
  Symlink = 2,
  Other = 3,  // sockets, fifos, devices: hidden from the remote view
};

/// Attributes as reported by the source filesystem. Kept deliberately small:
/// this struct is stored once per node in the in-memory metadata tree.
struct Attributes {
  std::uint64_t size = 0;
  std::int64_t mtime_ns = 0;  // nanoseconds since the Unix epoch
  std::uint32_t mode = 0;     // POSIX permission bits (lower 12 bits meaningful)
  NodeKind kind = NodeKind::File;

  friend bool operator==(const Attributes&, const Attributes&) = default;
};

/// What the client should do to its metadata tree for one path.
enum class InvalidationKind : std::uint8_t {
  Upsert = 0,  // create or refresh the node with the attached attributes
  Remove = 1,  // drop the node and everything below it
  Rescan = 2,  // the watcher lost events; refetch a snapshot of this subtree
};

/// What actually happened to a path, as opposed to what a mirror must do about
/// it. InvalidationKind is all a metadata mirror needs: refresh the node, or
/// drop it. A change-notification consumer needs more — it has to tell a file
/// that appeared from one that was rewritten, and it has to see the two halves
/// of a rename as one move rather than as an unrelated removal and creation.
///
/// The two travel together in every invalidation op. Nothing in the mirror
/// depends on this field, so a consumer that ignores it stays correct.
enum class ChangeKind : std::uint8_t {
  Unknown = 0,    // no watcher event behind this op (a subtree expansion, a rescan)
  Created = 1,
  Modified = 2,
  Removed = 3,
  MovedFrom = 4,  // source half of a rename, paired with its MovedTo by cookie
  MovedTo = 5,    // destination half
};

/// Highest valid ChangeKind value, for validating one off the wire. Derived, so
/// adding a kind cannot leave it behind.
inline constexpr std::uint8_t kMaxChangeKind = static_cast<std::uint8_t>(ChangeKind::MovedTo);

/// True for the two halves of a rename, the only ops that carry a cookie.
[[nodiscard]] constexpr bool is_move(ChangeKind c) noexcept {
  return c == ChangeKind::MovedFrom || c == ChangeKind::MovedTo;
}

}  // namespace wsld
