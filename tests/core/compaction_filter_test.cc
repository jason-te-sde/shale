#include "core/compaction_filter.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "util/dbformat.h"
#include "util/random.h"

namespace shale {
namespace {

struct Entry {
  std::string user_key;
  SequenceNumber seq = 0;
  ValueType type = ValueType::kValue;
};

Entry V(std::string user_key, SequenceNumber seq) {
  return {std::move(user_key), seq, ValueType::kValue};
}

Entry D(std::string user_key, SequenceNumber seq) {
  return {std::move(user_key), seq, ValueType::kDeletion};
}

using Kept = std::vector<std::pair<std::string, SequenceNumber>>;
using KeptSet = std::set<std::pair<std::string, SequenceNumber>>;

// Feeds `entries`, which must already be in CompareInternalKey() order, through `filter` and
// returns the (user key, sequence) of those it keeps, in order.
Kept RunFilter(CompactionFilter& filter, const std::vector<Entry>& entries, bool is_base) {
  Kept kept;
  for (const Entry& e : entries) {
    const ParsedInternalKey key{e.user_key, e.seq, e.type};
    if (filter.Next(key, is_base) == CompactionFilter::Decision::kKeep) {
      kept.emplace_back(e.user_key, e.seq);
    }
  }
  return kept;
}

Kept RunWith(std::vector<SequenceNumber> snapshots, const std::vector<Entry>& entries,
             bool is_base) {
  CompactionFilter filter(std::move(snapshots));
  return RunFilter(filter, entries, is_base);
}

TEST(CompactionFilterTest, NoSnapshotsKeepsNewestOnly) {
  CompactionFilter filter({});
  EXPECT_EQ(RunFilter(filter, {V("a", 9), V("a", 7), D("a", 5)}, /*is_base=*/false),
            (Kept{{"a", 9}}));
  EXPECT_EQ(filter.stats().kept, 1u);
  EXPECT_EQ(filter.stats().shadowed_dropped, 2u);
  EXPECT_EQ(filter.stats().tombstones_dropped, 0u);
}

TEST(CompactionFilterTest, TombstoneDroppedAtBaseLevel) {
  CompactionFilter filter({});
  EXPECT_EQ(RunFilter(filter, {D("a", 9), V("a", 7)}, /*is_base=*/true), (Kept{}));
  EXPECT_EQ(filter.stats().kept, 0u);
  EXPECT_EQ(filter.stats().tombstones_dropped, 1u);
  EXPECT_EQ(filter.stats().shadowed_dropped, 1u);
}

TEST(CompactionFilterTest, TombstoneKeptAboveBaseLevel) {
  CompactionFilter filter({});
  EXPECT_EQ(RunFilter(filter, {D("a", 9), V("a", 7)}, /*is_base=*/false), (Kept{{"a", 9}}));
  EXPECT_EQ(filter.stats().kept, 1u);
  EXPECT_EQ(filter.stats().tombstones_dropped, 0u);
  EXPECT_EQ(filter.stats().shadowed_dropped, 1u);
}

TEST(CompactionFilterTest, LoneTombstone) {
  EXPECT_EQ(RunWith({}, {D("a", 9)}, /*is_base=*/true), (Kept{}));
  EXPECT_EQ(RunWith({}, {D("a", 9)}, /*is_base=*/false), (Kept{{"a", 9}}));
}

TEST(CompactionFilterTest, SnapshotKeepsVersionItSees) {
  EXPECT_EQ(RunWith({8}, {V("a", 9), V("a", 7), V("a", 5)}, false), (Kept{{"a", 9}, {"a", 7}}));
}

TEST(CompactionFilterTest, SnapshotAtAnEntrysSequenceSeesIt) {
  // A snapshot at 7 sees the entry written at 7: it is in the snapshot's stripe, not above it.
  CompactionFilter filter({7});
  EXPECT_EQ(RunFilter(filter, {V("a", 9), V("a", 7), V("a", 5)}, false),
            (Kept{{"a", 9}, {"a", 7}}));
  EXPECT_EQ(filter.stats().shadowed_dropped, 1u);
}

TEST(CompactionFilterTest, TwoSnapshotsThreeStripes) {
  EXPECT_EQ(RunWith({4, 8}, {V("a", 9), V("a", 7), V("a", 5), V("a", 3), V("a", 1)}, false),
            (Kept{{"a", 9}, {"a", 7}, {"a", 3}}));
}

TEST(CompactionFilterTest, SnapshotsMayComeInAnyOrderAndRepeat) {
  const std::vector<Entry> entries = {V("a", 9), V("a", 7), V("a", 5), V("a", 3), V("a", 1)};
  const Kept expected = {{"a", 9}, {"a", 7}, {"a", 3}};
  EXPECT_EQ(RunWith({8, 4}, entries, false), expected);
  EXPECT_EQ(RunWith({4, 8, 4, 8}, entries, false), expected);
  EXPECT_EQ(RunWith({8, 8, 4}, entries, false), expected);
}

TEST(CompactionFilterTest, SnapshotsNewerThanEveryEntryShareTheirStripe) {
  // Both snapshots are past everything written, so they and the latest state see the same entry.
  EXPECT_EQ(RunWith({50, 60}, {V("a", 9), V("a", 7)}, false), (Kept{{"a", 9}}));
}

TEST(CompactionFilterTest, SnapshotsOlderThanEveryEntryKeepNothingExtra) {
  // The snapshots predate every entry, so they see nothing; the latest state sees a@9.
  EXPECT_EQ(RunWith({1, 2}, {V("a", 9), V("a", 7), V("a", 5)}, false), (Kept{{"a", 9}}));
}

TEST(CompactionFilterTest, TombstoneAboveOldestStripeIsKept) {
  // The snapshot at 4 is older than the deletion, so it needs the value at 3, and the deletion
  // must stay to hide that value from everyone newer.
  CompactionFilter filter({4});
  EXPECT_EQ(RunFilter(filter, {D("a", 9), V("a", 3)}, /*is_base=*/true),
            (Kept{{"a", 9}, {"a", 3}}));
  EXPECT_EQ(filter.stats().tombstones_dropped, 0u);
}

TEST(CompactionFilterTest, TombstoneInOldestStripeIsDropped) {
  CompactionFilter filter({10});
  EXPECT_EQ(RunFilter(filter, {D("a", 9), V("a", 3)}, /*is_base=*/true), (Kept{}));
  EXPECT_EQ(filter.stats().tombstones_dropped, 1u);
  EXPECT_EQ(filter.stats().shadowed_dropped, 1u);
}

TEST(CompactionFilterTest, TombstoneAtTheOldestSnapshotIsDropped) {
  // A deletion at the snapshot's own sequence is in the snapshot's stripe, and no snapshot is
  // older.
  EXPECT_EQ(RunWith({9}, {D("a", 9), V("a", 3)}, true), (Kept{}));
  // One sequence number later it is above the snapshot, which still needs the value beneath.
  EXPECT_EQ(RunWith({8}, {D("a", 9), V("a", 3)}, true), (Kept{{"a", 9}, {"a", 3}}));
}

TEST(CompactionFilterTest, TombstoneInAMiddleStripeIsKept) {
  // The deletion at 6 hides a@3 from the snapshot at 8 and from the latest state; a@3 stays only
  // for the snapshot at 4. The deletion stays as well, because that snapshot is older than it:
  // dropped, it would show a@3 to the other two.
  CompactionFilter filter({4, 8});
  EXPECT_EQ(RunFilter(filter, {D("a", 6), V("a", 3)}, /*is_base=*/true),
            (Kept{{"a", 6}, {"a", 3}}));
  EXPECT_EQ(filter.stats().tombstones_dropped, 0u);
}

// A deletion goes only in the oldest stripe, at the base level: the one case in which the entry
// alone is enough to know that nobody needs it. Where the answer would depend on the entries after
// it, which Next() has not seen, the deletion is kept, so the rule is safe and not exact. These two
// are the cases it keeps although no observer could tell the difference.
TEST(CompactionFilterTest, TombstoneRuleIsConservative) {
  // A snapshot at 0 sees nothing of any key, and the latest state would see "deleted" either way,
  // but the deletion is above the snapshot, so it is not in the oldest stripe.
  CompactionFilter at_zero({0});
  EXPECT_EQ(RunFilter(at_zero, {D("a", 9)}, /*is_base=*/true), (Kept{{"a", 9}}));
  EXPECT_EQ(at_zero.stats().tombstones_dropped, 0u);

  // The snapshot at 2 cannot see a@3 either, so the deletion at 6 hides nothing from anyone. a@3
  // is shadowed, since the deletion before it is in its stripe, and the deletion stays.
  CompactionFilter at_two({2});
  EXPECT_EQ(RunFilter(at_two, {D("a", 6), V("a", 3)}, /*is_base=*/true), (Kept{{"a", 6}}));
  EXPECT_EQ(at_two.stats().tombstones_dropped, 0u);
  EXPECT_EQ(at_two.stats().shadowed_dropped, 1u);
}

TEST(CompactionFilterTest, TombstoneBelowBaseLevelStaysWhateverTheSnapshots) {
  EXPECT_EQ(RunWith({}, {D("a", 9)}, false), (Kept{{"a", 9}}));
  EXPECT_EQ(RunWith({10}, {D("a", 9)}, false), (Kept{{"a", 9}}));
  EXPECT_EQ(RunWith({4}, {D("a", 9)}, false), (Kept{{"a", 9}}));
}

TEST(CompactionFilterTest, TombstoneHidesTheValuesNewerThanASnapshotButNotTheSnapshots) {
  // Snapshot 4 sees a@3. Everyone else sees the deletion at 9, which hides a@7.
  CompactionFilter filter({4});
  EXPECT_EQ(RunFilter(filter, {D("a", 9), V("a", 7), V("a", 3)}, true), (Kept{{"a", 9}, {"a", 3}}));
  EXPECT_EQ(filter.stats().kept, 2u);
  EXPECT_EQ(filter.stats().shadowed_dropped, 1u);
}

TEST(CompactionFilterTest, NewUserKeyResetsState) {
  CompactionFilter filter({});
  EXPECT_EQ(RunFilter(filter, {V("a", 5), V("b", 4)}, false), (Kept{{"a", 5}, {"b", 4}}));
  EXPECT_EQ(filter.stats().kept, 2u);
  EXPECT_EQ(filter.stats().shadowed_dropped, 0u);
}

TEST(CompactionFilterTest, EachUserKeyIsJudgedOnItsOwn) {
  // b@4 is in the same stripe as a@5, the entry before it, and a new user key must not count as
  // shadowed by it. c@2 is in a different stripe from c@8, so the deletion stays, and it shadows
  // the value beneath.
  CompactionFilter filter({6});
  EXPECT_EQ(RunFilter(filter,
                      {V("a", 9), V("a", 5), V("b", 4), D("b", 3), V("c", 8), D("c", 2), V("c", 1)},
                      /*is_base=*/false),
            (Kept{{"a", 9}, {"a", 5}, {"b", 4}, {"c", 8}, {"c", 2}}));
  EXPECT_EQ(filter.stats().kept, 5u);
  EXPECT_EQ(filter.stats().shadowed_dropped, 2u);
}

TEST(CompactionFilterTest, TheSameInternalKeyTwiceIsNotAnError) {
  // The second copy is shadowed by the first, as any entry in the same stripe is.
  CompactionFilter filter({});
  EXPECT_EQ(RunFilter(filter, {V("a", 5), V("a", 5), V("b", 4)}, false),
            (Kept{{"a", 5}, {"b", 4}}));
  EXPECT_EQ(filter.stats().shadowed_dropped, 1u);

  // Also within a snapshot's stripe, and when the copies differ in type, a value first as the
  // order has it.
  CompactionFilter with_snapshot({5});
  EXPECT_EQ(RunFilter(with_snapshot, {V("a", 5), D("a", 5), V("a", 3)}, true), (Kept{{"a", 5}}));
  EXPECT_EQ(with_snapshot.stats().shadowed_dropped, 2u);
}

TEST(CompactionFilterTest, ASnapshotAtTheLargestSequenceNumberIsAccepted) {
  // Every possible entry is at or before it, so all of them are in its stripe.
  EXPECT_EQ(RunWith({kMaxSequenceNumber}, {V("a", kMaxSequenceNumber), V("a", 7)}, false),
            (Kept{{"a", kMaxSequenceNumber}}));
  EXPECT_EQ(
      RunWith({8, kMaxSequenceNumber}, {V("a", kMaxSequenceNumber), V("a", 9), V("a", 7)}, false),
      (Kept{{"a", kMaxSequenceNumber}, {"a", 7}}));
}

TEST(CompactionFilterTest, TheBaseLevelFlagIsPerKey) {
  CompactionFilter filter({});
  EXPECT_EQ(filter.Next({"a", 9, ValueType::kDeletion}, /*is_base_level_for_key=*/true),
            CompactionFilter::Decision::kDrop);
  EXPECT_EQ(filter.Next({"b", 9, ValueType::kDeletion}, /*is_base_level_for_key=*/false),
            CompactionFilter::Decision::kKeep);
  EXPECT_EQ(filter.Next({"c", 9, ValueType::kDeletion}, /*is_base_level_for_key=*/true),
            CompactionFilter::Decision::kDrop);
  EXPECT_EQ(filter.stats().tombstones_dropped, 2u);
  EXPECT_EQ(filter.stats().kept, 1u);
}

TEST(CompactionFilterTest, EmptyInput) {
  CompactionFilter filter({3});
  EXPECT_EQ(RunFilter(filter, {}, true), (Kept{}));
  EXPECT_EQ(filter.stats().kept, 0u);
  EXPECT_EQ(filter.stats().shadowed_dropped, 0u);
  EXPECT_EQ(filter.stats().tombstones_dropped, 0u);
}

TEST(CompactionFilterTest, KeyNamesThatPrefixEachOtherAreDifferentKeys) {
  EXPECT_EQ(RunWith({}, {V("a", 5), V("ab", 4), V("abc", 3)}, false),
            (Kept{{"a", 5}, {"ab", 4}, {"abc", 3}}));
  // And a user key may hold any byte, including the ones the tag is made of.
  EXPECT_EQ(RunWith({}, {V("a", 5), V(std::string("a\0", 2), 4)}, false),
            (Kept{{"a", 5}, {std::string("a\0", 2), 4}}))
      << "keys \"a\" and \"a\\0\" are different";
}

// Entries have to come in CompareInternalKey() order: dropping one by its stripe is only right if
// every entry of the key that is newer has been seen. Input out of order is a bug in the caller's
// merge, and silently dropping the wrong entries would lose data, so the filter refuses it.
TEST(CompactionFilterDeathTest, RefusesAUserKeyThatGoesBackwards) {
  CompactionFilter filter({});
  EXPECT_EQ(filter.Next({"b", 5, ValueType::kValue}, false), CompactionFilter::Decision::kKeep);
  EXPECT_DEATH((void)filter.Next({"a", 4, ValueType::kValue}, false), "order");
}

TEST(CompactionFilterDeathTest, RefusesASequenceThatGoesForwardsWithinAUserKey) {
  CompactionFilter filter({});
  EXPECT_EQ(filter.Next({"a", 5, ValueType::kValue}, false), CompactionFilter::Decision::kKeep);
  EXPECT_DEATH((void)filter.Next({"a", 6, ValueType::kValue}, false), "order");
}

TEST(CompactionFilterDeathTest, RefusesASnapshotThatNoSequenceCanReach) {
  EXPECT_DEATH(CompactionFilter filter({kMaxSequenceNumber + 1}), "snapshot");
}

// ---------------------------------------------------------------------------------------------
// The brute-force check: whatever the filter drops, no observer can tell.

// What `observer` sees for `user_key` among `entries`: the newest entry at or before it, as its
// sequence number, "none" if that is a deletion, and, when nothing is at or before it, whatever an
// older version below the entries would show (`older_below`), or "none".
std::string Visible(const std::vector<Entry>& entries, const std::string& user_key,
                    SequenceNumber observer, bool older_below) {
  const Entry* newest = nullptr;
  for (const Entry& e : entries) {
    if (e.user_key == user_key && e.seq <= observer && (newest == nullptr || e.seq > newest->seq)) {
      newest = &e;
    }
  }
  if (newest == nullptr) {
    return older_below ? "older" : "none";
  }
  return newest->type == ValueType::kDeletion ? "none" : "value@" + std::to_string(newest->seq);
}

std::string Describe(const std::vector<Entry>& entries,
                     const std::vector<SequenceNumber>& snapshots, bool is_base) {
  std::string text = "entries:";
  for (const Entry& e : entries) {
    text += " " + e.user_key + "@" + std::to_string(e.seq) +
            (e.type == ValueType::kDeletion ? "(del)" : "");
  }
  text += "\nsnapshots:";
  for (const SequenceNumber s : snapshots) {
    text += " " + std::to_string(s);
  }
  return text + "\nis_base: " + (is_base ? "true" : "false");
}

TEST(CompactionFilterTest, MatchesBruteForceVisibility) {
  const std::vector<std::string> user_keys = {"a", "b", "c", "d", "e"};
  // What the generator has to produce for the comparison to mean anything.
  uint64_t dropped_tombstones = 0;
  uint64_t kept_tombstones = 0;
  uint64_t shadowed = 0;
  // Older versions of a key that stay because a snapshot sees them.
  uint64_t kept_for_snapshots = 0;
  for (uint64_t seed = 0; seed < 500; seed++) {
    SCOPED_TRACE("seed " + std::to_string(seed));
    Random rnd(seed);

    // Unique sequence numbers from 1..200, by a partial shuffle.
    std::vector<SequenceNumber> sequences(200);
    for (size_t i = 0; i < sequences.size(); i++) {
      sequences[i] = i + 1;
    }
    std::vector<Entry> entries;
    const size_t count = 1 + rnd.Uniform(60);
    for (size_t i = 0; i < count; i++) {
      std::swap(sequences[i], sequences[i + rnd.Uniform(static_cast<uint32_t>(200 - i))]);
      entries.push_back({user_keys[rnd.Uniform(5)], sequences[i],
                         rnd.OneIn(5) ? ValueType::kDeletion : ValueType::kValue});
    }
    std::vector<SequenceNumber> snapshots;
    for (uint32_t i = rnd.Uniform(5); i > 0; i--) {
      snapshots.push_back(rnd.Uniform(201));
    }
    const bool is_base = rnd.OneIn(2);

    // The order a compaction's merged input arrives in.
    const auto internal = [](const Entry& e) {
      std::string key;
      AppendInternalKey(&key, {e.user_key, e.seq, e.type});
      return key;
    };
    std::sort(entries.begin(), entries.end(), [&](const Entry& a, const Entry& b) {
      return CompareInternalKey(internal(a), internal(b)) < 0;
    });

    SCOPED_TRACE(Describe(entries, snapshots, is_base));

    CompactionFilter filter(snapshots);
    const Kept kept_list = RunFilter(filter, entries, is_base);
    std::vector<Entry> kept_entries;
    for (const Entry& e : entries) {
      if (std::find(kept_list.begin(), kept_list.end(), std::make_pair(e.user_key, e.seq)) !=
          kept_list.end()) {
        kept_entries.push_back(e);
      }
    }
    ASSERT_EQ(kept_entries.size(), kept_list.size());
    EXPECT_EQ(filter.stats().kept, kept_list.size());
    EXPECT_EQ(
        filter.stats().kept + filter.stats().shadowed_dropped + filter.stats().tombstones_dropped,
        entries.size())
        << "every entry is kept or dropped for exactly one reason";

    // Safety: every observer, each snapshot and the latest state, sees what it saw before. When
    // the output is not at the base level, an older value may sit below and shows through
    // wherever the entries do not cover it, which is what a dropped deletion would wrongly expose.
    std::vector<SequenceNumber> observers = snapshots;
    observers.push_back(kMaxSequenceNumber);
    for (const SequenceNumber observer : observers) {
      for (const std::string& user_key : user_keys) {
        EXPECT_EQ(Visible(kept_entries, user_key, observer, !is_base),
                  Visible(entries, user_key, observer, !is_base))
            << "observer " << observer << ", key " << user_key;
      }
    }

    // Exactness of shadowing: the entries kept are those some observer sees, except for the
    // deletions the documented rule drops. That rule is conservative, not exact (see
    // TombstoneRuleIsConservative), so it is written out here as the header states it, and not
    // derived from what observers see. A filter that kept more than this would be safe and
    // wasteful, and one that kept less is caught by the comparison above.
    const auto tombstone_dropped = [&](const Entry& e) {
      // At the base level, and in the oldest stripe: no snapshot is older than the deletion.
      return is_base && e.type == ValueType::kDeletion &&
             std::none_of(snapshots.begin(), snapshots.end(),
                          [&](SequenceNumber snapshot) { return snapshot < e.seq; });
    };
    KeptSet expected;
    for (const SequenceNumber observer : observers) {
      for (const std::string& user_key : user_keys) {
        const Entry* newest = nullptr;
        for (const Entry& e : entries) {
          if (e.user_key == user_key && e.seq <= observer &&
              (newest == nullptr || e.seq > newest->seq)) {
            newest = &e;
          }
        }
        if (newest != nullptr && !tombstone_dropped(*newest)) {
          expected.emplace(newest->user_key, newest->seq);
        }
      }
    }
    EXPECT_EQ(KeptSet(kept_list.begin(), kept_list.end()), expected);

    dropped_tombstones += filter.stats().tombstones_dropped;
    shadowed += filter.stats().shadowed_dropped;
    for (size_t i = 0; i < kept_entries.size(); i++) {
      if (kept_entries[i].type == ValueType::kDeletion) {
        kept_tombstones++;
      }
      if (i > 0 && kept_entries[i].user_key == kept_entries[i - 1].user_key) {
        kept_for_snapshots++;
      }
    }
  }
  // Measured with these seeds: 147, 769, 10616 and 2304. A filter that dropped nothing, or kept
  // everything, would pass the comparisons above, and these keep the test from drifting into
  // inputs that cannot tell it apart from one that does the right thing.
  EXPECT_GT(dropped_tombstones, 100u);
  EXPECT_GT(kept_tombstones, 500u);
  EXPECT_GT(shadowed, 5000u);
  EXPECT_GT(kept_for_snapshots, 1000u);
}

}  // namespace
}  // namespace shale
