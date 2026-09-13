#include "core/coalescer.hpp"

#include <gtest/gtest.h>

#include <chrono>

namespace wsld {
namespace {

using namespace std::chrono_literals;
using tp = Coalescer::clock::time_point;

const tp t0 = tp{} + 1s;

std::vector<PlannedOp> ops(std::initializer_list<std::pair<InvalidationKind, const char*>> list) {
  std::vector<PlannedOp> out;
  for (const auto& [k, p] : list) out.push_back(PlannedOp{k, p});
  return out;
}

TEST(Coalescer, IdleIsNotReady) {
  Coalescer c;
  EXPECT_TRUE(c.empty());
  EXPECT_FALSE(c.ready(t0));
  EXPECT_FALSE(c.deadline().has_value());
  EXPECT_TRUE(c.take().empty());
}

TEST(Coalescer, QuietPeriodAndMaxLatency) {
  Coalescer c(Coalescer::Options{.max_pending = 100, .quiet_period = 2ms, .max_latency = 25ms});
  c.push({FsEventKind::Modified, "a"}, t0);
  EXPECT_FALSE(c.ready(t0 + 1ms));
  EXPECT_EQ(*c.deadline(), t0 + 2ms);
  EXPECT_TRUE(c.ready(t0 + 2ms));

  // A steady trickle keeps resetting the quiet timer until max_latency hits.
  Coalescer d(Coalescer::Options{.max_pending = 100, .quiet_period = 2ms, .max_latency = 25ms});
  tp now = t0;
  for (int i = 0; i < 24; ++i) {
    d.push({FsEventKind::Modified, "a"}, now);
    EXPECT_FALSE(d.ready(now + 1ms)) << i;
    now += 1ms;
  }
  EXPECT_EQ(*d.deadline(), t0 + 25ms);
  EXPECT_TRUE(d.ready(t0 + 25ms));
}

TEST(Coalescer, MaxPendingForcesReady) {
  Coalescer c(Coalescer::Options{.max_pending = 3, .quiet_period = 1h, .max_latency = 1h});
  c.push({FsEventKind::Created, "a"}, t0);
  c.push({FsEventKind::Created, "b"}, t0);
  EXPECT_FALSE(c.ready(t0));
  c.push({FsEventKind::Created, "c"}, t0);
  EXPECT_TRUE(c.ready(t0));
  EXPECT_EQ(*c.deadline(), t0);
}

TEST(Coalescer, RepeatedEventsCollapse) {
  Coalescer c;
  c.push({FsEventKind::Created, "f"}, t0);
  c.push({FsEventKind::Modified, "f"}, t0);
  c.push({FsEventKind::Modified, "f"}, t0);
  c.push({FsEventKind::Modified, "g"}, t0);
  EXPECT_EQ(c.pending(), 2u);
  EXPECT_EQ(c.take(), ops({{InvalidationKind::Upsert, "f"}, {InvalidationKind::Upsert, "g"}}));
  EXPECT_TRUE(c.empty());
}

TEST(Coalescer, LastEventWinsAndOrderFollowsLastEvent) {
  Coalescer c;
  c.push({FsEventKind::Modified, "a"}, t0);
  c.push({FsEventKind::Modified, "b"}, t0);
  c.push({FsEventKind::Removed, "a"}, t0);  // a's final event is now after b's
  EXPECT_EQ(c.take(), ops({{InvalidationKind::Upsert, "b"}, {InvalidationKind::Remove, "a"}}));

  c.push({FsEventKind::Removed, "x"}, t0);
  c.push({FsEventKind::Created, "x"}, t0);  // deleted then re-created -> refresh
  EXPECT_EQ(c.take(), ops({{InvalidationKind::Upsert, "x"}}));
}

TEST(Coalescer, RenameBecomesRemovePlusUpsert) {
  Coalescer c;
  c.push({FsEventKind::RenamedFrom, "old.txt"}, t0);
  c.push({FsEventKind::RenamedTo, "new.txt"}, t0);
  EXPECT_EQ(c.take(), ops({{InvalidationKind::Remove, "old.txt"}, {InvalidationKind::Upsert, "new.txt"}}));
}

TEST(Coalescer, RemovedDirectoryDropsOlderDescendants) {
  Coalescer c;
  c.push({FsEventKind::Modified, "dir/a"}, t0);
  c.push({FsEventKind::Modified, "dir/sub/b"}, t0);
  c.push({FsEventKind::Modified, "directory/unrelated"}, t0);  // shares a prefix but is not under "dir"
  c.push({FsEventKind::Removed, "dir/sub"}, t0);
  c.push({FsEventKind::Removed, "dir"}, t0);
  c.push({FsEventKind::Created, "dir/c"}, t0);  // re-created after the removal: must survive
  EXPECT_EQ(c.take(), ops({{InvalidationKind::Upsert, "directory/unrelated"},
                           {InvalidationKind::Remove, "dir"},
                           {InvalidationKind::Upsert, "dir/c"}}));
}

TEST(Coalescer, OverflowCollapsesToRescan) {
  Coalescer c(Coalescer::Options{.max_pending = 100, .quiet_period = 1h, .max_latency = 1h});
  c.push({FsEventKind::Modified, "a"}, t0);
  c.push({FsEventKind::Overflow, ""}, t0);
  c.push({FsEventKind::Modified, "b"}, t0);  // ignored; the rescan covers it
  EXPECT_TRUE(c.overflowed());
  EXPECT_TRUE(c.ready(t0));
  EXPECT_EQ(c.pending(), 1u);
  EXPECT_EQ(c.take(), ops({{InvalidationKind::Rescan, ""}}));
  EXPECT_FALSE(c.overflowed());
  EXPECT_TRUE(c.empty());
}

TEST(Coalescer, MarksEntriesThatAppeared) {
  // A renamed directory brings a subtree the watcher never reports, so the
  // sender needs to know which upserts are NEW entries (worth enumerating) and
  // which are mere modifications of something the peers already have.
  Coalescer c;
  c.push({FsEventKind::Created, "new"}, t0);
  c.push({FsEventKind::RenamedTo, "moved"}, t0);
  c.push({FsEventKind::Modified, "touched"}, t0);
  c.push({FsEventKind::Created, "new-then-touched"}, t0);
  c.push({FsEventKind::Modified, "new-then-touched"}, t0);  // still new: nothing removed it
  c.push({FsEventKind::Created, "gone"}, t0);
  c.push({FsEventKind::Removed, "gone"}, t0);  // a removal resets it
  c.push({FsEventKind::Removed, "gone"}, t0);
  c.push({FsEventKind::Created, "gone"}, t0);  // re-created after removal: new again
  auto out = c.take();
  auto find = [&](std::string_view p) -> const PlannedOp& {
    for (const auto& op : out)
      if (op.path == p) return op;
    static const PlannedOp none{InvalidationKind::Rescan, "?"};
    return none;
  };
  EXPECT_TRUE(find("new").appeared);
  EXPECT_TRUE(find("moved").appeared);
  EXPECT_FALSE(find("touched").appeared);
  EXPECT_TRUE(find("new-then-touched").appeared);
  EXPECT_EQ(find("gone").kind, InvalidationKind::Upsert);
  EXPECT_TRUE(find("gone").appeared);
}

// --- change kinds and rename pairing (the inotify bridge's input) ------------

// Every consumer downstream keys off `change`, and the whole point of keeping
// it separate from `kind` is that a burst collapses to one op without losing
// what the burst was.
TEST(Coalescer, ChangeKindSurvivesCollapsing) {
  Coalescer c;
  c.push({FsEventKind::Created, "new.txt"}, t0);
  c.push({FsEventKind::Modified, "new.txt"}, t0);  // still a creation, not a modification
  c.push({FsEventKind::Modified, "old.txt"}, t0);
  c.push({FsEventKind::Created, "gone.txt"}, t0);
  c.push({FsEventKind::Removed, "gone.txt"}, t0);  // last event wins

  auto out = c.take();
  auto change_of = [&](std::string_view p) {
    for (const auto& op : out)
      if (op.path == p) return op.change;
    return ChangeKind::Unknown;
  };
  EXPECT_EQ(change_of("new.txt"), ChangeKind::Created);
  EXPECT_EQ(change_of("old.txt"), ChangeKind::Modified);
  EXPECT_EQ(change_of("gone.txt"), ChangeKind::Removed);
}

TEST(Coalescer, RenamePairSharesACookie) {
  Coalescer c;
  c.push({FsEventKind::RenamedFrom, "old.txt", 77}, t0);
  c.push({FsEventKind::RenamedTo, "new.txt", 77}, t0);

  auto out = c.take();
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(out[0].change, ChangeKind::MovedFrom);
  EXPECT_EQ(out[0].path, "old.txt");
  EXPECT_EQ(out[1].change, ChangeKind::MovedTo);
  EXPECT_EQ(out[1].path, "new.txt");
  EXPECT_EQ(out[0].cookie, 77u);
  EXPECT_EQ(out[1].cookie, 77u);
}

// A write to the destination after the move must not cost the pairing: the
// consumer still has to see one move rather than a delete and an unrelated
// write. This is the git lock-file dance (rename into place, then touch).
TEST(Coalescer, WriteAfterMoveKeepsThePair) {
  Coalescer c;
  c.push({FsEventKind::RenamedFrom, "a", 5}, t0);
  c.push({FsEventKind::RenamedTo, "b", 5}, t0);
  c.push({FsEventKind::Modified, "b"}, t0);

  auto out = c.take();
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(out[0].change, ChangeKind::MovedFrom);
  EXPECT_EQ(out[1].change, ChangeKind::MovedTo);
  EXPECT_EQ(out[1].cookie, 5u);
}

// The source coming back under its old name is exactly what git does, and it
// leaves the destination's half with nobody to pair with. A half-move must not
// reach a consumer: on its own the destination simply appeared.
TEST(Coalescer, StrandedMoveHalfDegrades) {
  Coalescer c;
  c.push({FsEventKind::RenamedFrom, "config", 9}, t0);
  c.push({FsEventKind::RenamedTo, "config.lock", 9}, t0);
  c.push({FsEventKind::Created, "config", 0}, t0);  // recreated, so it is no longer a move source

  auto out = c.take();
  ASSERT_EQ(out.size(), 2u);
  for (const auto& op : out) {
    EXPECT_FALSE(is_move(op.change)) << op.path;
    EXPECT_EQ(op.cookie, 0u) << op.path;
    EXPECT_EQ(op.change, ChangeKind::Created) << op.path;
  }
}

// The other direction: a removed directory takes the destination out of the
// batch, so the source is left as the surviving half.
TEST(Coalescer, MoveIntoARemovedDirectoryDegradesToRemoval) {
  Coalescer c;
  c.push({FsEventKind::RenamedFrom, "src.txt", 3}, t0);
  c.push({FsEventKind::RenamedTo, "dir/dst.txt", 3}, t0);
  c.push({FsEventKind::Removed, "dir"}, t0);  // drops dir/dst.txt with it

  auto out = c.take();
  ASSERT_EQ(out.size(), 2u);
  auto change_of = [&](std::string_view p) {
    for (const auto& op : out)
      if (op.path == p) return op.change;
    return ChangeKind::Unknown;
  };
  EXPECT_EQ(change_of("src.txt"), ChangeKind::Removed);
  EXPECT_EQ(change_of("dir"), ChangeKind::Removed);
}

TEST(Coalescer, OverflowCarriesNoChangeKind) {
  Coalescer c;
  c.push({FsEventKind::Created, "a"}, t0);
  c.push({FsEventKind::Overflow, {}}, t0);
  auto out = c.take();
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].kind, InvalidationKind::Rescan);
  EXPECT_EQ(out[0].change, ChangeKind::Unknown);
}

// repair_move_pairs is applied again by the sender and by the receiver, on
// their own op types, so it is exercised directly here too.
TEST(RepairMovePairs, MatchedPairSurvivesAndOrphansDegrade) {
  std::vector<PlannedOp> ops{
      PlannedOp{InvalidationKind::Remove, "a", false, ChangeKind::MovedFrom, 1},
      PlannedOp{InvalidationKind::Upsert, "b", true, ChangeKind::MovedTo, 1},
      PlannedOp{InvalidationKind::Upsert, "c", true, ChangeKind::MovedTo, 2},   // no partner
      PlannedOp{InvalidationKind::Remove, "d", false, ChangeKind::MovedFrom, 3},  // no partner
      PlannedOp{InvalidationKind::Upsert, "e", true, ChangeKind::MovedTo, 0},   // no cookie at all
  };
  repair_move_pairs(ops);
  EXPECT_EQ(ops[0].change, ChangeKind::MovedFrom);
  EXPECT_EQ(ops[1].change, ChangeKind::MovedTo);
  EXPECT_EQ(ops[2].change, ChangeKind::Created);
  EXPECT_EQ(ops[3].change, ChangeKind::Removed);
  EXPECT_EQ(ops[4].change, ChangeKind::Created);
  EXPECT_EQ(ops[2].cookie, 0u);
  EXPECT_EQ(ops[3].cookie, 0u);
}

}  // namespace
}  // namespace wsld
