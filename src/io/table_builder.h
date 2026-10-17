#ifndef SHALE_IO_TABLE_BUILDER_H_
#define SHALE_IO_TABLE_BUILDER_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "io/block_builder.h"
#include "io/bloom.h"
#include "io/table_format.h"
#include "shale/env.h"
#include "shale/status.h"

namespace shale {

/// What shapes the table a TableBuilder writes. The defaults are the engine's.
struct TableBuilderOptions {
  /// The size at which a data block is cut. A larger block makes a smaller index and compresses
  /// the keys better, and costs more to read for each lookup, which reads a whole block.
  size_t block_size = 4096;
  /// How many entries go between two restart points in a data block.
  int block_restart_interval = 16;
  /// Bits of bloom filter for each user key; 0 writes a table without one.
  int bloom_bits_per_key = 10;
};

/// Writes a table, in the layout of table_format.h, from entries added in order.
///
/// Entries go into a data block until it reaches `block_size`, and the block is then written, with
/// an entry in the index for it. Finish() writes the last block, the filter, the index and the
/// footer. The index holds one entry for each data block: the last internal key in it, which is
/// the greatest key the block holds, and the block's handle. The index is built with a restart
/// point for every entry, so that a reader's search of it compares the whole key at every probe.
///
/// A write that fails makes the builder's status an error, and it writes nothing more: the file is
/// not a table, and the caller discards it. A builder has to be ended by Finish() or Abandon();
/// destroying one that was neither ends the process, because the file it leaves looks like a table
/// that was cut short, which is the worst kind of damage to meet later.
///
/// It does not own the file, and never syncs or closes it. It is not thread-safe.
class TableBuilder {
 public:
  /// Creates a builder that writes to `file`, which must be new and empty and must outlive the
  /// builder.
  /// @param options the shape of the table.
  /// @param file where the table goes.
  TableBuilder(const TableBuilderOptions& options, WritableFile* file);

  /// Aborts if the builder was not ended by Finish() or Abandon().
  ~TableBuilder();

  TableBuilder(const TableBuilder&) = delete;
  TableBuilder& operator=(const TableBuilder&) = delete;

  /// Adds an entry. Aborts if `internal_key` is shorter than the 8-byte tag of an internal key, if
  /// it does not sort strictly after the key added before it under CompareInternalKey() with
  /// "keys must be added in increasing order", or if the builder has been ended. If an earlier
  /// write failed this checks the order and then does nothing else.
  /// @param internal_key the key, user key and tag.
  /// @param value the value; empty for a deletion.
  void Add(std::string_view internal_key, std::string_view value);

  /// Returns OK, or the first error from the file.
  Status status() const { return status_; }

  /// Writes the last data block, the filter block, the index block and the footer, and returns the
  /// builder's status. Does not sync or close the file, and does not flush it: what the builder
  /// has written is in the file's buffer, as the file defines that, and the caller's Sync() and
  /// Close() make it durable. Aborts if the builder has been ended.
  Status Finish();

  /// Ends the builder without writing anything more, for a table that is not wanted. What was
  /// already written is in the file, which the caller discards. Aborts if the builder has been
  /// ended.
  void Abandon();

  /// Returns the number of entries added.
  uint64_t NumEntries() const { return num_entries_; }

  /// Returns the number of bytes handed to the file so far. Once Finish() has returned OK it is
  /// the size of the table.
  uint64_t FileSize() const { return offset_; }

 private:
  // Writes the data block being built, if it has entries, and its index entry.
  void FlushDataBlock();

  // Appends `contents` and its trailer to the file and sets `*handle` to where they went. Does
  // nothing if an earlier write failed.
  void WriteBlock(std::string_view contents, BlockHandle* handle);

  // Appends bytes to the file, or does nothing if a write has failed. Counts those that went in.
  void Append(std::string_view data);

  const TableBuilderOptions options_;
  WritableFile* const file_;
  BlockBuilder data_block_;
  BlockBuilder index_block_;
  BloomFilterBuilder filter_;
  std::string last_key_;  // The internal key added last.
  uint64_t num_entries_ = 0;
  uint64_t offset_ = 0;
  bool ended_ = false;  // Finish() or Abandon() has been called.
  Status status_;
};

}  // namespace shale

#endif  // SHALE_IO_TABLE_BUILDER_H_
