// Keeps docs/format.md true.
//
// A byte layout that is written in prose drifts from the code the first time somebody changes a
// constant and forgets the document. So the document carries its numbers in places this test can
// read, and this test checks every one of them against the code:
//
//   - a table of constants, each compared with the constant in the source, or, for a number the
//     code does not name, with what the code does (the first byte of an encoded edit, the number
//     of restart points of an index block);
//   - a table of the fixed-size structures, the offset and size of every field, which are used to
//     take real bytes apart: a field at the wrong offset reads the wrong number;
//   - a table of file names, compared with the functions that print and parse them;
//   - worked examples, hex dumps of what the encoders write for a stated input, compared byte for
//     byte, and fed to the decoders to show that they are what the decoders read.
//
// The document marks what is read with HTML comments (`<!-- table:constants -->` and its closing
// twin, `<!-- example:wal_record -->` before a fenced block), which Markdown does not show. The
// tests fail, and do not skip, when a marker is missing: a document that has lost its table must
// not pass.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/version_edit.h"
#include "db/filename_io.h"
#include "db/version_set.h"
#include "docs/doc_tables.h"
#include "io/block.h"
#include "io/block_builder.h"
#include "io/bloom.h"
#include "io/env_util.h"
#include "io/log_format.h"
#include "io/log_reader.h"
#include "io/log_writer.h"
#include "io/table_builder.h"
#include "io/table_format.h"
#include "io/write_batch_internal.h"
#include "shale/env.h"
#include "shale/iterator.h"
#include "shale/options.h"
#include "shale/status.h"
#include "shale/write_batch.h"
#include "testkit/test_util.h"
#include "util/coding.h"
#include "util/crc32c.h"
#include "util/dbformat.h"
#include "util/filename.h"
#include "util/hash.h"

namespace shale {
namespace {

// --- Reading the document ------------------------------------------------------------------------

using doc_tables::FencedBlocks;
using doc_tables::Hex;
using doc_tables::HexBytes;
using doc_tables::ParseNumber;
using doc_tables::TableRows;

std::vector<std::string> LoadLines() { return doc_tables::LoadLines(SHALE_FORMAT_DOC_PATH); }

// The constants table as a map. Checks that a name appears once and that its value is a number.
std::map<std::string, uint64_t> DocConstants(const std::vector<std::string>& lines) {
  std::map<std::string, uint64_t> constants;
  for (const auto& row : TableRows(lines, "constants", {"Constant", "Value", "Meaning"})) {
    uint64_t value = 0;
    if (!ParseNumber(row[1], &value)) {
      ADD_FAILURE() << "the value of '" << row[0] << "' is '" << row[1] << "', not a number";
      continue;
    }
    if (!constants.emplace(row[0], value).second) {
      ADD_FAILURE() << "the constant '" << row[0] << "' appears twice";
    }
  }
  return constants;
}

uint64_t DocConstant(const std::map<std::string, uint64_t>& constants, const std::string& name) {
  const auto it = constants.find(name);
  if (it == constants.end()) {
    ADD_FAILURE() << "docs/format.md has no constant '" << name << "'";
    return 0;
  }
  return it->second;
}

struct Field {
  std::string name;
  uint64_t offset = 0;
  uint64_t size = 0;
};

// The fixed layouts of the document, each structure's fields in the order of its rows.
std::map<std::string, std::vector<Field>> DocLayouts(const std::vector<std::string>& lines) {
  std::map<std::string, std::vector<Field>> layouts;
  for (const auto& row :
       TableRows(lines, "layouts", {"Structure", "Field", "Offset", "Size", "Encoding"})) {
    Field field;
    field.name = row[1];
    if (!ParseNumber(row[2], &field.offset) || !ParseNumber(row[3], &field.size)) {
      ADD_FAILURE() << "the offset or size of " << row[0] << "." << row[1] << " is not a number";
      continue;
    }
    layouts[row[0]].push_back(field);
  }
  return layouts;
}

// The field `name` of a structure, or an empty one, with a failure, if the document has none.
Field FieldOf(const std::map<std::string, std::vector<Field>>& layouts,
              const std::string& structure, const std::string& name) {
  const auto it = layouts.find(structure);
  if (it != layouts.end()) {
    for (const Field& field : it->second) {
      if (field.name == name) {
        return field;
      }
    }
  }
  ADD_FAILURE() << "docs/format.md has no field " << structure << "." << name;
  return Field{};
}

// Checks that the fields of `structure` follow one another with no gap and no overlap from the
// first byte, and that together they are `size` bytes.
void ExpectFieldsTile(const std::map<std::string, std::vector<Field>>& layouts,
                      const std::string& structure, uint64_t size) {
  const auto it = layouts.find(structure);
  ASSERT_TRUE(it != layouts.end()) << "docs/format.md has no layout for " << structure;
  uint64_t next = 0;
  for (const Field& field : it->second) {
    EXPECT_EQ(field.offset, next) << structure << "." << field.name << " does not start where the "
                                  << "field before it ends";
    next = field.offset + field.size;
  }
  EXPECT_EQ(next, size) << "the fields of " << structure << " do not add up to its size";
}

// The `size` bytes at `offset` of `bytes` as a little-endian integer.
uint64_t ReadLE(std::string_view bytes, const Field& field) {
  if (field.size == 0 || field.size > 8 || field.offset + field.size > bytes.size()) {
    ADD_FAILURE() << "the field " << field.name << " (offset " << field.offset << ", size "
                  << field.size << ") is not inside the " << bytes.size() << " bytes";
    return 0;
  }
  uint64_t value = 0;
  for (uint64_t i = field.size; i > 0; i--) {
    value = (value << 8) | uint64_t{static_cast<uint8_t>(bytes[field.offset + i - 1])};
  }
  return value;
}

// --- What the code writes ------------------------------------------------------------------------

// The batch of the examples: sequence number 7, a put of "key" and a deletion of "old".
std::string ExampleBatch() {
  WriteBatch batch;
  batch.Put("key", "value");
  batch.Delete("old");
  WriteBatchInternal::SetSequence(&batch, 7);
  return std::string(WriteBatchInternal::Contents(&batch));
}

// What a log file holds after one record of `record`.
std::string LogFileHolding(const std::string& record) {
  std::unique_ptr<Env> env = NewMemEnv();
  std::unique_ptr<WritableFile> file;
  EXPECT_OK(env->NewWritableFile("/wal.log", &file));
  {
    log::Writer writer(file.get());
    EXPECT_OK(writer.AddRecord(record));
  }
  EXPECT_OK(file->Close());
  std::string bytes;
  EXPECT_OK(ReadFileToString(env.get(), "/wal.log", &bytes));
  return bytes;
}

std::string ExampleDataBlock() {
  BlockBuilder builder(2);
  builder.Add("apple", "1");
  builder.Add("applesauce", "2");
  builder.Add("banana", "3");
  return std::string(builder.Finish());
}

// The trailer of a block with these contents: the type byte, and the masked CRC32C of the contents
// and that byte.
std::string TrailerOf(std::string_view contents) {
  std::string covered(contents);
  covered.push_back('\0');
  std::string trailer(1, '\0');
  PutFixed32(&trailer, crc32c::Mask(crc32c::Value(covered.data(), covered.size())));
  return trailer;
}

std::string ExampleFilter() {
  BloomFilterBuilder builder(10);
  builder.AddKey("apple");
  builder.AddKey("banana");
  return builder.Finish();
}

std::string ExampleFooter() {
  Footer footer;
  footer.filter = BlockHandle{4096, 9};
  footer.index = BlockHandle{4110, 40};
  std::string bytes;
  footer.EncodeTo(&bytes);
  return bytes;
}

std::string InternalKeyOf(std::string_view user_key, SequenceNumber sequence, ValueType type) {
  std::string key;
  ParsedInternalKey parsed;
  parsed.user_key = user_key;
  parsed.sequence = sequence;
  parsed.type = type;
  AppendInternalKey(&key, parsed);
  return key;
}

VersionEdit ExampleEdit() {
  VersionEdit edit;
  edit.log_number = 12;
  edit.next_file_number = 15;
  edit.last_sequence = 1000;
  edit.compact_pointers.emplace_back(1, InternalKeyOf("m", 9, ValueType::kValue));
  edit.deleted_files.emplace(1, 9);
  FileMetaData file;
  file.number = 14;
  file.file_size = 3000;
  file.smallest = InternalKeyOf("a", 5, ValueType::kValue);
  file.largest = InternalKeyOf("m", 9, ValueType::kValue);
  edit.new_files.emplace_back(2, file);
  return edit;
}

std::string Encoded(const VersionEdit& edit) {
  std::string bytes;
  edit.EncodeTo(&bytes);
  return bytes;
}

// The hex examples of the document, as the code writes them.
std::map<std::string, std::string> CodeExamples() {
  std::map<std::string, std::string> examples;
  const std::string batch = ExampleBatch();
  const std::string block = ExampleDataBlock();
  examples["write_batch"] = batch;
  examples["wal_record"] = LogFileHolding(batch);
  examples["data_block"] = block;
  examples["block_trailer"] = TrailerOf(block);
  examples["filter_block"] = ExampleFilter();
  examples["footer"] = ExampleFooter();
  examples["version_edit"] = Encoded(ExampleEdit());
  examples["internal_key"] = InternalKeyOf("k", 7, ValueType::kValue);
  examples["memtable_lookup_key"] = std::string(LookupKey("k", 7).memtable_key());
  return examples;
}

// A table of a few data blocks, written to memory, and where its parts are.
struct TinyTable {
  std::string bytes;
  Footer footer;
  std::string index;  // the contents of the index block
};

TinyTable BuildTinyTable() {
  TinyTable table;
  std::unique_ptr<Env> env = NewMemEnv();
  std::unique_ptr<WritableFile> file;
  EXPECT_OK(env->NewWritableFile("/t.sst", &file));
  TableBuilderOptions options;
  options.block_size = 256;  // small, so that there are several data blocks and index entries
  {
    TableBuilder builder(options, file.get());
    for (const auto& [key, value] : NumberedEntries(200)) {
      builder.Add(key, value);
    }
    EXPECT_OK(builder.Finish());
  }
  EXPECT_OK(file->Close());
  EXPECT_OK(ReadFileToString(env.get(), "/t.sst", &table.bytes));
  EXPECT_GE(table.bytes.size(), kFooterSize);
  if (table.bytes.size() >= kFooterSize) {
    EXPECT_OK(table.footer.DecodeFrom(
        std::string_view(table.bytes).substr(table.bytes.size() - kFooterSize)));
  }
  std::unique_ptr<RandomAccessFile> reader;
  EXPECT_OK(env->NewRandomAccessFile("/t.sst", &reader));
  BlockContents index;
  EXPECT_OK(ReadBlock(reader.get(), table.footer.index, true, &index));
  table.index = std::string(index.view());
  return table;
}

// The constants of the document that the code does not name as a constant, found by looking at what
// the code does.
uint64_t FirstByteOfEdit(const VersionEdit& edit) {
  const std::string bytes = Encoded(edit);
  EXPECT_FALSE(bytes.empty());
  if (bytes.empty()) {
    return 0;
  }
  return uint64_t{static_cast<uint8_t>(bytes[0])};
}

uint64_t ByteAfterBatchHeader(bool put) {
  WriteBatch batch;
  if (put) {
    batch.Put("k", "v");
  } else {
    batch.Delete("k");
  }
  const std::string_view contents = WriteBatchInternal::Contents(&batch);
  EXPECT_GT(contents.size(), size_t{12});
  if (contents.size() <= 12) {
    return 0;
  }
  return uint64_t{static_cast<uint8_t>(contents[12])};
}

// How many index entries each restart point serves in the index block of a table.
uint64_t IndexEntriesPerRestart(const TinyTable& table) {
  const std::unique_ptr<Iterator> it = NewBlockIterator(std::make_shared<const Block>(table.index),
                                                        CompareInternalKey, kInternalKeyTagSize);
  uint64_t entries = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    entries++;
  }
  EXPECT_OK(it->status());
  EXPECT_GT(entries, uint64_t{2}) << "the table has too few data blocks for this check";
  EXPECT_GE(table.index.size(), size_t{4});
  if (table.index.size() < 4) {
    return 0;
  }
  const uint64_t restarts = DecodeFixed32(table.index.data() + table.index.size() - 4);
  if (restarts == 0) {
    return 0;
  }
  return entries / restarts;
}

uint64_t TrailerTypeByteOfFilterBlock(const TinyTable& table) {
  const uint64_t at = table.footer.filter.offset + table.footer.filter.size;
  EXPECT_LT(at, table.bytes.size());
  if (at >= table.bytes.size()) {
    return 99;
  }
  return uint64_t{static_cast<uint8_t>(table.bytes[at])};
}

// Every constant of the document that the code can supply, by the name the document gives it.
std::map<std::string, uint64_t> CodeConstants() {
  std::map<std::string, uint64_t> c;
  c["log::kBlockSize"] = log::kBlockSize;
  c["log::kHeaderSize"] = log::kHeaderSize;
  c["log::kZeroType"] = log::kZeroType;
  c["log::kFullType"] = log::kFullType;
  c["log::kFirstType"] = log::kFirstType;
  c["log::kMiddleType"] = log::kMiddleType;
  c["log::kLastType"] = log::kLastType;
  c["kBlockTrailerSize"] = kBlockTrailerSize;
  c["block_type_uncompressed"] = TrailerTypeByteOfFilterBlock(BuildTinyTable());
  c["kFooterSize"] = kFooterSize;
  c["kTableFormatVersion"] = kTableFormatVersion;
  c["kTableMagicNumber"] = kTableMagicNumber;
  c["index_block_restart_interval"] = IndexEntriesPerRestart(BuildTinyTable());
  c["Options::block_size"] = Options{}.block_size;
  c["Options::block_restart_interval"] = static_cast<uint64_t>(Options{}.block_restart_interval);
  c["Options::bloom_bits_per_key"] = static_cast<uint64_t>(Options{}.bloom_bits_per_key);
  c["kInternalKeyTagSize"] = kInternalKeyTagSize;
  c["kMaxSequenceNumber"] = kMaxSequenceNumber;
  c["ValueType::kDeletion"] = static_cast<uint64_t>(ValueType::kDeletion);
  c["ValueType::kValue"] = static_cast<uint64_t>(ValueType::kValue);
  c["kNumLevels"] = static_cast<uint64_t>(kNumLevels);
  c["kMaxFileNumber"] = kMaxFileNumber;
  c["kMaxKeySize"] = kMaxKeySize;
  c["kMaxValueSize"] = kMaxValueSize;
  const WriteBatch empty_batch;
  c["write_batch_header_size"] = WriteBatchInternal::ByteSize(&empty_batch);
  c["write_batch_tag_deletion"] = ByteAfterBatchHeader(false);
  c["write_batch_tag_put"] = ByteAfterBatchHeader(true);
  VersionEdit edit;
  edit.log_number = 1;
  c["tag_log_number"] = FirstByteOfEdit(edit);
  edit = VersionEdit{};
  edit.next_file_number = 1;
  c["tag_next_file_number"] = FirstByteOfEdit(edit);
  edit = VersionEdit{};
  edit.last_sequence = 1;
  c["tag_last_sequence"] = FirstByteOfEdit(edit);
  edit = VersionEdit{};
  edit.compact_pointers.emplace_back(0, InternalKeyOf("k", 1, ValueType::kValue));
  c["tag_compact_pointer"] = FirstByteOfEdit(edit);
  edit = VersionEdit{};
  edit.deleted_files.emplace(0, 1);
  c["tag_deleted_file"] = FirstByteOfEdit(edit);
  edit = VersionEdit{};
  FileMetaData file;
  file.number = 1;
  file.file_size = 1;
  file.smallest = InternalKeyOf("a", 1, ValueType::kValue);
  file.largest = InternalKeyOf("b", 1, ValueType::kValue);
  edit.new_files.emplace_back(0, file);
  c["tag_new_file"] = FirstByteOfEdit(edit);
  return c;
}

// The constants of the document that no entry of CodeConstants() can check, because they are
// numbers inside a formula. Each is read from the document by the test that applies the formula
// to the code's own output.
const std::set<std::string>& ConstantsCheckedByTheirFormulas() {
  static const std::set<std::string> names = {
      "crc32c_mask_rotate",       "crc32c_mask_delta", "crc32c_check_value", "bloom_hash_seed",
      "bloom_probe_delta_rotate", "bloom_min_bits",    "bloom_max_probes",   "bloom_probe_percent",
      "hash_multiplier",          "hash_word_shift",   "hash_tail_shift"};
  return names;
}

// --- The tests -----------------------------------------------------------------------------------

TEST(FormatDocTest, TheConstantsTableAndTheCodeAgreeBothWays) {
  const std::vector<std::string> lines = LoadLines();
  const std::map<std::string, uint64_t> doc = DocConstants(lines);
  ASSERT_FALSE(doc.empty()) << "docs/format.md has no constants";
  const std::map<std::string, uint64_t> code = CodeConstants();
  const std::set<std::string>& formulas = ConstantsCheckedByTheirFormulas();

  for (const auto& [name, value] : doc) {
    if (formulas.count(name) != 0) {
      continue;
    }
    const auto it = code.find(name);
    if (it == code.end()) {
      ADD_FAILURE() << "docs/format.md documents '" << name << "', which this test does not check";
      continue;
    }
    EXPECT_EQ(value, it->second) << "docs/format.md says " << name << " is " << value
                                 << " and the code says " << it->second;
  }
  for (const auto& [name, value] : code) {
    EXPECT_TRUE(doc.count(name) != 0)
        << "the code has " << name << " (" << value << "), which docs/format.md does not list";
  }
  for (const std::string& name : formulas) {
    EXPECT_TRUE(doc.count(name) != 0) << "docs/format.md does not list '" << name << "'";
  }
}

TEST(FormatDocTest, TheFixedLayoutsTakeRealBytesApart) {
  const std::vector<std::string> lines = LoadLines();
  const auto layouts = DocLayouts(lines);
  ASSERT_FALSE(layouts.empty());
  const std::set<std::string> documented = [&] {
    std::set<std::string> names;
    for (const auto& entry : layouts) {
      names.insert(entry.first);
    }
    return names;
  }();
  EXPECT_EQ(documented, (std::set<std::string>{"block_trailer", "footer", "wal_record_header",
                                               "write_batch_header"}));

  // The header of a log record: the checksum covers the type byte and the payload.
  {
    ExpectFieldsTile(layouts, "wal_record_header", log::kHeaderSize);
    const std::string payload = "hello";
    const std::string file = LogFileHolding(payload);
    ASSERT_EQ(file.size(), log::kHeaderSize + payload.size());
    const Field checksum = FieldOf(layouts, "wal_record_header", "checksum");
    const Field length = FieldOf(layouts, "wal_record_header", "length");
    const Field type = FieldOf(layouts, "wal_record_header", "type");
    EXPECT_EQ(ReadLE(file, length), payload.size());
    EXPECT_EQ(ReadLE(file, type), uint64_t{log::kFullType});
    ASSERT_EQ(type.size, uint64_t{1});
    EXPECT_EQ(ReadLE(file, checksum),
              crc32c::Mask(crc32c::Value(file.data() + type.offset, 1 + payload.size())))
        << "the checksum is the masked CRC32C of the type byte and the payload";
    EXPECT_EQ(file.substr(log::kHeaderSize), payload);
  }

  // The footer: the CRC of the first 36 bytes is stored as it is, and not masked.
  {
    ExpectFieldsTile(layouts, "footer", kFooterSize);
    Footer footer;
    footer.filter = BlockHandle{123456, 789};
    footer.index = BlockHandle{7890123, 456};
    std::string bytes;
    footer.EncodeTo(&bytes);
    ASSERT_EQ(bytes.size(), kFooterSize);
    EXPECT_EQ(ReadLE(bytes, FieldOf(layouts, "footer", "filter_offset")), uint64_t{123456});
    EXPECT_EQ(ReadLE(bytes, FieldOf(layouts, "footer", "filter_size")), uint64_t{789});
    EXPECT_EQ(ReadLE(bytes, FieldOf(layouts, "footer", "index_offset")), uint64_t{7890123});
    EXPECT_EQ(ReadLE(bytes, FieldOf(layouts, "footer", "index_size")), uint64_t{456});
    EXPECT_EQ(ReadLE(bytes, FieldOf(layouts, "footer", "format_version")), kTableFormatVersion);
    EXPECT_EQ(ReadLE(bytes, FieldOf(layouts, "footer", "magic")), kTableMagicNumber);
    const Field checksum = FieldOf(layouts, "footer", "checksum");
    EXPECT_EQ(ReadLE(bytes, checksum), crc32c::Value(bytes.data(), checksum.offset))
        << "the checksum covers every byte before it and is not masked";
    EXPECT_NE(ReadLE(bytes, checksum), crc32c::Mask(crc32c::Value(bytes.data(), checksum.offset)));
  }

  // The trailer after a block: read from a real table, after its filter block.
  {
    ExpectFieldsTile(layouts, "block_trailer", kBlockTrailerSize);
    const TinyTable table = BuildTinyTable();
    const BlockHandle filter = table.footer.filter;
    const std::string_view trailer =
        std::string_view(table.bytes).substr(filter.offset + filter.size, kBlockTrailerSize);
    ASSERT_EQ(trailer.size(), kBlockTrailerSize);
    const Field type = FieldOf(layouts, "block_trailer", "type");
    ASSERT_EQ(type.offset, uint64_t{0}) << "the type byte follows the contents directly, which "
                                           "is what lets the checksum cover both in one run";
    EXPECT_EQ(ReadLE(trailer, type), uint64_t{0});
    EXPECT_EQ(ReadLE(trailer, FieldOf(layouts, "block_trailer", "checksum")),
              crc32c::Mask(crc32c::Value(table.bytes.data() + filter.offset, filter.size + 1)))
        << "the checksum is the masked CRC32C of the contents and the type byte";
  }

  // The header of a batch: the sequence number of its first operation and the count.
  {
    ExpectFieldsTile(layouts, "write_batch_header", 12);
    const std::string batch = ExampleBatch();
    EXPECT_EQ(ReadLE(batch, FieldOf(layouts, "write_batch_header", "sequence")), uint64_t{7});
    EXPECT_EQ(ReadLE(batch, FieldOf(layouts, "write_batch_header", "count")), uint64_t{2});
  }
}

TEST(FormatDocTest, TheExamplesAreWhatTheCodeWrites) {
  const std::vector<std::string> lines = LoadLines();
  const auto blocks = FencedBlocks(lines, "example");
  ASSERT_FALSE(blocks.empty()) << "docs/format.md has no worked examples";
  const std::map<std::string, std::string> code = CodeExamples();

  for (const auto& [name, body] : blocks) {
    const auto it = code.find(name);
    if (it == code.end()) {
      ADD_FAILURE() << "docs/format.md has an example '" << name << "' this test does not make";
      continue;
    }
    const std::string documented = HexBytes(name, body);
    EXPECT_EQ(Hex(documented), Hex(it->second)) << "the example '" << name << "'";
  }
  for (const auto& [name, bytes] : code) {
    EXPECT_TRUE(blocks.count(name) != 0) << "docs/format.md has no example '" << name << "'";
  }
}

TEST(FormatDocTest, TheCurrentFileExampleIsWhatTheCodeWrites) {
  const std::vector<std::string> lines = LoadLines();
  const auto blocks = FencedBlocks(lines, "text");
  const auto it = blocks.find("current");
  ASSERT_TRUE(it != blocks.end()) << "docs/format.md has no '<!-- text:current -->' example";
  std::string documented;
  for (const std::string& line : it->second) {
    documented += line + "\n";
  }
  EXPECT_EQ(documented, CurrentFileContents("db", 7));
  // The checksum is the masked CRC32C of the first line with its newline, in lower-case hex.
  const std::string first_line = "MANIFEST-000007\n";
  char hex[9];
  std::snprintf(hex, sizeof hex, "%08x",
                crc32c::Mask(crc32c::Value(first_line.data(), first_line.size())));
  EXPECT_EQ(documented, first_line + "crc32c:" + hex + "\n");
}

TEST(FormatDocTest, TheReadersAcceptTheDocumentedBytes) {
  const std::vector<std::string> lines = LoadLines();
  const auto blocks = FencedBlocks(lines, "example");
  const auto bytes_of = [&](const std::string& name) {
    const auto it = blocks.find(name);
    if (it == blocks.end()) {
      ADD_FAILURE() << "docs/format.md has no example '" << name << "'";
      return std::string();
    }
    return HexBytes(name, it->second);
  };

  // A log file that holds the documented record yields the documented batch, and nothing else.
  {
    std::unique_ptr<Env> env = NewMemEnv();
    ASSERT_OK(WriteStringToFile(env.get(), bytes_of("wal_record"), "/wal.log", false));
    std::unique_ptr<SequentialFile> file;
    ASSERT_OK(env->NewSequentialFile("/wal.log", &file));
    class Reports : public log::Reader::Reporter {
     public:
      void Corruption(size_t bytes, const Status& status) override {
        ADD_FAILURE() << "the reader reported " << bytes << " bytes: " << status.ToString();
      }
    } reports;
    log::Reader reader(file.get(), &reports, /*checksum=*/true);
    std::string_view record;
    std::string scratch;
    ASSERT_TRUE(reader.ReadRecord(&record, &scratch));
    EXPECT_EQ(std::string(record), bytes_of("write_batch"));
    EXPECT_FALSE(reader.ReadRecord(&record, &scratch));

    // The batch has the operations the document says.
    WriteBatch batch;
    ASSERT_OK(WriteBatchInternal::SetContents(&batch, bytes_of("write_batch")));
    EXPECT_EQ(WriteBatchInternal::Sequence(&batch), SequenceNumber{7});
    class Recorder : public WriteBatch::Handler {
     public:
      void Put(std::string_view key, std::string_view value) override {
        operations.push_back("put " + std::string(key) + "=" + std::string(value));
      }
      void Delete(std::string_view key) override {
        operations.push_back("delete " + std::string(key));
      }
      std::vector<std::string> operations;
    } recorder;
    ASSERT_OK(batch.Iterate(&recorder));
    EXPECT_EQ(recorder.operations, (std::vector<std::string>{"put key=value", "delete old"}));
  }

  // The documented data block holds the three entries, and a seek finds the one after a restart
  // point as well as the ones before it.
  {
    const std::unique_ptr<Iterator> it =
        NewBlockIterator(std::make_shared<const Block>(bytes_of("data_block")), BytewiseCompare);
    std::vector<std::string> entries;
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      entries.push_back(std::string(it->key()) + "=" + std::string(it->value()));
    }
    ASSERT_OK(it->status());
    EXPECT_EQ(entries, (std::vector<std::string>{"apple=1", "applesauce=2", "banana=3"}));
    it->Seek("applesauce");
    ASSERT_TRUE(it->Valid());
    EXPECT_EQ(it->key(), "applesauce");
    it->Seek("b");
    ASSERT_TRUE(it->Valid());
    EXPECT_EQ(it->key(), "banana");
  }

  // The documented footer decodes to the handles it was made from.
  {
    Footer footer;
    ASSERT_OK(footer.DecodeFrom(bytes_of("footer")));
    EXPECT_EQ(footer.filter.offset, uint64_t{4096});
    EXPECT_EQ(footer.filter.size, uint64_t{9});
    EXPECT_EQ(footer.index.offset, uint64_t{4110});
    EXPECT_EQ(footer.index.size, uint64_t{40});
  }

  // The documented filter says "maybe" to the keys that went into it.
  {
    const std::string filter = bytes_of("filter_block");
    EXPECT_TRUE(BloomFilterMayMatch(filter, "apple"));
    EXPECT_TRUE(BloomFilterMayMatch(filter, "banana"));
  }

  // The documented edit decodes to the edit it was made from.
  {
    VersionEdit decoded;
    ASSERT_OK(decoded.DecodeFrom(bytes_of("version_edit")));
    EXPECT_EQ(decoded, ExampleEdit());
  }
}

TEST(FormatDocTest, TheChecksumMaskFollowsTheDocumentedFormula) {
  const std::vector<std::string> lines = LoadLines();
  const std::map<std::string, uint64_t> doc = DocConstants(lines);
  const auto rotate = static_cast<unsigned>(DocConstant(doc, "crc32c_mask_rotate"));
  const auto delta = static_cast<uint32_t>(DocConstant(doc, "crc32c_mask_delta"));
  ASSERT_GT(rotate, 0u);
  ASSERT_LT(rotate, 32u);
  for (const uint32_t crc : {uint32_t{0}, uint32_t{1}, uint32_t{0x8000}, uint32_t{0x12345678},
                             uint32_t{0xdeadbeef}, uint32_t{0xffffffff}}) {
    const uint32_t expected =
        static_cast<uint32_t>((crc >> rotate) | (crc << (32 - rotate))) + delta;
    EXPECT_EQ(crc32c::Mask(crc), expected) << "Mask(" << crc << ")";
    EXPECT_EQ(crc32c::Unmask(expected), crc);
  }
  // The checksum is CRC-32C, Castagnoli's polynomial: the standard check value of the nine digits.
  EXPECT_EQ(uint64_t{crc32c::Value("123456789", 9)}, DocConstant(doc, "crc32c_check_value"));
}

// The filter a builder writes, worked out from the rules of the document alone.
std::string FilterByTheDocument(const std::vector<std::string>& keys, int bits_per_key,
                                const std::map<std::string, uint64_t>& doc) {
  const auto seed = static_cast<uint32_t>(DocConstant(doc, "bloom_hash_seed"));
  const auto rotate = static_cast<unsigned>(DocConstant(doc, "bloom_probe_delta_rotate"));
  const uint64_t min_bits = DocConstant(doc, "bloom_min_bits");
  const auto percent = static_cast<int>(DocConstant(doc, "bloom_probe_percent"));
  const auto max_probes = static_cast<int>(DocConstant(doc, "bloom_max_probes"));
  const uint64_t bits =
      std::max<uint64_t>(keys.size() * static_cast<uint64_t>(bits_per_key), min_bits);
  const uint64_t bytes = (bits + 7) / 8;
  const int probes = std::clamp(bits_per_key * percent / 100, 1, max_probes);
  std::string filter(bytes + 1, '\0');
  filter[bytes] = static_cast<char>(probes);
  for (const std::string& key : keys) {
    uint32_t h = Hash(key.data(), key.size(), seed);
    const uint32_t delta = (h >> (32 - rotate)) | (h << rotate);
    for (int j = 0; j < probes; j++) {
      const uint64_t bit = h % (bytes * 8);
      filter[bit / 8] = static_cast<char>(filter[bit / 8] | (1 << (bit % 8)));
      h += delta;
    }
  }
  return filter;
}

TEST(FormatDocTest, TheFilterFollowsTheDocumentedRules) {
  const std::vector<std::string> lines = LoadLines();
  const std::map<std::string, uint64_t> doc = DocConstants(lines);

  for (const int bits_per_key : {1, 5, 10, 20, 30}) {
    for (const size_t count : {size_t{1}, size_t{2}, size_t{7}, size_t{100}}) {
      std::vector<std::string> keys;
      BloomFilterBuilder builder(bits_per_key);
      for (size_t i = 0; i < count; i++) {
        keys.push_back("key" + std::to_string(i * 7919));
        builder.AddKey(keys.back());
      }
      EXPECT_EQ(Hex(builder.Finish()), Hex(FilterByTheDocument(keys, bits_per_key, doc)))
          << count << " keys at " << bits_per_key << " bits per key";
    }
  }
  // More bits per key than the probes can use: the probe count stops at the ceiling.
  {
    BloomFilterBuilder builder(100);
    builder.AddKey("k");
    const std::string filter = builder.Finish();
    ASSERT_FALSE(filter.empty());
    EXPECT_EQ(uint64_t{static_cast<uint8_t>(filter.back())}, DocConstant(doc, "bloom_max_probes"));
  }
  // A filter of none: a table written with no bits per key has an empty filter block.
  {
    BloomFilterBuilder builder(0);
    builder.AddKey("k");
    EXPECT_TRUE(builder.Finish().empty());
  }
  // The smallest bit array, which a table of one key still gets.
  {
    BloomFilterBuilder builder(10);
    builder.AddKey("k");
    EXPECT_EQ(static_cast<uint64_t>((builder.Finish().size() - 1) * 8),
              DocConstant(doc, "bloom_min_bits"));
  }
}

// The hash of the filter, worked out from the rules of the document alone.
uint32_t HashByTheDocument(const std::string& data, uint32_t seed,
                           const std::map<std::string, uint64_t>& doc) {
  const auto m = static_cast<uint32_t>(DocConstant(doc, "hash_multiplier"));
  const auto word_shift = static_cast<unsigned>(DocConstant(doc, "hash_word_shift"));
  const auto tail_shift = static_cast<unsigned>(DocConstant(doc, "hash_tail_shift"));
  uint32_t h = seed ^ static_cast<uint32_t>(static_cast<uint64_t>(data.size()) * m);
  size_t i = 0;
  for (; data.size() - i >= 4; i += 4) {
    uint32_t word = 0;
    for (size_t j = 0; j < 4; j++) {
      word |= uint32_t{static_cast<uint8_t>(data[i + j])} << (8 * j);
    }
    h += word;
    h *= m;
    h ^= h >> word_shift;
  }
  const size_t left = data.size() - i;
  if (left > 0) {
    if (left == 3) {
      h += uint32_t{static_cast<uint8_t>(data[i + 2])} << 16;
    }
    if (left >= 2) {
      h += uint32_t{static_cast<uint8_t>(data[i + 1])} << 8;
    }
    h += uint32_t{static_cast<uint8_t>(data[i])};
    h *= m;
    h ^= h >> tail_shift;
  }
  return h;
}

TEST(FormatDocTest, TheHashFollowsTheDocumentedRules) {
  const std::vector<std::string> lines = LoadLines();
  const std::map<std::string, uint64_t> doc = DocConstants(lines);
  // Bytes that are not text, a zero among them, so that no length of input is treated as special.
  const std::string pattern = [] {
    std::string text = "shale";
    text.insert(text.begin(), {'\x01', static_cast<char>(0x80), static_cast<char>(0xfe), '\x7f'});
    text.push_back('\0');
    text.push_back(static_cast<char>(0xff));
    return text;
  }();
  for (size_t length = 0; length <= 9; length++) {
    for (const uint32_t seed : {uint32_t{0}, uint32_t{0xbc9f1d34}, uint32_t{0xffffffff}}) {
      const std::string data = pattern.substr(0, length);
      EXPECT_EQ(Hash(data.data(), data.size(), seed), HashByTheDocument(data, seed, doc))
          << length << " bytes, seed " << seed;
    }
  }
  // The hash of nothing is the seed.
  EXPECT_EQ(Hash(nullptr, 0, 12345), uint32_t{12345});
}

// The messages of the table are those that ParseCurrentFile() gives, each for a CURRENT that it is
// made for.
TEST(FormatDocTest, TheCurrentMessagesAreTheOnesTheCodeGives) {
  const std::vector<std::string> lines = LoadLines();
  const auto rows = TableRows(lines, "current_messages", {"Message", "When"});
  ASSERT_FALSE(rows.empty());
  std::set<std::string> documented;
  for (const auto& row : rows) {
    documented.insert(row[0]);
  }

  // A CURRENT whose checksum line is right for `first_line`.
  const auto with_checksum = [](const std::string& first_line) {
    char hex[9];
    std::snprintf(hex, sizeof hex, "%08x",
                  crc32c::Mask(crc32c::Value(first_line.data(), first_line.size())));
    return first_line + "crc32c:" + hex + "\n";
  };
  const std::vector<std::string> contents = {
      "",                                             // does not end with a newline
      "MANIFEST-000007",                              // nor does this
      "MANIFEST-000007\nthe checksum is not here\n",  // malformed
      "MANIFEST-000007\ncrc32c:00000000\n",           // a checksum that is not the name's
      with_checksum("garbage\n"),                     // right checksum, not a MANIFEST name
  };
  std::set<std::string> produced;
  for (const std::string& text : contents) {
    uint64_t number = 0;
    const Status s = ParseCurrentFile("db", text, &number);
    ASSERT_TRUE(s.IsCorruption()) << "'" << text << "': " << s.ToString();
    const std::string message = s.ToString();
    ASSERT_EQ(message.rfind("Corruption: ", 0), size_t{0}) << message;
    produced.insert(message.substr(std::string("Corruption: ").size()));
  }
  EXPECT_EQ(documented, produced);
  // And the form that is accepted.
  uint64_t number = 0;
  EXPECT_OK(ParseCurrentFile("db", CurrentFileContents("db", 7), &number));
  EXPECT_EQ(number, uint64_t{7});
}

TEST(FormatDocTest, TheFileNamesAreWhatTheCodePrintsAndParses) {
  const std::vector<std::string> lines = LoadLines();
  const auto rows = TableRows(lines, "files", {"Kind", "File name", "Number"});
  ASSERT_FALSE(rows.empty());

  struct Kind {
    FileType type;
    bool numbered;
  };
  const std::map<std::string, Kind> kinds = {
      {"log", {FileType::kLogFile, true}},
      {"table", {FileType::kTableFile, true}},
      {"manifest", {FileType::kDescriptorFile, true}},
      {"current", {FileType::kCurrentFile, false}},
      {"lock", {FileType::kLockFile, false}},
      {"info_log", {FileType::kInfoLogFile, false}},
      {"old_info_log", {FileType::kInfoLogFile, false}},
      {"temp", {FileType::kTempFile, true}},
  };
  std::set<std::string> seen;
  for (const auto& row : rows) {
    const std::string& kind = row[0];
    const std::string& name = row[1];
    const auto it = kinds.find(kind);
    if (it == kinds.end()) {
      ADD_FAILURE() << "docs/format.md has a file kind '" << kind << "' this test does not know";
      continue;
    }
    seen.insert(kind);
    uint64_t number = 0;
    if (it->second.numbered) {
      ASSERT_TRUE(ParseNumber(row[2], &number)) << "the number of " << name;
    } else {
      EXPECT_EQ(row[2], "-") << name << " has no number";
    }

    std::string path;
    if (kind == "log") {
      path = LogFileName("d", number);
    } else if (kind == "table") {
      path = TableFileName("d", number);
    } else if (kind == "manifest") {
      path = DescriptorFileName("d", number);
    } else if (kind == "current") {
      path = CurrentFileName("d");
    } else if (kind == "lock") {
      path = LockFileName("d");
    } else if (kind == "info_log") {
      path = InfoLogFileName("d");
    } else if (kind == "old_info_log") {
      path = OldInfoLogFileName("d");
    } else {
      path = TempFileName("d", number);
    }
    EXPECT_EQ("d/" + name, path) << "the " << kind << " file";

    uint64_t parsed_number = 12345;
    FileType parsed_type = FileType::kCurrentFile;
    ASSERT_TRUE(ParseFileName(name, &parsed_number, &parsed_type)) << name;
    EXPECT_EQ(parsed_type, it->second.type) << name;
    EXPECT_EQ(parsed_number, number) << name;
  }
  for (const auto& [kind, unused] : kinds) {
    EXPECT_TRUE(seen.count(kind) != 0) << "docs/format.md has no example of a " << kind << " file";
  }

  // Numbers are spelled one way. The names that are not it are not the database's, and are left
  // alone by recovery.
  uint64_t number = 0;
  FileType type = FileType::kCurrentFile;
  for (const char* name : {"9.log", "0009.log", "0000009.log", "MANIFEST-9", "9.dbtmp"}) {
    EXPECT_FALSE(ParseFileName(name, &number, &type)) << name;
  }
  EXPECT_TRUE(ParseFileName("1234567.sst", &number, &type));
  EXPECT_EQ(number, uint64_t{1234567});
}

}  // namespace
}  // namespace shale
