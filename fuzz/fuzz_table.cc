// Feeds the input to the table reader as the bytes of an SSTable file: opens it, and when it opens,
// iterates every entry and looks up the first key seen, with and without checksum verification.
//
// A fuzzer cannot compute a CRC32C, so the input as it is gets no further than the footer's
// checksum, or the first block's. The input is therefore read twice: as it is, and with the
// checksums that cover the footer, the index block, the filter block and every data block the index
// names set right for whatever those bytes now are. The second read is what reaches the validation
// of the index and the decoding of blocks that a table with damaged contents but a good checksum
// has, as a file written by something other than the engine would.
//
// What the table promises for any bytes: Open() returns OK or Corruption, and leaves `*table` null
// on failure; reads return the entry that is there or a Corruption, and an iterator walks the
// internal keys in strictly increasing order, so that its output can be fed to a TableBuilder.
//
// And for a table that nothing has touched, the input as it came with the checksums checked and a
// walk that found no damage, the target holds the lookups and the seeks to the walk itself: a key
// the walk returned is found, unless the filter says it is not there, and a seek lands on the first
// key of the walk that is not before its target. Those checks are what makes a Get that misses a
// key it holds, or a seek that skips an exact match, fail here and not in a database.

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fuzz_util.h"
#include "io/block.h"
#include "io/bloom.h"
#include "io/table.h"
#include "io/table_format.h"
#include "shale/env.h"
#include "shale/iterator.h"
#include "shale/status.h"
#include "util/coding.h"
#include "util/crc32c.h"
#include "util/dbformat.h"

namespace shale {
namespace {

// Offsets inside the footer, as table_format.h lays it out.
constexpr size_t kFooterFilterOffset = 0;
constexpr size_t kFooterIndexOffset = 16;
constexpr size_t kFooterChecksumOffset = 36;

// Sets the trailer of the block at `handle` right for the contents that are there, and leaves the
// block's type byte as it is: a table with a type the reader does not know is one to see refused.
// A handle that does not leave room for the block and its trailer in `file` is left alone.
void RepairTrailer(std::string* file, const BlockHandle& handle) {
  if (handle.offset > file->size() || handle.size > file->size() - handle.offset ||
      file->size() - handle.offset - handle.size < kBlockTrailerSize) {
    return;
  }
  const size_t trailer = static_cast<size_t>(handle.offset + handle.size);
  const uint32_t crc =
      crc32c::Extend(crc32c::Value(file->data() + handle.offset, static_cast<size_t>(handle.size)),
                     file->data() + trailer, 1);
  EncodeFixed32(file->data() + trailer + 1, crc32c::Mask(crc));
}

// Returns `bytes` with every checksum that the reader checks before it reads an entry made to
// match the bytes, as far as they can be found: the data blocks through the index block, which is
// read here as any block is, and so may name nothing at all.
std::string WithChecksumsRepaired(std::string_view bytes) {
  std::string file(bytes);
  if (file.size() < kFooterSize) {
    return file;
  }
  char* footer = file.data() + (file.size() - kFooterSize);
  BlockHandle filter;
  filter.offset = DecodeFixed64(footer + kFooterFilterOffset);
  filter.size = DecodeFixed64(footer + kFooterFilterOffset + 8);
  BlockHandle index;
  index.offset = DecodeFixed64(footer + kFooterIndexOffset);
  index.size = DecodeFixed64(footer + kFooterIndexOffset + 8);

  RepairTrailer(&file, filter);
  RepairTrailer(&file, index);
  if (index.offset <= file.size() && index.size <= file.size() - index.offset) {
    const auto block = std::make_shared<const Block>(
        file.substr(static_cast<size_t>(index.offset), static_cast<size_t>(index.size)));
    const std::unique_ptr<Iterator> it = NewBlockIterator(block, BytewiseCompare);
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      std::string_view value = it->value();
      BlockHandle data;
      if (data.DecodeFrom(&value).ok()) {
        RepairTrailer(&file, data);
      }
    }
  }
  // The footer's checksum goes last: a block the file holds may overlap the footer's bytes.
  footer = file.data() + (file.size() - kFooterSize);
  EncodeFixed32(footer + kFooterChecksumOffset, crc32c::Value(footer, kFooterChecksumOffset));
  return file;
}

// What a walk of the table returned, in order, and whether it then reached the end without an
// error.
struct Walk {
  std::vector<std::pair<std::string, std::string>> entries;
  bool clean = false;
};

// Walks the table with `options`, checking the order of its keys.
Walk Iterate(const Table& table, const TableReadOptions& options, size_t file_size) {
  Walk walk;
  const std::unique_ptr<Iterator> it = table.NewIterator(options);
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    const std::string_view key = it->key();
    // Every entry takes at least three bytes of a block, and no block is read twice.
    SHALE_FUZZ_CHECK(walk.entries.size() < file_size / 3 + 1);
    SHALE_FUZZ_CHECK(key.size() >= kInternalKeyTagSize);
    if (!walk.entries.empty()) {
      SHALE_FUZZ_CHECK(CompareInternalKey(walk.entries.back().first, key) < 0);
    }
    walk.entries.emplace_back(std::string(key), std::string(it->value()));
    fuzz::Touch(key);
    fuzz::Touch(it->value());
  }
  const Status s = it->status();
  SHALE_FUZZ_CHECK(s.ok() || s.IsCorruption());
  walk.clean = s.ok();
  return walk;
}

// Returns the internal key that sorts just after `key`: the same user key with the next smaller
// tag, since the larger tag of a user key is the newer entry and comes first. Empty if the tag is
// zero and there is none.
std::string JustAfter(std::string_view key) {
  std::string after(key);
  const uint64_t tag = DecodeFixed64(after.data() + after.size() - kInternalKeyTagSize);
  if (tag == 0) {
    return "";
  }
  EncodeFixed64(after.data() + after.size() - kInternalKeyTagSize, tag - 1);
  return after;
}

// What a table that nothing has touched must do, on top of what every table must. `walk` is what
// iterating it returned, in full and without an error, so the file is one the engine could have
// written: every checksum in it holds, so every block, the index and the filter are as they were
// written. Then a lookup of a key that the walk saw finds it, and a seek lands exactly where the
// order of the walk puts it, and these are tested against the walk itself, not against what the
// table might say of itself.
class ExactChecks {
 public:
  ExactChecks(const Table& table, const TableReadOptions& options, const Walk& walk,
              std::string_view bytes)
      : table_(table), options_(options), walk_(walk) {
    // The filter block is where the footer says; Open() has checked that it lies in the file.
    const char* footer = bytes.data() + (bytes.size() - kFooterSize);
    const uint64_t offset = DecodeFixed64(footer + kFooterFilterOffset);
    const uint64_t size = DecodeFixed64(footer + kFooterFilterOffset + 8);
    filter_ = std::string(bytes.substr(static_cast<size_t>(offset), static_cast<size_t>(size)));
  }

  // Checks the lookup of, and the seek to, every entry of the table, and keys that are before all
  // of it, just after each entry, and after all of it. These checks run only on a table whose
  // checksums hold, which in practice is a seed of a few dozen entries, so every entry costs little
  // and no bug can hide between samples.
  void Run() const {
    const size_t n = walk_.entries.size();
    for (size_t i = 0; i < n; i++) {
      CheckGetEntry(i);
      CheckNewest(i);
      CheckSeek(walk_.entries[i].first);
      const std::string after = JustAfter(walk_.entries[i].first);
      if (!after.empty()) {
        CheckSeek(after);
      }
    }
    // Before every entry: no user key is smaller than the empty one, and its newest entry first.
    std::string before;
    AppendInternalKey(&before, ParsedInternalKey{"", kMaxSequenceNumber, kValueTypeForSeek});
    CheckSeek(before);
    // After every entry: the largest user key there is, one byte longer.
    if (n != 0) {
      std::string after_all;
      AppendInternalKey(
          &after_all,
          ParsedInternalKey{std::string(ExtractUserKey(walk_.entries[n - 1].first)) + '\xff',
                            kMaxSequenceNumber, kValueTypeForSeek});
      CheckSeek(after_all);
    }
  }

 private:
  // The lookup of entry `i`'s own key returns that entry. The one answer that is not it is "not
  // found", and only the filter can give it: a filter says no for a user key it does not hold, and
  // that is all it may say no to.
  void CheckGetEntry(size_t i) const { Check(i, walk_.entries[i].first); }

  // The lookup that reads as of the newest sequence number returns the first entry of the user
  // key, which is the newest.
  void CheckNewest(size_t i) const {
    const std::string_view user_key = ExtractUserKey(walk_.entries[i].first);
    size_t first = i;
    while (first > 0 && ExtractUserKey(walk_.entries[first - 1].first) == user_key) {
      first--;
    }
    const LookupKey newest(user_key, kMaxSequenceNumber);
    Check(first, newest.internal_key());
  }

  // Looks `lookup_key` up and requires the answer to be that of entry `expected`.
  void Check(size_t expected, std::string_view lookup_key) const {
    const auto& [key, value] = walk_.entries[expected];
    ParsedInternalKey parsed;
    if (!ParseInternalKey(key, &parsed)) {
      return;  // a tag that is no kind of write: for the table to say, which the iteration did not
    }
    TableGetResult result;
    const Status s = table_.Get(options_, lookup_key, &result);
    SHALE_FUZZ_CHECK(s.ok());
    if (result.kind == TableGetResult::Kind::kNotFound) {
      SHALE_FUZZ_CHECK(!BloomFilterMayMatch(filter_, parsed.user_key));
    } else if (parsed.type == ValueType::kValue) {
      SHALE_FUZZ_CHECK(result.kind == TableGetResult::Kind::kFound);
      SHALE_FUZZ_CHECK(result.value == value);
    } else {
      SHALE_FUZZ_CHECK(result.kind == TableGetResult::Kind::kDeleted);
      SHALE_FUZZ_CHECK(result.value.empty());
    }
  }

  // A seek to `target` lands on the first entry of the walk that is not before it, or on nothing
  // if there is none, and the entries that follow are those of the walk.
  void CheckSeek(std::string_view target) const {
    const auto& entries = walk_.entries;
    size_t expected = 0;
    while (expected < entries.size() && CompareInternalKey(entries[expected].first, target) < 0) {
      expected++;
    }
    const std::unique_ptr<Iterator> it = table_.NewIterator(options_);
    it->Seek(target);
    for (int steps = 0; steps < 4 && expected < entries.size(); steps++, expected++, it->Next()) {
      SHALE_FUZZ_CHECK(it->Valid());
      SHALE_FUZZ_CHECK(it->key() == entries[expected].first);
      SHALE_FUZZ_CHECK(it->value() == entries[expected].second);
    }
    if (expected == entries.size()) {
      SHALE_FUZZ_CHECK(!it->Valid());
    }
    SHALE_FUZZ_CHECK(it->status().ok());
  }

  const Table& table_;
  const TableReadOptions options_;
  const Walk& walk_;
  std::string filter_;
};

// What any table must do, whatever its bytes: a seek lands at or after its target or the iteration
// ends with Corruption, since the table's iterator checks it, and a lookup answers or fails with
// Corruption. A table that the walk found damage in, or whose checksums were made to match, is held
// to no more: a lookup does not walk a block, and in one whose keys are out of order it may land
// elsewhere in it, and a file with forged checksums has no promise beyond staying inside its
// buffers.
void CheckAnyTable(const Table& table, const TableReadOptions& options, const Walk& walk) {
  if (walk.entries.empty()) {
    return;
  }
  const std::string& first = walk.entries.front().first;
  const std::unique_ptr<Iterator> it = table.NewIterator(options);
  it->Seek(first);
  for (int steps = 0; it->Valid() && steps < 8; steps++, it->Next()) {
    SHALE_FUZZ_CHECK(it->key().size() >= kInternalKeyTagSize);
    if (steps == 0) {
      SHALE_FUZZ_CHECK(CompareInternalKey(it->key(), first) >= 0);
    }
    fuzz::Touch(it->key());
    fuzz::Touch(it->value());
  }
  const Status seek_status = it->status();
  SHALE_FUZZ_CHECK(seek_status.ok() || seek_status.IsCorruption());

  TableGetResult result;
  const Status get_status = table.Get(options, first, &result);
  SHALE_FUZZ_CHECK(get_status.ok() || get_status.IsCorruption());
  const LookupKey newest(ExtractUserKey(first), kMaxSequenceNumber);
  const Status newest_status = table.Get(options, newest.internal_key(), &result);
  SHALE_FUZZ_CHECK(newest_status.ok() || newest_status.IsCorruption());
}

// Opens `bytes` and reads it. `untouched` says that they are the input as it came, and not a copy
// whose checksums were repaired, which is what lets the reads that a table that nothing has damaged
// must pass be held to exactly.
void OpenAndRead(std::string_view bytes, bool untouched) {
  const std::string name = "/table.sst";
  const std::unique_ptr<Env> env = fuzz::EnvWithFile(name, bytes);
  std::unique_ptr<RandomAccessFile> file;
  SHALE_FUZZ_CHECK(env->NewRandomAccessFile(name, &file).ok());

  BlockCache cache(size_t{1} << 16);  // outlives the table, which reads through it
  std::shared_ptr<Table> table;
  const Status opened = Table::Open(std::move(file), bytes.size(), /*cache_id=*/1, &cache, &table);
  if (!opened.ok()) {
    SHALE_FUZZ_CHECK(opened.IsCorruption());
    SHALE_FUZZ_CHECK(table == nullptr);
    return;
  }
  SHALE_FUZZ_CHECK(table != nullptr);

  for (const bool verify : {true, false}) {
    TableReadOptions options;
    options.verify_checksums = verify;
    options.fill_cache = verify;
    const Walk walk = Iterate(*table, options, bytes.size());
    CheckAnyTable(*table, options, walk);
    // Only with the checksums checked is a block that came back one whose bytes were read as they
    // were written: without, a block that is damaged decodes into whatever its bytes say, and the
    // iteration can accept entries that a lookup, which has the block's index entry to go by, does
    // not find.
    if (untouched && verify && walk.clean) {
      const ExactChecks exact(*table, options, walk, bytes);
      exact.Run();
    }
  }
}

}  // namespace
}  // namespace shale

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::string_view bytes(reinterpret_cast<const char*>(data), size);
  shale::OpenAndRead(bytes, /*untouched=*/true);
  shale::OpenAndRead(shale::WithChecksumsRepaired(bytes), /*untouched=*/false);
  return 0;
}
