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

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "fuzz_util.h"
#include "io/block.h"
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

// What the iterator returned first, and whether it then reached the end of the table without an
// error.
struct FirstEntry {
  bool seen = false;
  bool clean = false;
  std::string key;
  std::string value;
};

// Walks the table with `options`, checking the order of its keys, and returns the first entry.
FirstEntry Iterate(const Table& table, const TableReadOptions& options, size_t file_size) {
  FirstEntry first;
  const std::unique_ptr<Iterator> it = table.NewIterator(options);
  std::string previous;
  size_t entries = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    const std::string_view key = it->key();
    // Every entry takes at least three bytes of a block, and no block is read twice.
    SHALE_FUZZ_CHECK(++entries <= file_size / 3 + 1);
    SHALE_FUZZ_CHECK(key.size() >= kInternalKeyTagSize);
    if (entries == 1) {
      first.seen = true;
      first.key = std::string(key);
      first.value = std::string(it->value());
    } else {
      SHALE_FUZZ_CHECK(CompareInternalKey(previous, key) < 0);
    }
    previous = std::string(key);
    fuzz::Touch(key);
    fuzz::Touch(it->value());
  }
  const Status s = it->status();
  SHALE_FUZZ_CHECK(s.ok() || s.IsCorruption());
  first.clean = s.ok();
  return first;
}

// Seeks to `target`, which is the first entry's own key, and walks a few entries on. A seek lands
// at or after its target or the iteration ends with Corruption: the table's iterator checks it,
// since a scan that begins before the key it was asked to begin at returns what it was not asked
// for.
void CheckSeek(const Table& table, const TableReadOptions& options, std::string_view target) {
  const std::unique_ptr<Iterator> it = table.NewIterator(options);
  it->Seek(target);
  for (int steps = 0; it->Valid() && steps < 8; steps++, it->Next()) {
    SHALE_FUZZ_CHECK(it->key().size() >= kInternalKeyTagSize);
    if (steps == 0) {
      SHALE_FUZZ_CHECK(CompareInternalKey(it->key(), target) >= 0);
    }
    fuzz::Touch(it->key());
    fuzz::Touch(it->value());
  }
  const Status s = it->status();
  SHALE_FUZZ_CHECK(s.ok() || s.IsCorruption());
}

// Looks `lookup_key` up, which is the first entry's own key or one that finds the same entry.
//
// Any bytes give OK or Corruption. When the iteration walked the whole table and found nothing
// wrong, every block is in order and under its index key, so the first entry is the first one that
// a lookup of its key finds, and what comes back is that entry's, or nothing: the filter block may
// say that a key is not there which the data blocks hold. A table that the iteration found damage
// in is not held to that, since a lookup does not walk a block, and in one whose keys are out of
// order it may land elsewhere in it. A file with a checksum that was made to match has no promise
// beyond staying inside its buffers.
void CheckGet(const Table& table, const TableReadOptions& options, std::string_view lookup_key,
              const FirstEntry& first) {
  TableGetResult result;
  const Status s = table.Get(options, lookup_key, &result);
  SHALE_FUZZ_CHECK(s.ok() || s.IsCorruption());
  if (!first.clean || !s.ok() || result.kind == TableGetResult::Kind::kNotFound) {
    return;
  }
  ParsedInternalKey parsed;
  if (!ParseInternalKey(first.key, &parsed)) {
    return;
  }
  if (parsed.type == ValueType::kValue) {
    SHALE_FUZZ_CHECK(result.kind == TableGetResult::Kind::kFound);
    SHALE_FUZZ_CHECK(result.value == first.value);
  } else {
    SHALE_FUZZ_CHECK(result.kind == TableGetResult::Kind::kDeleted);
    SHALE_FUZZ_CHECK(result.value.empty());
  }
}

void OpenAndRead(std::string_view bytes) {
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
    const FirstEntry first = Iterate(*table, options, bytes.size());
    if (!first.seen) {
      continue;
    }
    CheckSeek(*table, options, first.key);
    CheckGet(*table, options, first.key, first);
    // The newest entry of the first user key, found as a read at the newest sequence number does.
    const LookupKey newest(ExtractUserKey(first.key), kMaxSequenceNumber);
    CheckGet(*table, options, newest.internal_key(), first);
  }
}

}  // namespace
}  // namespace shale

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::string_view bytes(reinterpret_cast<const char*>(data), size);
  shale::OpenAndRead(bytes);
  shale::OpenAndRead(shale::WithChecksumsRepaired(bytes));
  return 0;
}
