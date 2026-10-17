#ifndef SHALE_CORE_COMPACTION_FILTER_H_
#define SHALE_CORE_COMPACTION_FILTER_H_

// Which entries of a compaction's input survive it.
//
// A compaction merges several tables into a stream of internal keys in CompareInternalKey() order,
// for each user key newest first, and writes some of them out. What it may leave out is decided by
// who can still look: the latest state of the database, and every snapshot that is still open. An
// entry that none of them can see is garbage; an entry that any of them sees must stay, or that
// reader's answer changes. The filter decides this one entry at a time, and exactly, not
// conservatively: it keeps the newest entry that each observer sees and nothing else.
//
// This is the pure kernel's: no I/O, no clock, no threads, no locks.

#include <cstdint>
#include <string>
#include <vector>

#include "util/dbformat.h"

namespace shale {

/// Decides which entries of a compaction's input stream survive it. One filter serves one
/// compaction: it remembers the entry before, so the entries must reach Next() in the order of the
/// stream, every one of them, and a new compaction makes a new filter. It is not thread-safe.
///
/// The snapshots cut the sequence numbers into stripes. An entry's stripe is the smallest snapshot
/// that is at or after its sequence number, or "latest" if none is. The observers are the
/// snapshots and the latest state, one per stripe, and the observer of a stripe sees the newest
/// entry of each user key at or before its own sequence number, which is the newest entry of that
/// key in the stripe, or in an older one if the stripe holds none. So within one user key:
///  - an entry is shadowed, and dropped, when the entry before it, which is newer, is in the same
///    stripe: no observer reaches it, because each of them that could see it sees the newer one.
///  - a deletion is dropped as well when it is in the oldest stripe, that is when no snapshot is
///    older than it, and no level below the output can hold the key. Nothing older is left for it
///    to hide, and nobody older is left to need it.
/// Everything else is kept: it is the newest entry of its stripe, so its observer sees it.
class CompactionFilter {
 public:
  /// Creates a filter for a compaction that runs while `snapshots` are open: their sequence
  /// numbers, in any order, repeats allowed. Each must be at most kMaxSequenceNumber.
  explicit CompactionFilter(std::vector<SequenceNumber> snapshots);

  /// What to do with one entry.
  enum class Decision : uint8_t {
    kKeep,  ///< Write the entry to the output.
    kDrop,  ///< Leave the entry out.
  };

  /// Decides the next entry of the stream. `is_base_level_for_key` is true if no level deeper than
  /// the compaction's output level can hold `key.user_key`, which is what IsBaseLevelForKey()
  /// answers: a deletion is only dropped then, since otherwise the older value below would come
  /// back. The entries must arrive in CompareInternalKey() order: a user key never before the one
  /// before it, and within a user key a sequence number never above the one before it. Aborts if
  /// they do not, since an entry dropped on the wrong evidence is a lost write. The same internal
  /// key twice is not an error; the second is shadowed.
  [[nodiscard]] Decision Next(const ParsedInternalKey& key, bool is_base_level_for_key);

  /// What the filter has decided so far.
  struct Stats {
    /// Entries kept.
    uint64_t kept = 0;
    /// Entries dropped because a newer entry of the same user key in their stripe hides them.
    uint64_t shadowed_dropped = 0;
    /// Deletions dropped because nothing older is left for them to hide or to need them.
    uint64_t tombstones_dropped = 0;
  };

  /// Returns the counts of the decisions made by Next() so far.
  const Stats& stats() const { return stats_; }

 private:
  // The stripe of an entry: the smallest snapshot at or after its sequence number, or
  // kLatestStripe, which is past every sequence number a snapshot can have.
  SequenceNumber StripeOf(SequenceNumber sequence) const;

  std::vector<SequenceNumber> snapshots_;  // ascending
  bool has_previous_ = false;
  std::string previous_user_key_;
  SequenceNumber previous_sequence_ = 0;
  SequenceNumber previous_stripe_ = 0;
  Stats stats_;
};

}  // namespace shale

#endif  // SHALE_CORE_COMPACTION_FILTER_H_
