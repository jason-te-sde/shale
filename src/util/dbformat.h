#ifndef SHALE_UTIL_DBFORMAT_H_
#define SHALE_UTIL_DBFORMAT_H_

// The format of the keys the engine stores internally. A user key alone cannot say which write it
// belongs to, so every entry in the memtable, in the write-ahead log and in the tables carries an
// internal key: the user key followed by an 8-byte tag holding the sequence number of the write
// and what kind of write it was.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "util/check.h"
#include "util/coding.h"

namespace shale {

/// The position of a write in the database's history. Every write gets the next number, so a
/// larger one is newer. A snapshot is the sequence number at the moment it was taken.
using SequenceNumber = uint64_t;

/// What a stored entry says about its user key.
enum class ValueType : uint8_t {
  kDeletion = 0x0,  ///< The key was deleted; the entry carries no value.
  kValue = 0x1,     ///< The key was set to the value the entry carries.
};

/// The highest-numbered ValueType. A lookup key is built with it so that, at one sequence number,
/// it sorts at or before every entry for that sequence number whatever its type. Anything that
/// tests for a valid type compares against this, so adding a type means moving it.
inline constexpr ValueType kValueTypeForSeek = ValueType::kValue;

/// The largest sequence number. The tag keeps the type in its low 8 bits and has 56 left for the
/// sequence.
inline constexpr SequenceNumber kMaxSequenceNumber = (uint64_t{1} << 56) - 1;

/// The size in bytes of the tag that ends an internal key: the sequence number and the type packed
/// into a fixed64. An internal key is at least this long, and its user key is everything before
/// the last this many bytes. Code that reads keys from a file and cannot trust their length checks
/// it against this.
inline constexpr size_t kInternalKeyTagSize = 8;

/// An internal key taken apart. `user_key` views into whatever it was parsed from.
struct ParsedInternalKey {
  std::string_view user_key;
  SequenceNumber sequence = 0;
  ValueType type = ValueType::kValue;
};

/// Returns the tag for a sequence number and type: `(seq << 8) | type`. Aborts if `seq` is
/// larger than kMaxSequenceNumber, because the high bits would be lost and the entry would
/// silently get an older sequence number.
uint64_t PackSequenceAndType(SequenceNumber seq, ValueType t);

/// Appends the internal key for `key` to `*result`: the user key followed by the tag as a
/// little-endian fixed64.
void AppendInternalKey(std::string* result, const ParsedInternalKey& key);

/// Splits an internal key into its parts. Returns false, leaving `*result` untouched, if the key
/// is shorter than the 8-byte tag or the tag names a type that does not exist.
bool ParseInternalKey(std::string_view internal_key, ParsedInternalKey* result);

// The three functions below are defined here and not in dbformat.cc: an iterator calls them for
// every entry it steps over, and a call that cannot be inlined costs more than the comparison.

/// Returns the user key of an internal key, as a view into it. Aborts if the key is shorter than
/// the 8-byte tag, since no code path may hand it anything else.
inline std::string_view ExtractUserKey(std::string_view internal_key) {
  SHALE_CHECK(internal_key.size() >= kInternalKeyTagSize,
              "internal key is shorter than its 8-byte tag");
  return internal_key.substr(0, internal_key.size() - kInternalKeyTagSize);
}

/// A three-way comparison of two keys: negative if `a` sorts first, positive if `b` does, and
/// zero if they are equal, always returned as -1, 0 or 1.
using KeyCompareFn = int (*)(std::string_view a, std::string_view b);

/// Orders user keys by their bytes as unsigned values, and a key before any longer key it is a
/// prefix of. Returns -1, 0 or 1.
inline int BytewiseCompare(std::string_view a, std::string_view b) {
  // string_view::compare orders by unsigned bytes, since char_traits<char> does. Its result is
  // only specified by sign, and callers rely on -1, 0 or 1.
  const int r = a.compare(b);
  return (r > 0) - (r < 0);
}

/// Orders internal keys by user key ascending, then by tag descending, so that for one user key
/// the newest entry comes first, and at one sequence number a value comes before a deletion.
/// Returns -1, 0 or 1.
inline int CompareInternalKey(std::string_view a, std::string_view b) {
  const int r = BytewiseCompare(ExtractUserKey(a), ExtractUserKey(b));
  if (r != 0) {
    return r;
  }
  // Same user key: the larger tag, which is the newer entry, sorts first.
  const uint64_t a_tag = DecodeFixed64(a.data() + a.size() - kInternalKeyTagSize);
  const uint64_t b_tag = DecodeFixed64(b.data() + b.size() - kInternalKeyTagSize);
  if (a_tag > b_tag) {
    return -1;
  }
  if (a_tag < b_tag) {
    return 1;
  }
  return 0;
}

/// The key a point lookup searches for, in the forms the layers below it want.
///
/// It holds a user key and a sequence number, the newest write the lookup may see. The internal
/// key it builds sorts before every entry for that user key with a sequence number at or below
/// that one, and after every newer one, so seeking to it lands on the entry to return.
class LookupKey {
 public:
  /// Builds the lookup key for `user_key` as of `sequence`, which must not exceed
  /// kMaxSequenceNumber. The key's bytes are copied, so `user_key` need not outlive this object.
  LookupKey(std::string_view user_key, SequenceNumber sequence);

  /// Returns the key in the form the memtable stores it: a varint32 holding the length of the
  /// internal key, then the internal key. The view is valid for as long as this object lives.
  std::string_view memtable_key() const { return rep_; }

  /// Returns the internal key: the user key followed by the tag.
  std::string_view internal_key() const {
    return std::string_view(rep_).substr(internal_key_start_);
  }

  /// Returns the user key.
  std::string_view user_key() const {
    const std::string_view internal = internal_key();
    return internal.substr(0, internal.size() - kInternalKeyTagSize);
  }

 private:
  std::string rep_;
  size_t internal_key_start_ = 0;
};

}  // namespace shale

#endif  // SHALE_UTIL_DBFORMAT_H_
