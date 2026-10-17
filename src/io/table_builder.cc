#include "io/table_builder.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "io/table_format.h"
#include "shale/status.h"
#include "util/check.h"
#include "util/coding.h"
#include "util/crc32c.h"
#include "util/dbformat.h"

namespace shale {

namespace {

// The type byte of a block stored as it is, the only kind this builder writes.
constexpr char kBlockTypeRaw = 0;

}  // namespace

TableBuilder::TableBuilder(const TableBuilderOptions& options, WritableFile* file)
    : options_(options),
      file_(file),
      data_block_(options.block_restart_interval),
      index_block_(1),
      filter_(options.bloom_bits_per_key) {
  SHALE_CHECK(file != nullptr, "TableBuilder needs a file");
}

TableBuilder::~TableBuilder() {
  SHALE_CHECK(ended_, "TableBuilder destroyed without Finish or Abandon");
}

void TableBuilder::Add(std::string_view internal_key, std::string_view value) {
  SHALE_CHECK(!ended_, "TableBuilder::Add after Finish or Abandon");
  SHALE_CHECK(internal_key.size() >= kInternalKeyTagSize,
              "TableBuilder::Add needs an internal key");
  if (num_entries_ > 0) {
    SHALE_CHECK(CompareInternalKey(last_key_, internal_key) < 0,
                "keys must be added in increasing order");
  }

  // Every version of a user key is in the table, one after another, and the filter is asked about
  // user keys, so it needs each of them once. Adding it again would only make the filter bigger.
  const std::string_view user_key = ExtractUserKey(internal_key);
  const bool new_user_key = num_entries_ == 0 || user_key != ExtractUserKey(last_key_);
  last_key_.assign(internal_key);
  ++num_entries_;
  if (!status_.ok()) {
    return;
  }

  if (new_user_key) {
    filter_.AddKey(user_key);
  }
  data_block_.Add(internal_key, value);
  if (data_block_.CurrentSizeEstimate() >= options_.block_size) {
    FlushDataBlock();
  }
}

void TableBuilder::FlushDataBlock() {
  if (data_block_.empty() || !status_.ok()) {
    return;
  }
  BlockHandle handle;
  WriteBlock(data_block_.Finish(), &handle);
  if (!status_.ok()) {
    return;
  }
  // The block ends at the key added last, which is the greatest it holds; a flush follows an Add
  // at once or ends the table, so nothing has been added since.
  std::string encoded_handle;
  handle.EncodeTo(&encoded_handle);
  index_block_.Add(last_key_, encoded_handle);
  data_block_.Reset();
}

void TableBuilder::WriteBlock(std::string_view contents, BlockHandle* handle) {
  handle->offset = offset_;
  handle->size = contents.size();
  Append(contents);

  // The checksum covers the contents and then the type byte.
  char trailer[kBlockTrailerSize];
  trailer[0] = kBlockTypeRaw;
  const uint32_t crc = crc32c::Extend(crc32c::Value(contents.data(), contents.size()), trailer, 1);
  EncodeFixed32(trailer + 1, crc32c::Mask(crc));
  Append(std::string_view(trailer, kBlockTrailerSize));
}

void TableBuilder::Append(std::string_view data) {
  if (!status_.ok()) {
    return;
  }
  Status s = file_->Append(data);
  if (s.ok()) {
    offset_ += data.size();
  } else {
    status_ = std::move(s);
  }
}

Status TableBuilder::Finish() {
  SHALE_CHECK(!ended_, "TableBuilder::Finish after Finish or Abandon");
  ended_ = true;
  FlushDataBlock();

  // The filter block is there whether or not there is a filter, empty if there is none, so that
  // every table has the same shape and a reader has no case for a missing block.
  BlockHandle filter_handle;
  const std::string filter = filter_.Finish();
  WriteBlock(filter, &filter_handle);
  BlockHandle index_handle;
  WriteBlock(index_block_.Finish(), &index_handle);

  if (status_.ok()) {
    Footer footer;
    footer.filter = filter_handle;
    footer.index = index_handle;
    std::string encoded;
    footer.EncodeTo(&encoded);
    Append(encoded);
  }
  return status_;
}

void TableBuilder::Abandon() {
  SHALE_CHECK(!ended_, "TableBuilder::Abandon after Finish or Abandon");
  ended_ = true;
}

}  // namespace shale
