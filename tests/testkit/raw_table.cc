#include "testkit/raw_table.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "io/block.h"
#include "io/block_builder.h"
#include "io/table_format.h"
#include "shale/env.h"
#include "shale/iterator.h"
#include "shale/status.h"
#include "util/coding.h"
#include "util/crc32c.h"
#include "util/dbformat.h"

namespace shale {

BlockHandle RawTable::AddBlock(std::string_view contents) {
  const BlockHandle handle{file_.size(), contents.size()};
  file_.append(contents);
  std::string trailer(1, '\0');
  const uint32_t crc =
      crc32c::Extend(crc32c::Value(contents.data(), contents.size()), trailer.data(), 1);
  PutFixed32(&trailer, crc32c::Mask(crc));
  file_.append(trailer);
  return handle;
}

void RawTable::AddPadding(size_t bytes) { file_.append(bytes, '\x5a'); }

BlockHandle RawTable::AddEntries(const Entries& entries, int restart_interval) {
  BlockBuilder builder(restart_interval);
  for (const auto& [key, value] : entries) {
    builder.Add(key, value);
  }
  return AddBlock(builder.Finish());
}

BlockHandle RawTable::AddIndex(const std::vector<std::pair<std::string, BlockHandle>>& blocks) {
  Entries entries;
  for (const auto& [key, handle] : blocks) {
    std::string encoded;
    handle.EncodeTo(&encoded);
    entries.emplace_back(key, encoded);
  }
  return AddEntries(entries);
}

std::string RawTable::Finish(const BlockHandle& filter, const BlockHandle& index) {
  Footer footer;
  footer.filter = filter;
  footer.index = index;
  footer.EncodeTo(&file_);
  return file_;
}

std::vector<BlockHandle> DataBlockHandles(Env* env, const std::string& path) {
  std::vector<BlockHandle> handles;
  std::unique_ptr<RandomAccessFile> file;
  uint64_t size = 0;
  Status s = env->NewRandomAccessFile(path, &file);
  if (s.ok()) {
    s = env->GetFileSize(path, &size);
  }
  if (!s.ok() || size < kFooterSize) {
    ADD_FAILURE() << "cannot read the footer of " << path << ": " << s.ToString();
    return handles;
  }
  std::string scratch(kFooterSize, '\0');
  std::string_view footer_bytes;
  s = file->Read(size - kFooterSize, kFooterSize, &footer_bytes, scratch.data());
  Footer footer;
  if (s.ok()) {
    s = footer.DecodeFrom(footer_bytes);
  }
  BlockContents contents;
  if (s.ok()) {
    s = ReadBlock(file.get(), footer.index, /*verify_checksum=*/true, &contents);
  }
  if (!s.ok()) {
    ADD_FAILURE() << "cannot read the index of " << path << ": " << s.ToString();
    return handles;
  }
  std::unique_ptr<Iterator> it = NewBlockIterator(
      std::make_shared<const Block>(std::move(contents)), CompareInternalKey, kInternalKeyTagSize);
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    std::string_view value = it->value();
    BlockHandle handle;
    s = handle.DecodeFrom(&value);
    if (!s.ok()) {
      ADD_FAILURE() << "bad handle in the index of " << path << ": " << s.ToString();
      return {};
    }
    handles.push_back(handle);
  }
  EXPECT_TRUE(it->status().ok()) << it->status().ToString();
  return handles;
}

}  // namespace shale
