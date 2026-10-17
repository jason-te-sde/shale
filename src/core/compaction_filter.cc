#include "core/compaction_filter.h"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include "util/check.h"
#include "util/dbformat.h"

namespace shale {

namespace {

// Past every snapshot, since a snapshot is at most kMaxSequenceNumber: the stripe of the entries
// that no snapshot is at or after.
constexpr SequenceNumber kLatestStripe = kMaxSequenceNumber + 1;

}  // namespace

CompactionFilter::CompactionFilter(std::vector<SequenceNumber> snapshots)
    : snapshots_(std::move(snapshots)) {
  std::sort(snapshots_.begin(), snapshots_.end());
  SHALE_CHECK(snapshots_.empty() || snapshots_.back() <= kMaxSequenceNumber,
              "a snapshot sequence number is above kMaxSequenceNumber");
}

SequenceNumber CompactionFilter::StripeOf(SequenceNumber sequence) const {
  const auto it = std::lower_bound(snapshots_.begin(), snapshots_.end(), sequence);
  return it == snapshots_.end() ? kLatestStripe : *it;
}

CompactionFilter::Decision CompactionFilter::Next(const ParsedInternalKey& key,
                                                  bool is_base_level_for_key) {
  const int order = has_previous_ ? BytewiseCompare(key.user_key, previous_user_key_) : 1;
  SHALE_CHECK(order >= 0, "CompactionFilter: input is out of order: a user key goes backwards");
  const bool same_user_key = order == 0;
  SHALE_CHECK(!same_user_key || key.sequence <= previous_sequence_,
              "CompactionFilter: input is out of order: a sequence number rises within a user key");

  const SequenceNumber stripe = StripeOf(key.sequence);
  // Entries arrive newest first and a stripe is a range of sequence numbers, so the entries of one
  // user key that share a stripe are next to each other, and it is enough to look at the one
  // before. That entry's own fate does not matter: whether it was kept or was a deletion that was
  // dropped, everything in its stripe beneath it is hidden.
  const bool shadowed = same_user_key && stripe == previous_stripe_;

  if (!same_user_key) {
    previous_user_key_.assign(key.user_key);
    has_previous_ = true;
  }
  previous_sequence_ = key.sequence;
  previous_stripe_ = stripe;

  if (shadowed) {
    stats_.shadowed_dropped++;
    return Decision::kDrop;
  }
  // In the oldest stripe no snapshot is older than the deletion, so none needs the entries it
  // hides, and with nothing below to hide, the deletion itself is no use to anyone.
  const bool in_oldest_stripe = snapshots_.empty() || key.sequence <= snapshots_.front();
  if (key.type == ValueType::kDeletion && is_base_level_for_key && in_oldest_stripe) {
    stats_.tombstones_dropped++;
    return Decision::kDrop;
  }
  stats_.kept++;
  return Decision::kKeep;
}

}  // namespace shale
