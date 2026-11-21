#ifndef SHALE_TESTKIT_RAW_TABLE_H_
#define SHALE_TESTKIT_RAW_TABLE_H_

// A table written block by block, for tests of how a reader treats a table whose parts are intact
// and whose contents are wrong. It lives in the shale_testkit library, which only test targets
// link.

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "io/table_format.h"
#include "shale/env.h"
#include "testkit/test_util.h"

namespace shale {

// Builds a file out of blocks written by hand, each with a trailer that is right for what is in
// it, so that the damage a test puts in a block is found by the reader's own decoding of the
// block and not by a checksum.
class RawTable {
 public:
  BlockHandle AddBlock(std::string_view contents);

  // A block with one entry for each of `entries`, which are in the order given.
  BlockHandle AddEntries(const Entries& entries, int restart_interval = 1);

  // The index block for data blocks given as (last key, handle).
  BlockHandle AddIndex(const std::vector<std::pair<std::string, BlockHandle>>& blocks);

  // The file, ended with a footer that names `filter` and `index`.
  std::string Finish(const BlockHandle& filter, const BlockHandle& index);

 private:
  std::string file_;
};

// The handles of the data blocks of the table at `path`, in the order the index of its footer
// names them. A test fails, and the result is empty, if the table cannot be read.
std::vector<BlockHandle> DataBlockHandles(Env* env, const std::string& path);

}  // namespace shale

#endif  // SHALE_TESTKIT_RAW_TABLE_H_
