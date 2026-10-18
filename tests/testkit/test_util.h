#ifndef SHALE_TESTKIT_TEST_UTIL_H_
#define SHALE_TESTKIT_TEST_UTIL_H_

// Helpers shared by the tests of every layer. They live in the shale_testkit library, which only
// test targets link.

#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "shale/iterator.h"
#include "shale/status.h"
#include "util/dbformat.h"

namespace shale {

class Random;

/// Succeeds if `s` is OK. Otherwise fails with the text of `s`, so that a test that checks a Status
/// reports what went wrong and not just that something did. Use it through ASSERT_OK / EXPECT_OK.
testing::AssertionResult IsOk(const Status& s);

/// Fails the test, and returns from the test body, unless the Status expression `expr` is OK.
#define ASSERT_OK(expr) ASSERT_TRUE(::shale::IsOk(expr))

/// Fails the test, and carries on, unless the Status expression `expr` is OK.
#define EXPECT_OK(expr) EXPECT_TRUE(::shale::IsOk(expr))

/// Returns `len` bytes drawn from `*rnd`, each a printable ASCII character (0x20 through 0x7e).
std::string RandomString(Random* rnd, size_t len);

/// Returns `len` bytes drawn from `*rnd`, each of any value from 0x00 through 0xff. It makes
/// tests that round-trip data check that nothing treats a zero byte, a newline or a byte above
/// 0x7f as special.
std::string RandomBinaryString(Random* rnd, size_t len);

/// A list of key-value pairs, which is what the iterator and table tests build their inputs from
/// and compare what they read with.
using Entries = std::vector<std::pair<std::string, std::string>>;

/// Returns the internal key for `user_key` at `sequence`, of type `type`: the user key and the tag.
std::string IKey(std::string_view user_key, SequenceNumber sequence,
                 ValueType type = ValueType::kValue);

/// Returns the user key "k" followed by `i` in six digits, "k000042": keys that sort as the
/// numbers do.
std::string UserKey(int i);

/// Returns the value that NumberedEntries() stores for `i`: "value-" and the number.
std::string ValueOf(int i);

/// Returns `n` entries in the order a table holds them: the internal key for UserKey(i) at
/// sequence number i + 1 and ValueOf(i), for i from 0 to n - 1.
Entries NumberedEntries(int n);

/// Returns an iterator over `sorted`, a list of key-value pairs in ascending bytewise order of key
/// (BytewiseCompare()), for tests of what is built on iterators: the two-level iterator, the
/// merging iterator. Seek() finds the first entry whose key is not less than the target. It
/// behaves as the Iterator contract says, and aborts where the contract is broken, on Next() or
/// key() when it is not valid.
///
/// Its status() is `status` at every moment, and the entries are served whatever it is, so a test
/// can give a child an error that shows while the child is still valid. An iterator that is
/// invalid because of an error is NewErrorIterator(), or NewFailingIterator() for one that has
/// entries first.
/// @param sorted the entries, sorted by key; keys may repeat, which Seek() does not mind.
/// @param status what status() returns.
std::unique_ptr<Iterator> NewVectorIterator(Entries sorted, Status status = {});

/// Returns an iterator that serves `sorted` as NewVectorIterator() does, and fails at the end of
/// them: once it has been positioned and has run off the last entry, whether by Next() or by a
/// seek that finds nothing, it is invalid and status() returns `error`, where it was OK until
/// then. It is what an iterator does that finds damage after it has returned some entries, so
/// that a test sees whether what is built on it takes the failure for the end of the data. A new
/// seek starts it over, as a block iterator that had not made its failure sticky would be.
/// @param sorted the entries it serves before it fails, sorted by key.
/// @param error what status() returns once it has failed; should not be OK.
std::unique_ptr<Iterator> NewFailingIterator(Entries sorted, Status error);

/// Creates a new, empty directory named `prefix` followed by random characters under
/// `testing::TempDir()` and returns its path, without a trailing slash. Aborts if the directory
/// cannot be created. The caller removes it; the helper does not, so that a failing test leaves
/// its files behind to be looked at.
std::string NewTempDir(const std::string& prefix);

}  // namespace shale

#endif  // SHALE_TESTKIT_TEST_UTIL_H_
