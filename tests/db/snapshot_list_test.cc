// The list of the snapshots that are open, which the database keeps so that a compaction knows
// which versions of a key it must not drop.

#include "db/snapshot_list.h"

#include <gtest/gtest.h>

#include <vector>

#include "util/dbformat.h"

namespace shale {
namespace {

TEST(SnapshotListTest, StartsEmpty) {
  SnapshotList list;
  EXPECT_EQ(list.size(), 0u);
  EXPECT_TRUE(list.Sequences().empty());
}

TEST(SnapshotListTest, NewRecordsTheSequenceAndTheOwner) {
  SnapshotList list;
  int owner = 0;
  const SnapshotImpl* s = list.New(42, &owner);
  EXPECT_EQ(s->sequence(), SequenceNumber{42});
  EXPECT_EQ(s->owner(), &owner);
  EXPECT_EQ(list.size(), 1u);
  list.Delete(s);
}

TEST(SnapshotListTest, SequencesAreAscendingAndDeleteRemovesOneSnapshot) {
  SnapshotList list;
  int owner = 0;
  const SnapshotImpl* a = list.New(3, &owner);
  const SnapshotImpl* b = list.New(5, &owner);
  const SnapshotImpl* c = list.New(5, &owner);  // two snapshots may be taken at one sequence
  const SnapshotImpl* d = list.New(9, &owner);
  EXPECT_EQ(list.Sequences(), (std::vector<SequenceNumber>{3, 5, 5, 9}));

  list.Delete(b);
  EXPECT_EQ(list.Sequences(), (std::vector<SequenceNumber>{3, 5, 9}));
  list.Delete(a);
  EXPECT_EQ(list.Sequences(), (std::vector<SequenceNumber>{5, 9}));
  list.Delete(d);
  list.Delete(c);
  EXPECT_EQ(list.size(), 0u);
  EXPECT_TRUE(list.Sequences().empty());
}

TEST(SnapshotListTest, OtherSnapshotsKeepTheirAddressesAcrossDeletes) {
  SnapshotList list;
  int owner = 0;
  const SnapshotImpl* a = list.New(1, &owner);
  const SnapshotImpl* b = list.New(2, &owner);
  list.Delete(a);
  const SnapshotImpl* c = list.New(3, &owner);
  EXPECT_EQ(b->sequence(), SequenceNumber{2});  // a pointer the caller holds stays valid
  EXPECT_EQ(c->sequence(), SequenceNumber{3});
  list.Delete(b);
  list.Delete(c);
}

TEST(SnapshotListDeathTest, SnapshotsAreTakenInAscendingOrder) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  SnapshotList list;
  int owner = 0;
  list.New(5, &owner);
  EXPECT_DEATH(list.New(4, &owner), "ascending");
}

TEST(SnapshotListDeathTest, DeletingTwiceAborts) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  SnapshotList list;
  int owner = 0;
  const SnapshotImpl* s = list.New(1, &owner);
  list.Delete(s);
  EXPECT_DEATH(list.Delete(s), "not a snapshot of this list");
}

TEST(SnapshotListDeathTest, DeletingAStrangerAborts) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  SnapshotList list;
  SnapshotList other;
  int owner = 0;
  const SnapshotImpl* s = other.New(1, &owner);
  EXPECT_DEATH(list.Delete(s), "not a snapshot of this list");
  other.Delete(s);
}

}  // namespace
}  // namespace shale
