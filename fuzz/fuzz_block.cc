// Feeds the input to the block decoder as the contents of a data block, walks every entry, and
// seeks to the first eight bytes of the input. The block is read once the way a data block is,
// under the order of internal keys with the 8-byte tag as the shortest key, and once under plain
// bytewise order, which takes any key at all.
//
// A block comes off a disk and is never trusted: reading it must stay inside it, end, and report
// what is wrong with it as a Corruption status on the iterator.
//
// That is all this target holds a block to: memory safety, ending, and the kind of status. It does
// not compare what Seek() finds with what the walk found, because it cannot: Seek() assumes the
// comparison orders the keys as the block was built, and a block whose restart array is out of
// order or whose restart entry shares bytes with a key before it makes Seek() land where
// SeekToFirst() never goes. That is documented behaviour of a damaged block and no finding. A block
// that is intact is held to its seeks by fuzz_table, whose checksums say that its blocks are as
// written.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "fuzz_util.h"
#include "io/block.h"
#include "shale/iterator.h"
#include "util/dbformat.h"

namespace shale {
namespace {

// An entry is three varints, at least one byte each, and its key and value: a block cannot hold
// more entries than a third of its size, and an iterator that goes on past that is not walking it.
size_t MostEntries(size_t block_size) { return block_size / 3 + 1; }

void CheckStatus(const Iterator& it) {
  // Nothing but damage ends the iteration early, and the decoder reads no file, so it has no other
  // error to report.
  const Status s = it.status();
  SHALE_FUZZ_CHECK(s.ok() || s.IsCorruption());
}

void Read(const std::shared_ptr<const Block>& block, KeyCompareFn cmp, size_t min_key_size,
          std::string_view target) {
  const size_t limit = MostEntries(block->size());

  const std::unique_ptr<Iterator> walk = NewBlockIterator(block, cmp, min_key_size);
  size_t entries = 0;
  for (walk->SeekToFirst(); walk->Valid(); walk->Next()) {
    SHALE_FUZZ_CHECK(++entries <= limit);
    // A key shorter than the reader said it takes is damage, and ends the iteration instead.
    SHALE_FUZZ_CHECK(walk->key().size() >= min_key_size);
    fuzz::Touch(walk->key());
    fuzz::Touch(walk->value());
  }
  CheckStatus(*walk);

  // A target shorter than the smallest key goes straight to the comparison, which the caller of
  // Seek() has to avoid: that is the contract of an internal-key comparison, not of the block.
  if (target.size() < min_key_size) {
    return;
  }
  const std::unique_ptr<Iterator> seek = NewBlockIterator(block, cmp, min_key_size);
  seek->Seek(target);
  entries = 0;
  for (; seek->Valid(); seek->Next()) {
    SHALE_FUZZ_CHECK(++entries <= limit);
    SHALE_FUZZ_CHECK(seek->key().size() >= min_key_size);
    fuzz::Touch(seek->key());
    fuzz::Touch(seek->value());
  }
  CheckStatus(*seek);
}

}  // namespace
}  // namespace shale

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::string_view bytes(reinterpret_cast<const char*>(data), size);
  const auto block = std::make_shared<const shale::Block>(std::string(bytes));
  const std::string_view target = bytes.substr(0, 8);
  shale::Read(block, shale::CompareInternalKey, shale::kInternalKeyTagSize, target);
  shale::Read(block, shale::BytewiseCompare, 0, target);
  return 0;
}
