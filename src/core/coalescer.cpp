#include "core/coalescer.hpp"

#include "core/path.hpp"

#include <algorithm>
#include <unordered_map>

namespace wsld {

void Coalescer::push(const FsEvent& ev, clock::time_point now) {
  if (pending_.empty() && !overflow_) first_ = now;
  last_ = now;

  if (ev.kind == FsEventKind::Overflow) {
    overflow_ = true;
    pending_.clear();
    return;
  }
  if (overflow_) return;  // the rescan will cover it

  InvalidationKind kind;
  bool appeared = false;
  ChangeKind change;
  std::uint32_t cookie = 0;
  switch (ev.kind) {
    case FsEventKind::Created:
      kind = InvalidationKind::Upsert;
      appeared = true;
      change = ChangeKind::Created;
      break;
    case FsEventKind::RenamedTo:
      kind = InvalidationKind::Upsert;
      appeared = true;
      change = ChangeKind::MovedTo;
      cookie = ev.cookie;
      break;
    case FsEventKind::Modified:
      kind = InvalidationKind::Upsert;
      change = ChangeKind::Modified;
      break;
    case FsEventKind::Removed:
      kind = InvalidationKind::Remove;
      change = ChangeKind::Removed;
      break;
    case FsEventKind::RenamedFrom:
      kind = InvalidationKind::Remove;
      change = ChangeKind::MovedFrom;
      cookie = ev.cookie;
      break;
    case FsEventKind::Overflow:
    default:
      return;
  }

  if (auto it = pending_.find(ev.path); it != pending_.end()) {
    // A Modified after a Created keeps the entry "new"; a removal resets it.
    const bool still_new = kind == InvalidationKind::Upsert && (appeared || it->second.appeared);
    // Same idea for the reported change: writes that land on a path this batch
    // has already seen appear (or move) into place do not turn it into a plain
    // modification — the consumer still has to be told the path is new, and a
    // move must keep the cookie that pairs it with its other half. Every other
    // event supersedes what came before it.
    const bool keep = change == ChangeKind::Modified &&
                      (it->second.change == ChangeKind::Created || it->second.change == ChangeKind::MovedTo);
    if (keep) {
      change = it->second.change;
      cookie = it->second.cookie;
    }
    it->second = Entry{kind, ++seq_, still_new, change, cookie};
  } else {
    pending_.emplace(std::string(ev.path), Entry{kind, ++seq_, appeared, change, cookie});
  }
}

bool Coalescer::ready(clock::time_point now) const noexcept {
  if (overflow_) return true;
  if (pending_.empty()) return false;
  if (pending_.size() >= opts_.max_pending) return true;
  return now - last_ >= opts_.quiet_period || now - first_ >= opts_.max_latency;
}

std::optional<Coalescer::clock::time_point> Coalescer::deadline() const noexcept {
  if (overflow_) return last_;
  if (pending_.empty()) return std::nullopt;
  if (pending_.size() >= opts_.max_pending) return last_;
  return std::min(last_ + opts_.quiet_period, first_ + opts_.max_latency);
}

std::vector<PlannedOp> Coalescer::take() {
  std::vector<PlannedOp> out;
  if (overflow_) {
    out.push_back(PlannedOp{InvalidationKind::Rescan, std::string{}});
    overflow_ = false;
    pending_.clear();
    return out;
  }
  if (pending_.empty()) return out;

  struct Item {
    std::string_view path;
    InvalidationKind kind;
    std::uint64_t seq;
    bool appeared;
    ChangeKind change;
    std::uint32_t cookie;
  };
  std::vector<Item> items;
  items.reserve(pending_.size());
  for (const auto& [path, e] : pending_)
    items.push_back(Item{path, e.kind, e.seq, e.appeared, e.change, e.cookie});

  // Sort by path so that a removed directory is immediately followed by its
  // descendants; drop descendants whose last event predates the removal.
  std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.path < b.path; });
  std::vector<Item> kept;
  kept.reserve(items.size());
  for (std::size_t i = 0; i < items.size(); ++i) {
    const Item& it = items[i];
    kept.push_back(it);
    if (it.kind != InvalidationKind::Remove) continue;
    std::size_t j = i + 1;
    while (j < items.size() && path_is_under(items[j].path, it.path)) {
      if (items[j].seq > it.seq) kept.push_back(items[j]);  // re-created after the removal
      ++j;
    }
    i = j - 1;
  }

  // Emit in arrival order of each path's final event.
  std::sort(kept.begin(), kept.end(), [](const Item& a, const Item& b) { return a.seq < b.seq; });

  out.reserve(kept.size());
  for (const Item& it : kept)
    out.push_back(PlannedOp{it.kind, std::string(it.path), it.appeared, it.change, it.cookie});

  // Collapsing can strand one half of a move: its partner is written again and
  // keeps the move kind while this one is re-created and loses it, or a removed
  // directory takes the partner out of the batch entirely.
  repair_move_pairs(out);

  pending_.clear();
  return out;
}

}  // namespace wsld
