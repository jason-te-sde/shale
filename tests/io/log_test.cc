// The write-ahead log's record format, and what its reader does with a damaged file.
//
// Crash recovery decides between "the log ends in a torn record, carry on" and "the log is damaged
// in the middle, refuse to open" from nothing but whether the reader reported something. So the
// tests below pin both halves of that rule: every way a file can simply stop is silent, and every
// other defect is reported.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "io/env_util.h"
#include "io/log_format.h"
#include "io/log_reader.h"
#include "io/log_writer.h"
#include "shale/env.h"
#include "shale/status.h"
#include "testkit/test_util.h"
#include "util/coding.h"
#include "util/crc32c.h"
#include "util/random.h"

namespace shale::log {
namespace {

using Records = std::vector<std::string>;

// Collects what a Reader reports. `messages` holds Status::ToString() of each report, so a test
// checks the category as well as the text.
struct ReportCollector : Reader::Reporter {
  size_t dropped = 0;
  std::vector<std::string> messages;

  void Corruption(size_t bytes, const Status& status) override {
    dropped += bytes;
    messages.push_back(status.ToString());
  }
};

// The text a Reader reports for the defect described by `what`.
std::string Report(std::string_view what) { return "Corruption: " + std::string(what); }

// Returns `partial` repeated, and cut, to exactly `n` bytes.
std::string BigString(std::string_view partial, size_t n) {
  std::string result;
  while (result.size() < n) {
    result.append(partial);
  }
  result.resize(n);
  return result;
}

std::string NumberString(int n) { return std::to_string(n) + "."; }

// Builds a string from byte values, so that a test can spell out bytes above 0x7f and zeros.
std::string Bytes(std::initializer_list<int> values) {
  std::string result;
  for (const int v : values) {
    result.push_back(static_cast<char>(v));
  }
  return result;
}

uint8_t ByteAt(std::string_view contents, size_t offset) {
  return static_cast<uint8_t>(contents.at(offset));
}

// One physical record, written the way the format says and without the Writer, so that a test can
// put into a file what the Writer never would.
std::string Fragment(RecordType type, std::string_view payload) {
  const char type_byte = static_cast<char>(type);
  const uint32_t crc = crc32c::Extend(crc32c::Value(&type_byte, 1), payload.data(), payload.size());
  std::string out(kHeaderSize, '\0');
  EncodeFixed32(out.data(), crc32c::Mask(crc));
  out[4] = static_cast<char>(payload.size() & 0xff);
  out[5] = static_cast<char>(payload.size() >> 8);
  out[6] = type_byte;
  out.append(payload);
  return out;
}

// Replaces the contents of `path` with `transform` applied to them.
template <typename Transform>
void RewriteFile(Env* env, const std::string& path, Transform transform) {
  std::string contents;
  ASSERT_OK(ReadFileToString(env, path, &contents));
  transform(&contents);
  ASSERT_OK(WriteStringToFile(env, contents, path, /*sync=*/false));
}

void SetByteInFile(Env* env, const std::string& path, size_t offset, uint8_t value) {
  RewriteFile(env, path, [&](std::string* contents) {
    ASSERT_LT(offset, contents->size());
    (*contents)[offset] = static_cast<char>(value);
  });
}

void IncrementByteInFile(Env* env, const std::string& path, size_t offset, int delta) {
  RewriteFile(env, path, [&](std::string* contents) {
    ASSERT_LT(offset, contents->size());
    (*contents)[offset] = static_cast<char>(ByteAt(*contents, offset) + delta);
  });
}

// Reads `contents` as a log, in a file of its own, until the Reader says it is done. If `zero_tail`
// is not null it receives how many zero bytes at the end the Reader dropped as the end of the log.
Records ReadContents(std::string_view contents, ReportCollector* reports, bool checksum = true,
                     uint64_t* zero_tail = nullptr) {
  const std::unique_ptr<Env> env = NewMemEnv();
  EXPECT_OK(WriteStringToFile(env.get(), contents, "/log", /*sync=*/false));
  std::unique_ptr<SequentialFile> source;
  EXPECT_OK(env->NewSequentialFile("/log", &source));
  Reader reader(source.get(), reports, checksum);
  Records records;
  std::string scratch;
  std::string_view record;
  while (reader.ReadRecord(&record, &scratch)) {
    records.emplace_back(record);
  }
  if (zero_tail != nullptr) {
    *zero_tail = reader.ZeroTailBytes();
  }
  return records;
}

// A sequential file over `data` whose reads succeed `good_reads` times and then fail.
class FailingSource : public SequentialFile {
 public:
  FailingSource(std::string data, int good_reads)
      : data_(std::move(data)), good_reads_(good_reads) {}
  Status Read(size_t n, std::string_view* result, char* scratch) override {
    if (good_reads_-- <= 0) {
      *result = {};
      return Status::IOError("injected read failure");
    }
    const size_t count = std::min(n, data_.size() - position_);
    data_.copy(scratch, count, position_);
    position_ += count;
    *result = std::string_view(scratch, count);
    return Status::OK();
  }
  Status Skip(uint64_t) override { return Status::OK(); }

 private:
  std::string data_;
  size_t position_ = 0;
  int good_reads_;
};

// Writes `records` with a Writer into a new file and returns the file's bytes.
std::string WriteContents(const Records& records) {
  const std::unique_ptr<Env> env = NewMemEnv();
  std::unique_ptr<WritableFile> dest;
  EXPECT_OK(env->NewWritableFile("/log", &dest));
  Writer writer(dest.get());
  for (const std::string& record : records) {
    EXPECT_OK(writer.AddRecord(record));
  }
  EXPECT_OK(dest->Close());
  std::string contents;
  EXPECT_OK(ReadFileToString(env.get(), "/log", &contents));
  return contents;
}

// A log in a MemEnv file, written through a Writer and read through a Reader.
class LogTest : public testing::Test {
 protected:
  void SetUp() override {
    env_ = NewMemEnv();
    ASSERT_OK(env_->NewWritableFile(path_, &dest_));
    writer_ = std::make_unique<Writer>(dest_.get());
  }

  void Write(std::string_view record) { ASSERT_OK(writer_->AddRecord(record)); }

  // Appends bytes the Writer knows nothing about, as a hand-written fragment. The Writer keeps its
  // own idea of where in the block it is, so a test that mixes the two keeps everything inside
  // the first block.
  void WriteRaw(std::string_view bytes) { ASSERT_OK(dest_->Append(bytes)); }

  std::string Contents() {
    std::string contents;
    EXPECT_OK(ReadFileToString(env_.get(), path_, &contents));
    return contents;
  }

  size_t FileSize() { return Contents().size(); }

  // Closes the Writer's handle. A test that rewrites the file does it first, because the rewrite
  // replaces the file and the handle would go on writing to the old one.
  void CloseWriter() {
    if (dest_ != nullptr) {
      ASSERT_OK(dest_->Close());
      writer_.reset();
      dest_.reset();
    }
  }

  void SetByte(size_t offset, uint8_t value) {
    CloseWriter();
    SetByteInFile(env_.get(), path_, offset, value);
  }

  void IncrementByte(size_t offset, int delta) {
    CloseWriter();
    IncrementByteInFile(env_.get(), path_, offset, delta);
  }

  // Sets the length field of the physical record that starts at `offset`.
  void SetLengthField(size_t offset, size_t length) {
    SetByte(offset + 4, static_cast<uint8_t>(length & 0xff));
    SetByte(offset + 5, static_cast<uint8_t>(length >> 8));
  }

  void Truncate(size_t size) {
    CloseWriter();
    RewriteFile(env_.get(), path_, [&](std::string* contents) { contents->resize(size); });
  }

  // Continues the file with a second Writer, as a process that found it on disk would: the new
  // handle starts with what the file holds and the Writer is told how long that is.
  void Resume() {
    const std::string existing = Contents();
    CloseWriter();
    ASSERT_OK(env_->NewWritableFile(path_, &dest_));
    ASSERT_OK(dest_->Append(existing));
    writer_ = std::make_unique<Writer>(dest_.get(), existing.size());
  }

  // Starts a Reader over the file as it is now.
  void StartReading(bool checksum = true) {
    ASSERT_OK(env_->NewSequentialFile(path_, &source_));
    reader_ = std::make_unique<Reader>(source_.get(), &reports_, checksum);
  }

  // The next record, or nullopt at the end of the log.
  std::optional<std::string> Read() {
    if (reader_ == nullptr) {
      StartReading();
    }
    std::string scratch;
    std::string_view record;
    if (!reader_->ReadRecord(&record, &scratch)) {
      return std::nullopt;
    }
    return std::string(record);
  }

  Records ReadAll() {
    Records records;
    while (std::optional<std::string> record = Read()) {
      records.push_back(*std::move(record));
    }
    return records;
  }

  void ExpectNoReports() {
    EXPECT_EQ(reports_.messages, Records{}) << "the reader reported damage in an intact log";
    EXPECT_EQ(reports_.dropped, 0u);
  }

  const std::string path_ = "/log";
  std::unique_ptr<Env> env_;
  std::unique_ptr<WritableFile> dest_;
  std::unique_ptr<Writer> writer_;
  std::unique_ptr<SequentialFile> source_;
  ReportCollector reports_;
  std::unique_ptr<Reader> reader_;
};

TEST_F(LogTest, EmptyLogReadsNothing) {
  EXPECT_EQ(Read(), std::nullopt);
  EXPECT_EQ(Read(), std::nullopt) << "the end of the log is not a state that ReadRecord leaves";
  ExpectNoReports();
}

TEST_F(LogTest, ReadWriteInOrder) {
  Write("foo");
  Write("bar");
  Write("");
  Write("xxxx");
  EXPECT_EQ(ReadAll(), (Records{"foo", "bar", "", "xxxx"}));
  EXPECT_EQ(Read(), std::nullopt);
  ExpectNoReports();
}

TEST_F(LogTest, ManyBlocks) {
  constexpr int kRecords = 100000;
  for (int i = 0; i < kRecords; i++) {
    Write(NumberString(i));
  }
  ASSERT_GT(FileSize(), 10 * kBlockSize) << "the test no longer spans many blocks";
  for (int i = 0; i < kRecords; i++) {
    const std::optional<std::string> record = Read();
    ASSERT_TRUE(record.has_value()) << "the log ended before record " << i;
    ASSERT_EQ(*record, NumberString(i)) << "record " << i;
  }
  EXPECT_EQ(Read(), std::nullopt);
  ExpectNoReports();
}

TEST_F(LogTest, HeaderLayoutIsMaskedCrcLengthType) {
  // The expected bytes were computed by an independent CRC32C, not by the code under test: the
  // masked CRC of the type byte followed by the payload, little-endian, then length, then type.
  Write("foo");
  EXPECT_EQ(Contents(), Bytes({0xdd, 0x5f, 0xb3, 0x7a, 0x03, 0x00, 0x01, 'f', 'o', 'o'}));

  Write("");
  EXPECT_EQ(Contents().substr(10), Bytes({0x05, 0x2b, 0x28, 0x43, 0x00, 0x00, 0x01}));

  // The hand-written fragments of the other tests must be what the format says too.
  EXPECT_EQ(Fragment(kFirstType, "hello"),
            Bytes({0x22, 0x92, 0xe3, 0xc4, 0x05, 0x00, 0x02, 'h', 'e', 'l', 'l', 'o'}));

  // A length above 255 puts its high byte in the second length byte.
  Write(std::string(300, 'x'));
  const std::string contents = Contents();
  EXPECT_EQ(ByteAt(contents, 17 + 4), 0x2c);
  EXPECT_EQ(ByteAt(contents, 17 + 5), 0x01);
  EXPECT_EQ(ByteAt(contents, 17 + 6), kFullType);
}

TEST_F(LogTest, FragmentedRecordUsesFirstMiddleLast) {
  Random rnd(301);
  const std::string big = RandomBinaryString(&rnd, 3 * kBlockSize);
  Write(big);
  const std::string contents = Contents();
  EXPECT_EQ(ByteAt(contents, 6), kFirstType);
  EXPECT_EQ(ByteAt(contents, kBlockSize + 6), kMiddleType);
  EXPECT_EQ(ByteAt(contents, 3 * kBlockSize + 6), kLastType);
  EXPECT_EQ(ReadAll(), Records{big});
  ExpectNoReports();
}

TEST_F(LogTest, MarginalTrailerExactlyOneHeader) {
  // The first record leaves exactly a header's worth of bytes. The Writer fills them with a
  // zero-length FIRST fragment and carries the whole of the next record to the next block.
  const std::string first = BigString("foo", kBlockSize - 2 * kHeaderSize);
  Write(first);
  Write("bar");
  const std::string contents = Contents();
  ASSERT_EQ(contents.size(), kBlockSize + kHeaderSize + 3);
  EXPECT_EQ(ByteAt(contents, kBlockSize - kHeaderSize + 4), 0);
  EXPECT_EQ(ByteAt(contents, kBlockSize - kHeaderSize + 5), 0);
  EXPECT_EQ(ByteAt(contents, kBlockSize - 1), kFirstType);
  EXPECT_EQ(ByteAt(contents, kBlockSize + 6), kLastType);

  EXPECT_EQ(ReadAll(), (Records{first, "bar"}));
  EXPECT_EQ(reader_->LastRecordOffset(), kBlockSize - kHeaderSize)
      << "a record starts at its first fragment, even an empty one";
  ExpectNoReports();
}

TEST_F(LogTest, ShortTrailerIsPadded) {
  // One byte more and only six bytes are left, too few for a header: they are zeros, and the next
  // record starts the next block.
  const std::string first = BigString("foo", kBlockSize - 2 * kHeaderSize + 1);
  Write(first);
  Write("bar");
  const std::string contents = Contents();
  ASSERT_EQ(contents.size(), kBlockSize + kHeaderSize + 3);
  for (size_t i = kBlockSize - 6; i < kBlockSize; i++) {
    EXPECT_EQ(ByteAt(contents, i), 0) << "padding byte at offset " << i;
  }
  EXPECT_EQ(ByteAt(contents, kBlockSize + 6), kFullType);

  EXPECT_EQ(ReadAll(), (Records{first, "bar"}));
  EXPECT_EQ(reader_->LastRecordOffset(), kBlockSize);
  ExpectNoReports();
}

TEST_F(LogTest, LastRecordOffsetIsWhereTheRecordStarts) {
  std::vector<size_t> offsets;
  const Records records = {"foo", "bar", std::string(2 * kBlockSize, 'x'), "tail"};
  for (const std::string& record : records) {
    offsets.push_back(FileSize());
    Write(record);
  }
  StartReading();
  EXPECT_EQ(reader_->LastRecordOffset(), 0u);
  for (size_t i = 0; i < records.size(); i++) {
    ASSERT_EQ(Read(), records[i]);
    EXPECT_EQ(reader_->LastRecordOffset(), offsets[i]) << "record " << i;
  }
  ExpectNoReports();
}

TEST_F(LogTest, WriterFlushesOnceEveryRecord) {
  // Every record that AddRecord acknowledged has been handed to the operating system, or a crash
  // of the process would lose a write that the caller was told is safe from it.
  struct SpyFile : WritableFile {
    explicit SpyFile(WritableFile* inner) : inner_(inner) {}
    Status Append(std::string_view data) override {
      unflushed = unflushed || !data.empty();
      return inner_->Append(data);
    }
    Status Flush() override {
      flushes++;
      unflushed = false;
      return inner_->Flush();
    }
    Status Sync() override {
      syncs++;
      return inner_->Sync();
    }
    Status Close() override { return inner_->Close(); }

    WritableFile* inner_;
    int flushes = 0;
    int syncs = 0;
    bool unflushed = false;
  };

  SpyFile spy(dest_.get());
  Writer writer(&spy);
  const Records records = {"foo", "", std::string(3 * kBlockSize, 'x'),
                           std::string(kBlockSize - 2 * kHeaderSize + 1, 'y'), "bar"};
  int acknowledged = 0;
  for (const std::string& record : records) {
    ASSERT_OK(writer.AddRecord(record));
    acknowledged++;
    EXPECT_FALSE(spy.unflushed) << "record " << acknowledged << " was acknowledged unflushed";
    EXPECT_EQ(spy.flushes, acknowledged)
        << "a record is flushed once, however many blocks it spans";
  }
  EXPECT_EQ(spy.syncs, 0) << "making records durable is the caller's decision";
}

TEST_F(LogTest, BytesWrittenIsWhatTheFileHoldsAfterEveryRecord) {
  EXPECT_EQ(writer_->bytes_written(), 0u);
  const Records records = {"foo",
                           "",
                           std::string(3 * kBlockSize, 'x'),
                           BigString("foo", kBlockSize - 2 * kHeaderSize + 1),
                           "bar",
                           BigString("foo", kBlockSize - 2 * kHeaderSize),
                           "baz"};
  for (size_t i = 0; i < records.size(); i++) {
    Write(records[i]);
    EXPECT_EQ(writer_->bytes_written(), FileSize()) << "after record " << i;
  }
}

TEST_F(LogTest, BytesWrittenOfARecordIsItsPayloadAndItsHeaders) {
  const auto cost_of = [&](std::string_view record) {
    const uint64_t before = writer_->bytes_written();
    Write(record);
    return writer_->bytes_written() - before;
  };
  EXPECT_EQ(cost_of("foo"), kHeaderSize + 3);
  EXPECT_EQ(cost_of(""), kHeaderSize) << "an empty record is a header";
  // Four fragments, one header each: the record starts 17 bytes into a block and ends in a fourth.
  EXPECT_EQ(cost_of(std::string(3 * kBlockSize, 'x')), 3 * kBlockSize + 4 * kHeaderSize);
}

TEST_F(LogTest, TheZerosThatPadABlockAreCountedWithTheRecordThatFollows) {
  Write(BigString("foo", kBlockSize - 2 * kHeaderSize + 1));  // leaves six bytes of the block
  ASSERT_EQ(FileSize(), kBlockSize - 6);
  const uint64_t before = writer_->bytes_written();
  Write("bar");
  EXPECT_EQ(writer_->bytes_written() - before, 6 + kHeaderSize + 3);
  EXPECT_EQ(FileSize(), kBlockSize + kHeaderSize + 3);
}

TEST_F(LogTest, AnEmptyFragmentThatFillsTheEndOfABlockIsCounted) {
  Write(BigString("foo", kBlockSize - 2 * kHeaderSize));  // leaves exactly one header
  const uint64_t before = writer_->bytes_written();
  Write("bar");
  // The header with no payload that fills the block, and then "bar" in the next.
  EXPECT_EQ(writer_->bytes_written() - before, kHeaderSize + kHeaderSize + 3);
  EXPECT_EQ(writer_->bytes_written(), FileSize());
}

TEST_F(LogTest, AResumedWriterCountsOnlyWhatItWrites) {
  Write("foo");
  Resume();
  EXPECT_EQ(writer_->bytes_written(), 0u) << "what the file held before is not this writer's";
  Write("x");
  EXPECT_EQ(writer_->bytes_written(), kHeaderSize + 1);
  EXPECT_EQ(FileSize(), 2 * kHeaderSize + 4);
}

enum class FailOn { kAppend, kFlush };

// A WritableFile that works until `fail_on` is called for the first time, fails that call, and
// counts every call after it.
class FailingFile : public WritableFile {
 public:
  explicit FailingFile(FailOn fail_on) : fail_on_(fail_on) {}

  Status Append(std::string_view) override {
    if (failed_) {
      calls_after_failure++;
      return Status::IOError("append after failure");
    }
    if (fail_on_ == FailOn::kAppend) {
      failed_ = true;
      return Status::IOError("injected append failure");
    }
    return Status::OK();
  }
  Status Flush() override {
    if (failed_) {
      calls_after_failure++;
      return Status::IOError("flush after failure");
    }
    if (fail_on_ == FailOn::kFlush) {
      failed_ = true;
      return Status::IOError("injected flush failure");
    }
    return Status::OK();
  }
  Status Sync() override { return Status::OK(); }
  Status Close() override { return Status::OK(); }

  int calls_after_failure = 0;

 private:
  const FailOn fail_on_;
  bool failed_ = false;
};

TEST_F(LogTest, AFailedWriteIsStickyAndTouchesTheFileNoMore) {
  // After a failed Append or Flush the Env allows nothing but Close, and the block position the
  // Writer tracks no longer says where the next byte would land. The Writer must therefore refuse
  // every later record, with the first error, instead of passing it on to the file.
  for (const FailOn fail_on : {FailOn::kAppend, FailOn::kFlush}) {
    SCOPED_TRACE(fail_on == FailOn::kAppend ? "Append fails" : "Flush fails");
    FailingFile file(fail_on);
    Writer writer(&file);
    const Status first = writer.AddRecord("foo");
    ASSERT_TRUE(first.IsIOError()) << first.ToString();
    const Status second = writer.AddRecord("bar");
    EXPECT_TRUE(second.IsIOError());
    EXPECT_EQ(second.ToString(), first.ToString());
    EXPECT_EQ(file.calls_after_failure, 0) << "the Writer used the file after it failed";
  }
}

// A WritableFile that accepts `accepted` calls to Append() and fails the one after them, and every
// one after that.
class FailAfterAppends : public WritableFile {
 public:
  explicit FailAfterAppends(int accepted) : accepted_(accepted) {}

  Status Append(std::string_view) override {
    if (accepted_ == 0) {
      return Status::IOError("injected append failure");
    }
    accepted_--;
    return Status::OK();
  }
  Status Flush() override { return Status::OK(); }
  Status Sync() override { return Status::OK(); }
  Status Close() override { return Status::OK(); }

 private:
  int accepted_;
};

TEST(LogWriterBytesTest, ARecordThatFailsPartWayCountsWhatTheFileAccepted) {
  // "foo" is two appends, its header and its payload.
  {
    FailAfterAppends file(0);
    Writer writer(&file);
    EXPECT_TRUE(writer.AddRecord("foo").IsIOError());
    EXPECT_EQ(writer.bytes_written(), 0u) << "the header was refused";
  }
  {
    FailAfterAppends file(1);
    Writer writer(&file);
    EXPECT_TRUE(writer.AddRecord("foo").IsIOError());
    EXPECT_EQ(writer.bytes_written(), kHeaderSize) << "the header went in and the payload did not";
    EXPECT_TRUE(writer.AddRecord("bar").IsIOError());
    EXPECT_EQ(writer.bytes_written(), kHeaderSize) << "nothing is counted after the failure";
  }
  {
    FailAfterAppends file(2);
    Writer writer(&file);
    EXPECT_OK(writer.AddRecord("foo"));
    EXPECT_EQ(writer.bytes_written(), kHeaderSize + 3);
    EXPECT_TRUE(writer.AddRecord("bar").IsIOError());
    EXPECT_EQ(writer.bytes_written(), kHeaderSize + 3);
  }
}

TEST_F(LogTest, TornPayloadAtEofIsSilent) {
  Write("foo");
  Write("bar");
  Write("0123456789");
  Truncate(FileSize() - 1);
  EXPECT_EQ(ReadAll(), (Records{"foo", "bar"}));
  ExpectNoReports();
}

TEST_F(LogTest, TornHeaderAtEofIsSilent) {
  Write("foo");
  Write("bar");
  Write("0123456789");
  Truncate(FileSize() - 10 - kHeaderSize + 3);  // three bytes of the last header are left
  EXPECT_EQ(ReadAll(), (Records{"foo", "bar"}));
  ExpectNoReports();
}

TEST_F(LogTest, EndingAtAnyByteOfASmallLogIsSilent) {
  // Cut the file at every byte. Each cut must yield exactly the records that were whole before it,
  // and never a report: a torn tail is the normal result of a crash.
  const Records records = {"foo", "", "0123456789", "bar"};
  const std::string contents = WriteContents(records);
  std::vector<size_t> ends;  // where each record ends in the file
  size_t end = 0;
  for (const std::string& record : records) {
    end += kHeaderSize + record.size();
    ends.push_back(end);
  }
  ASSERT_EQ(ends.back(), contents.size());

  for (size_t cut = 0; cut <= contents.size(); cut++) {
    SCOPED_TRACE("file cut at byte " + std::to_string(cut));
    ReportCollector reports;
    const Records got = ReadContents(contents.substr(0, cut), &reports);
    const auto whole = static_cast<size_t>(std::count_if(
        ends.begin(), ends.end(), [&](size_t record_end) { return record_end <= cut; }));
    EXPECT_EQ(got, Records(records.begin(), records.begin() + static_cast<std::ptrdiff_t>(whole)));
    EXPECT_EQ(reports.messages, Records{});
  }
}

TEST_F(LogTest, EndingInsideAFragmentedRecordIsSilent) {
  // The writer may die between the fragments of one record. The reader sees a FIRST, perhaps
  // MIDDLEs, and then the end of the file, wherever the cut falls relative to the blocks.
  const std::string contents = WriteContents({"foo", std::string(3 * kBlockSize, 'x')});
  size_t cuts = 0;
  for (size_t cut = 11; cut < contents.size(); cut++) {
    const size_t in_block = cut % kBlockSize;
    const bool interesting =
        in_block <= kHeaderSize + 1 || in_block >= kBlockSize - 1 || cut + 1 == contents.size();
    if (!interesting) {
      continue;
    }
    cuts++;
    SCOPED_TRACE("file cut at byte " + std::to_string(cut));
    ReportCollector reports;
    EXPECT_EQ(ReadContents(contents.substr(0, cut), &reports), Records{"foo"});
    EXPECT_EQ(reports.messages, Records{});
  }
  EXPECT_GT(cuts, 20u) << "the sweep stopped covering the block boundaries";
}

TEST_F(LogTest, EndingInsideTheTrailerPaddingIsSilent) {
  const std::string first = BigString("foo", kBlockSize - 2 * kHeaderSize + 1);  // six bytes left
  Write(first);
  Write("bar");
  const std::string contents = Contents();
  for (const size_t cut : {kBlockSize - 6 + 3, kBlockSize}) {
    SCOPED_TRACE("file cut at byte " + std::to_string(cut));
    ReportCollector reports;
    EXPECT_EQ(ReadContents(contents.substr(0, cut), &reports), Records{first});
    EXPECT_EQ(reports.messages, Records{});
  }
}

TEST_F(LogTest, FragmentsWithoutAnEndAtEofAreSilent) {
  WriteRaw(Fragment(kFirstType, "a"));
  WriteRaw(Fragment(kMiddleType, "b"));
  EXPECT_EQ(ReadAll(), Records{});
  ExpectNoReports();
}

TEST_F(LogTest, ChecksumMismatchIsReportedAndBlockDropped) {
  // r1 and the first fragment of r2 share block 0; r2 ends in block 1 and r3 follows it there.
  const std::string r2 = BigString("x", kBlockSize);
  constexpr size_t kR2FirstPayload = kBlockSize - (kHeaderSize + 3) - kHeaderSize;
  constexpr size_t kR2LastPayload = kBlockSize - kR2FirstPayload;
  constexpr size_t kR2LastPhysical = kHeaderSize + kR2LastPayload;
  Write("foo");
  Write(r2);
  Write("baz");
  IncrementByte(kHeaderSize, 1);  // the first payload byte of r1

  EXPECT_EQ(ReadAll(), Records{"baz"});
  // The damaged record cannot be told apart from a damaged length, so the whole of block 0 goes,
  // r2's first fragment with it. Its last fragment, in block 1, then has no start.
  EXPECT_EQ(reports_.messages,
            (Records{Report("checksum mismatch"), Report("missing start of fragmented record")}));
  // Bytes are bytes of the log, headers included, for every kind of report.
  EXPECT_EQ(reports_.dropped, kBlockSize + kR2LastPhysical);
}

TEST_F(LogTest, ChecksumFieldDamageIsReported) {
  Write("foo");
  Write("bar");
  IncrementByte(0, 1);  // the first byte of the stored checksum
  EXPECT_EQ(ReadAll(), Records{});
  EXPECT_EQ(reports_.messages, Records{Report("checksum mismatch")});
  EXPECT_EQ(reports_.dropped, 2 * (kHeaderSize + 3));
}

TEST_F(LogTest, ChecksumOffIgnoresMismatch) {
  Write("foo");
  IncrementByte(kHeaderSize, 1);
  StartReading(/*checksum=*/false);
  EXPECT_EQ(ReadAll(), Records{"goo"});
  ExpectNoReports();
}

TEST_F(LogTest, BadRecordTypeIsReported) {
  Write("foo");
  SetByte(6, 9);
  EXPECT_EQ(ReadAll(), Records{});
  // The type is judged before the checksum, which no longer matches either: what is reported is
  // the first thing that is wrong with the header, not whatever the checksum makes of it.
  EXPECT_EQ(reports_.messages, Records{Report("unknown record type")});
  EXPECT_EQ(reports_.dropped, kHeaderSize + 3);
}

TEST_F(LogTest, TypesOutsideTheFormatAreUnknownWhetherOrNotChecksumsAreOn) {
  // Zero is not a record type either. The Writer pads with fewer than seven zero bytes, which are
  // no header, so a zero type is a damaged header or a file whose tail the filesystem zero-filled,
  // and neither is a clean cut.
  for (const bool checksum : {true, false}) {
    for (const int type : {0, kMaxRecordType + 1, 255}) {
      SCOPED_TRACE("type " + std::to_string(type) + ", checksum " + std::to_string(checksum));
      ReportCollector reports;
      std::string contents = Fragment(kFullType, "foo");
      contents[6] = static_cast<char>(type);
      EXPECT_EQ(ReadContents(contents, &reports, checksum), Records{});
      EXPECT_EQ(reports.messages, Records{Report("unknown record type")});
    }
  }
}

TEST_F(LogTest, BadLengthNotAtEofIsReported) {
  // Block 0 holds "foo" and a record that fills it to the last byte; "tail" is in block 1.
  Write("foo");
  Write(BigString("x", kBlockSize - (kHeaderSize + 3) - kHeaderSize));
  Write("tail");
  ASSERT_EQ(FileSize(), kBlockSize + kHeaderSize + 4);
  SetLengthField(0, kBlockSize - kHeaderSize + 100);  // 100 bytes beyond the end of the block

  EXPECT_EQ(ReadAll(), Records{"tail"});
  EXPECT_EQ(reports_.messages, Records{Report("bad record length")});
  EXPECT_EQ(reports_.dropped, kBlockSize);
}

TEST_F(LogTest, LengthPastTheBlockIsReportedEvenInTheLastBlock) {
  // This block is also the end of the file, but no record can leave its block, so a length that
  // does is a defect and not a record that the end of the file cut short.
  Write("foo");
  SetLengthField(0, 40000);
  EXPECT_EQ(ReadAll(), Records{});
  EXPECT_EQ(reports_.messages, Records{Report("bad record length")});
  EXPECT_EQ(reports_.dropped, kHeaderSize + 3);
}

TEST_F(LogTest, LengthPastTheEndOfTheFileOfARecordThatIsWholeIsReported) {
  // The record is all there, and its length says it is a byte longer than the file. That is what a
  // record that a crash cut short looks like, until it is seen that the bytes the file does have
  // are a whole record with the stored checksum: a crash leaves a prefix of a record, which does
  // not have it, and a record whose length was damaged does. So it is reported, and not taken for
  // the end of the file.
  Write("foo");
  Write("bar");
  SetLengthField(kHeaderSize + 3, 4);  // one byte more than the file holds
  EXPECT_EQ(ReadAll(), Records{"foo"});
  EXPECT_EQ(reports_.messages, Records{Report("bad record length")});
  EXPECT_EQ(reports_.dropped, kHeaderSize + 3) << "what is left of the block, which is the record";
}

TEST_F(LogTest, LengthPastTheEndOfTheFileOfARecordThatIsCutShortIsStillATornTail) {
  // The same length, and the file really does end inside the record: the checksum matches no length
  // of the bytes that are there, and this is the end that a crash leaves.
  Write("foo");
  Write("bar!");
  Truncate(FileSize() - 1);  // "bar", under a header that says four bytes
  EXPECT_EQ(ReadAll(), Records{"foo"});
  ExpectNoReports();
}

// The records of `contents`, a log, as the offsets in it at which each starts.
std::vector<size_t> RecordOffsets(const std::string& contents) {
  const std::unique_ptr<Env> env = NewMemEnv();
  EXPECT_OK(WriteStringToFile(env.get(), contents, "/offsets", /*sync=*/false));
  std::unique_ptr<SequentialFile> source;
  EXPECT_OK(env->NewSequentialFile("/offsets", &source));
  ReportCollector reports;
  Reader reader(source.get(), &reports, /*checksum=*/true);
  std::vector<size_t> offsets;
  std::string scratch;
  std::string_view record;
  while (reader.ReadRecord(&record, &scratch)) {
    offsets.push_back(static_cast<size_t>(reader.LastRecordOffset()));
  }
  EXPECT_EQ(reports.messages, Records{});
  return offsets;
}

// Flips each of the 16 bits of the length field of each record of `records` that starts at or
// after byte `from`, one at a time, in the log that holds them, and checks that the damage is
// reported whichever way the length went, that the records before the damaged one are all that is
// returned, since a defect drops the rest of its block, and that the report is one of the two that
// a damaged length can be.
void ExpectEveryDamagedLengthIsReported(const Records& records, size_t from) {
  const std::string intact = WriteContents(records);
  const std::vector<size_t> offsets = RecordOffsets(intact);
  ASSERT_EQ(offsets.size(), records.size());
  size_t flips = 0;
  for (size_t i = 0; i < records.size(); i++) {
    if (offsets[i] < from) {
      continue;
    }
    for (size_t bit = 0; bit < 16; bit++) {
      SCOPED_TRACE("bit " + std::to_string(bit) + " of the length of record " + std::to_string(i));
      std::string damaged = intact;
      damaged[offsets[i] + 4 + bit / 8] =
          static_cast<char>(ByteAt(damaged, offsets[i] + 4 + bit / 8) ^ (1u << (bit % 8)));
      ReportCollector reports;
      const Records got = ReadContents(damaged, &reports);
      ASSERT_FALSE(reports.messages.empty()) << "the damage went unreported";
      for (const std::string& message : reports.messages) {
        EXPECT_TRUE(message == Report("bad record length") ||
                    message == Report("checksum mismatch"))
            << message;
      }
      EXPECT_EQ(got, Records(records.begin(), records.begin() + static_cast<std::ptrdiff_t>(i)));
      flips++;
    }
  }
  EXPECT_GT(flips, 0u);
}

TEST_F(LogTest, EveryBitOfTheLengthOfTheLastRecordIsReported) {
  // Which way the flipped bit moves the length does not matter: shorter, the record has the wrong
  // checksum; longer and still in the file, the same; longer than the file and inside the block,
  // the bytes that are there are a whole record; longer than the block, it is a bad length.
  ExpectEveryDamagedLengthIsReported({"foo", "bar", "0123456789"}, 0);
}

TEST_F(LogTest, EveryBitOfTheLengthOfAnyRecordIsReported) {
  // A record in the middle of the file has what follows it inside the length a flipped bit makes
  // too long, and the checksum of what it really was is still found in the bytes that are there.
  // The records are those of a MANIFEST: a snapshot of a state, then edits, of every size.
  ExpectEveryDamagedLengthIsReported({std::string(120, 'a'), "", std::string(700, 'b'),
                                      std::string(33, 'c'), "d", std::string(250, 'e')},
                                     0);
}

TEST_F(LogTest, EveryBitOfTheLengthOfAnyRecordOfTheLastBlockIsReported) {
  // The first record is as long as a block and a bit, so the last block starts with the end of it
  // and holds the records after it: where the file is no longer a whole number of blocks, and a
  // length can run past its end.
  ExpectEveryDamagedLengthIsReported(
      {std::string(kBlockSize + 100, 'x'), std::string(90, 'y'), "", "z", std::string(300, 'w')},
      kBlockSize);
}

TEST_F(LogTest, ADamagedLengthPastTheEndOfTheFileIsNotCaughtWithoutChecksums) {
  // With the checksums off there is nothing to find the record by, and the rest of the file is read
  // as the end of it. That is what the checksum switch means.
  const std::string intact = WriteContents({"foo", "bar"});
  std::string damaged = intact;
  damaged[kHeaderSize + 3 + 4] = 4;
  ReportCollector reports;
  EXPECT_EQ(ReadContents(damaged, &reports, /*checksum=*/false), Records{"foo"});
  EXPECT_EQ(reports.messages, Records{});
}

TEST_F(LogTest, EveryStrictCutOfARandomLogIsSilent) {
  // The other side of the search for a record that is whole: a file that ends inside a record never
  // has the checksum of any length of what it holds, short of a collision of 32 bits, which no
  // cut of these logs is. So whatever the sizes, cutting the file anywhere returns the records that
  // were whole and reports nothing.
  for (uint64_t seed = 0; seed < 4; seed++) {
    SCOPED_TRACE("seed " + std::to_string(seed));
    Random rnd(seed);
    Records written;
    for (int i = 0; i < 12; i++) {
      written.push_back(std::to_string(i) + ":" + RandomBinaryString(&rnd, rnd.Skewed(9)));
    }
    const std::string contents = WriteContents(written);
    const std::vector<size_t> offsets = RecordOffsets(contents);
    for (size_t cut = 0; cut <= contents.size(); cut++) {
      ReportCollector reports;
      const Records got = ReadContents(contents.substr(0, cut), &reports);
      size_t whole = 0;
      while (whole + 1 < written.size() && offsets[whole + 1] <= cut) {
        whole++;
      }
      // Record `whole` is whole if the file holds all of it: up to where the next starts, or to the
      // end.
      const size_t end = whole + 1 < written.size() ? offsets[whole + 1] : contents.size();
      const size_t count = cut >= end ? whole + 1 : whole;
      ASSERT_EQ(got, Records(written.begin(), written.begin() + static_cast<std::ptrdiff_t>(count)))
          << "cut at byte " << cut;
      ASSERT_EQ(reports.messages, Records{}) << "cut at byte " << cut;
    }
  }
}

TEST_F(LogTest, MiddleWithoutFirstIsReported) {
  WriteRaw(Fragment(kMiddleType, "bar"));
  Write("foo");
  EXPECT_EQ(ReadAll(), Records{"foo"});
  EXPECT_EQ(reports_.messages, Records{Report("missing start of fragmented record")});
  EXPECT_EQ(reports_.dropped, kHeaderSize + 3);
}

TEST_F(LogTest, LastWithoutFirstIsReported) {
  WriteRaw(Fragment(kLastType, "bar"));
  Write("foo");
  EXPECT_EQ(ReadAll(), Records{"foo"});
  EXPECT_EQ(reports_.messages, Records{Report("missing start of fragmented record")});
  EXPECT_EQ(reports_.dropped, kHeaderSize + 3);
}

TEST_F(LogTest, FirstThenFullIsReported) {
  WriteRaw(Fragment(kFirstType, "partial"));
  WriteRaw(Fragment(kFullType, "bar"));
  EXPECT_EQ(ReadAll(), Records{"bar"});
  EXPECT_EQ(reports_.messages, Records{Report("partial record without end")});
  EXPECT_EQ(reports_.dropped, kHeaderSize + 7);
}

TEST_F(LogTest, FirstThenFirstIsReportedAndTheSecondIsKept) {
  WriteRaw(Fragment(kFirstType, "one"));
  WriteRaw(Fragment(kFirstType, "two"));
  WriteRaw(Fragment(kLastType, "three"));
  EXPECT_EQ(ReadAll(), Records{"twothree"});
  EXPECT_EQ(reports_.messages, Records{Report("partial record without end")});
  EXPECT_EQ(reports_.dropped, kHeaderSize + 3);
}

TEST_F(LogTest, DamageInsideAFragmentedRecordLosesTheWholeRecord) {
  // FIRST in block 0, MIDDLE in block 1, LAST in block 2, then "tail". Damage the MIDDLE: the
  // FIRST that was already read is gone with it, and the LAST that follows has no start. None of
  // the three may be glued to anything.
  constexpr size_t kLastPayload = 3 * kBlockSize - 100 - 2 * (kBlockSize - kHeaderSize);
  Write(std::string(3 * kBlockSize - 100, 'x'));
  Write("tail");
  IncrementByte(kBlockSize + kHeaderSize, 1);

  EXPECT_EQ(ReadAll(), Records{"tail"});
  EXPECT_EQ(reports_.messages,
            (Records{Report("checksum mismatch"), Report("partial record without end"),
                     Report("missing start of fragmented record")}));
  // The MIDDLE's block, the whole block that the FIRST filled, and the LAST with its header.
  EXPECT_EQ(reports_.dropped, kBlockSize + kBlockSize + (kHeaderSize + kLastPayload));
}

TEST_F(LogTest, ADefectSwallowsTheRestOfItsBlockIncludingIntactRecords) {
  // After a defect the reader cannot trust a length, so it drops the rest of the block. Intact
  // records that were acknowledged can be in it. Hence a report is never "the log ends here",
  // wherever it falls, and a caller must not take a report with nothing valid after it for a torn
  // tail: that would accept the loss of records that were fine.
  Write("foo");
  Write("bar");
  Write("baz");
  Write(BigString("x", kBlockSize - 3 * (kHeaderSize + 3) - kHeaderSize));  // fills block 0
  Write("later");
  ASSERT_EQ(FileSize(), kBlockSize + kHeaderSize + 5);
  IncrementByte(kHeaderSize, 1);  // damage "foo" and nothing else

  EXPECT_EQ(ReadAll(), Records{"later"}) << "bar, baz and the filler were intact";
  EXPECT_EQ(reports_.messages, Records{Report("checksum mismatch")});
  EXPECT_EQ(reports_.dropped, kBlockSize) << "one report, for the whole block";

  // The same damage in the last block: no valid record follows the report, and two intact ones
  // were lost with it.
  std::string contents = WriteContents({"foo", "bar", "baz"});
  contents[kHeaderSize] = static_cast<char>(contents[kHeaderSize] + 1);
  ReportCollector reports;
  EXPECT_EQ(ReadContents(contents, &reports), Records{});
  EXPECT_EQ(reports.messages, Records{Report("checksum mismatch")});
  EXPECT_EQ(reports.dropped, contents.size());
}

// --- A zero-filled tail (D6) ---------------------------------------------------------------------
//
// A crash can leave a file longer than what reached it: the filesystem kept the new length and not
// the data, and the end reads back as zeros. Where the zeros start at a record boundary and every
// byte from there to the end of the file is zero, no record can come after them, so the Reader
// takes them for the end of the log, as it does a record cut short. Zeros followed by anything
// else, and zeros that begin inside a record, are what they were: defects.

TEST_F(LogTest, ZerosFromARecordBoundaryToTheEndOfTheFileAreTheEndOfTheLog) {
  const std::string valid = WriteContents({"foo", "bar"});
  // A header's worth, more, and to the last byte of the first block.
  for (const size_t zeros : {kHeaderSize, size_t{100}, kBlockSize - valid.size()}) {
    for (const bool checksum : {true, false}) {
      SCOPED_TRACE(std::to_string(zeros) + " zeros, checksum " + std::to_string(checksum));
      ReportCollector reports;
      uint64_t zero_tail = 0;
      EXPECT_EQ(ReadContents(valid + std::string(zeros, '\0'), &reports, checksum, &zero_tail),
                (Records{"foo", "bar"}));
      EXPECT_EQ(reports.messages, Records{});
      EXPECT_EQ(reports.dropped, 0u);
      EXPECT_EQ(zero_tail, zeros) << "the Reader says how many zero bytes it dropped";
    }
  }
}

TEST_F(LogTest, ZerosToTheEndOfTheFileAcrossBlockBoundariesAreTheEndOfTheLog) {
  {  // From inside the first block to the end of the third, and on into a fourth.
    const std::string valid = WriteContents({"foo", "bar"});
    for (const size_t end : {3 * kBlockSize, 3 * kBlockSize + 100}) {
      SCOPED_TRACE("zeros to offset " + std::to_string(end));
      ReportCollector reports;
      uint64_t zero_tail = 0;
      EXPECT_EQ(ReadContents(valid + std::string(end - valid.size(), '\0'), &reports,
                             /*checksum=*/true, &zero_tail),
                (Records{"foo", "bar"}));
      EXPECT_EQ(reports.messages, Records{});
      EXPECT_EQ(zero_tail, end - valid.size());
    }
  }
  {  // From a block boundary: the record before the zeros fills its block to the last byte.
    const std::string record = BigString("x", kBlockSize - kHeaderSize);
    const std::string full = WriteContents({record});
    ASSERT_EQ(full.size(), kBlockSize);
    ReportCollector reports;
    uint64_t zero_tail = 0;
    EXPECT_EQ(ReadContents(full + std::string(2 * kBlockSize, '\0'), &reports, true, &zero_tail),
              Records{record});
    EXPECT_EQ(reports.messages, Records{});
    EXPECT_EQ(zero_tail, 2 * kBlockSize);
  }
  {  // Where the next fragment of a record would start: the half-read record is dropped without a
     // word, as it is when the file ends there.
    const std::string spanning = WriteContents({"foo", BigString("y", kBlockSize)});
    ASSERT_GT(spanning.size(), kBlockSize);
    ReportCollector reports;
    uint64_t zero_tail = 0;
    EXPECT_EQ(ReadContents(spanning.substr(0, kBlockSize) + std::string(kBlockSize + 10, '\0'),
                           &reports, true, &zero_tail),
              Records{"foo"});
    EXPECT_EQ(reports.messages, Records{});
    EXPECT_EQ(zero_tail, kBlockSize + 10);
  }
}

TEST_F(LogTest, AFileOfNothingButZerosIsAnEmptyLog) {
  for (const size_t size : {kHeaderSize, size_t{100}, kBlockSize, 2 * kBlockSize + 5}) {
    SCOPED_TRACE(std::to_string(size) + " zeros");
    ReportCollector reports;
    uint64_t zero_tail = 0;
    EXPECT_EQ(ReadContents(std::string(size, '\0'), &reports, true, &zero_tail), Records{});
    EXPECT_EQ(reports.messages, Records{});
    EXPECT_EQ(zero_tail, size);
  }
  {  // Fewer than a header's worth is a header cut off by the end of the file, silent as before.
    ReportCollector reports;
    uint64_t zero_tail = 99;
    EXPECT_EQ(ReadContents(std::string(kHeaderSize - 1, '\0'), &reports, true, &zero_tail),
              Records{});
    EXPECT_EQ(reports.messages, Records{});
    EXPECT_EQ(zero_tail, 0u) << "no header, so no zeros where a header would be";
  }
}

TEST_F(LogTest, AnIntactOrTornLogHasNoZeroTail) {
  const std::string valid = WriteContents({"foo", "bar"});
  for (const size_t size : {valid.size(), valid.size() - 2, size_t{3}}) {
    SCOPED_TRACE(size);
    ReportCollector reports;
    uint64_t zero_tail = 99;
    ReadContents(valid.substr(0, size), &reports, true, &zero_tail);
    EXPECT_EQ(zero_tail, 0u);
  }
}

TEST_F(LogTest, ZerosFollowedByAnyOtherByteAreStillADefect) {
  // A byte that is not zero may be where a record after the zeros starts, so the zeros are not the
  // end of the log. What is reported is exactly what any zero header gets: one report for each
  // block the zeros reach into, and nothing taken for the end of the file.
  const std::string valid = WriteContents({"foo", "bar"});
  struct Case {
    size_t end;     // the size of the file; its last byte is the one that is not zero
    size_t blocks;  // the blocks that the zeros reach into, each reported
  };
  for (const Case c : {Case{100, 1}, Case{kBlockSize, 1}, Case{3 * kBlockSize, 3},
                       Case{3 * kBlockSize + 100, 4}}) {
    SCOPED_TRACE("a byte that is not zero at offset " + std::to_string(c.end - 1));
    std::string contents = valid + std::string(c.end - valid.size(), '\0');
    contents.back() = '\x01';
    ReportCollector reports;
    uint64_t zero_tail = 99;
    EXPECT_EQ(ReadContents(contents, &reports, true, &zero_tail), (Records{"foo", "bar"}));
    EXPECT_EQ(reports.messages, Records(c.blocks, Report("unknown record type")));
    EXPECT_EQ(reports.dropped, c.end - valid.size());
    EXPECT_EQ(zero_tail, 0u);
  }
  {  // A record after the zeros, at the start of a later block, is read: nothing after the zeros is
     // swallowed beyond the blocks that they fill.
    const std::string contents =
        valid + std::string(3 * kBlockSize - valid.size(), '\0') + Fragment(kFullType, "later");
    ReportCollector reports;
    uint64_t zero_tail = 99;
    EXPECT_EQ(ReadContents(contents, &reports, true, &zero_tail), (Records{"foo", "bar", "later"}));
    EXPECT_EQ(reports.messages, Records(3, Report("unknown record type")));
    EXPECT_EQ(reports.dropped, 3 * kBlockSize - valid.size());
    EXPECT_EQ(zero_tail, 0u);
  }
  {  // After a FIRST fragment: the record that the zeros interrupt is reported as abandoned, after
     // the first block of zeros and before the next, as with any defect.
    const std::string spanning = WriteContents({"foo", BigString("y", kBlockSize)});
    std::string contents = spanning.substr(0, kBlockSize) + std::string(2 * kBlockSize, '\0');
    contents.back() = '\x01';
    ReportCollector reports;
    uint64_t zero_tail = 99;
    EXPECT_EQ(ReadContents(contents, &reports, true, &zero_tail), Records{"foo"});
    EXPECT_EQ(reports.messages,
              (Records{Report("unknown record type"), Report("partial record without end"),
                       Report("unknown record type")}));
    EXPECT_EQ(zero_tail, 0u);
  }
}

TEST_F(LogTest, ZerosWithOneOtherByteAnywhereAreNeverTakenForTheEnd) {
  // Wherever the one byte that is not zero falls, in the first header, at the end of a block, in a
  // later block or at the very end, the zeros are reported and the zero tail is empty.
  const std::string valid = WriteContents({"foo", "bar"});
  Random rnd(301);
  for (int i = 0; i < 300; i++) {
    const size_t zeros = 1 + rnd.Uniform(3 * kBlockSize);
    const size_t at = rnd.Uniform(static_cast<uint32_t>(zeros));
    SCOPED_TRACE(std::to_string(zeros) + " zeros, a byte that is not zero " + std::to_string(at) +
                 " bytes into them");
    std::string contents = valid + std::string(zeros, '\0');
    contents[valid.size() + at] = static_cast<char>(1 + rnd.Uniform(255));
    ReportCollector reports;
    uint64_t zero_tail = 99;
    const Records records = ReadContents(contents, &reports, true, &zero_tail);
    EXPECT_EQ(zero_tail, 0u);
    ASSERT_GE(records.size(), 2u);
    EXPECT_EQ(records[0], "foo");
    EXPECT_EQ(records[1], "bar");
    if (zeros >= kHeaderSize) {
      // Fewer bytes than a header are a header that the end of the file cut off, which is silent.
      EXPECT_FALSE(reports.messages.empty());
    }
  }
}

TEST_F(LogTest, AGarbageTailIsStillADefect) {
  // Bytes after the last record that are not all zeros are reported as they were.
  const std::string valid = WriteContents({"foo", "bar"});
  for (const std::string& tail : {std::string(100, '\xff'), std::string(100, 'x'),
                                  std::string(4, '\xab') + std::string(96, '\0')}) {
    ReportCollector reports;
    uint64_t zero_tail = 99;
    EXPECT_EQ(ReadContents(valid + tail, &reports, true, &zero_tail), (Records{"foo", "bar"}));
    EXPECT_EQ(reports.messages, Records{Report("unknown record type")});
    EXPECT_EQ(zero_tail, 0u);
  }
}

TEST_F(LogTest, ZerosThatBeginInsideARecordAreStillADefect) {
  // The rule is for zeros from a record boundary. Zeros that begin inside a record leave a header,
  // or a part of one, that is not zero: a damaged record, reported as it was.
  const std::string valid = WriteContents({"foo", "bar", "baz"});
  ASSERT_EQ(valid.size(), 3 * (kHeaderSize + 3));
  const size_t baz = 2 * (kHeaderSize + 3);
  {  // The last record's header is whole and its payload is zeros, as are the bytes after it.
    ReportCollector reports;
    uint64_t zero_tail = 99;
    const std::string contents = valid.substr(0, baz + kHeaderSize) + std::string(100, '\0');
    EXPECT_EQ(ReadContents(contents, &reports, true, &zero_tail), (Records{"foo", "bar"}));
    EXPECT_EQ(reports.messages, Records{Report("checksum mismatch")});
    EXPECT_EQ(zero_tail, 0u);
  }
  {  // The zeros begin inside the last record's header, after its checksum.
    ReportCollector reports;
    uint64_t zero_tail = 99;
    const std::string contents = valid.substr(0, baz + 4) + std::string(100, '\0');
    EXPECT_EQ(ReadContents(contents, &reports, true, &zero_tail), (Records{"foo", "bar"}));
    EXPECT_EQ(reports.messages, Records{Report("unknown record type")});
    EXPECT_EQ(zero_tail, 0u);
  }
}

TEST_F(LogTest, AReadErrorPastTheZerosIsNotTakenForTheirEnd) {
  // To know that the zeros run to the end of the file, the Reader reads on to it. A read that fails
  // on the way says nothing about the rest of the file: the zeros before it are reported as they
  // would be without the failure, and so is the failure.
  const std::string valid = WriteContents({"foo", "bar"});
  const std::string contents = valid + std::string(3 * kBlockSize - valid.size(), '\0');
  ReportCollector reports;
  FailingSource source(contents, /*good_reads=*/2);  // blocks 0 and 1, then the read of 2 fails
  Reader reader(&source, &reports, true);
  std::string scratch;
  std::string_view record;
  ASSERT_TRUE(reader.ReadRecord(&record, &scratch));
  EXPECT_EQ(record, "foo");
  ASSERT_TRUE(reader.ReadRecord(&record, &scratch));
  EXPECT_EQ(record, "bar");
  EXPECT_FALSE(reader.ReadRecord(&record, &scratch));
  ASSERT_EQ(reports.messages.size(), 3u);
  EXPECT_EQ(reports.messages[0], Report("unknown record type"));
  EXPECT_EQ(reports.messages[1], Report("unknown record type"));
  EXPECT_NE(reports.messages[2].find("IO error"), std::string::npos) << reports.messages[2];
  EXPECT_EQ(reader.ZeroTailBytes(), 0u);
}

TEST_F(LogTest, ReadErrorIsReportedAndEndsTheLog) {
  // A failed read is neither a clean end of file nor a damaged record, and must not look like the
  // first: recovery would then take a log it could not read for one that simply ended.
  const std::string contents = WriteContents({"foo", std::string(2 * kBlockSize, 'x')});

  {  // The very first read fails.
    ReportCollector reports;
    FailingSource source(contents, 0);
    Reader reader(&source, &reports, true);
    std::string scratch;
    std::string_view record;
    EXPECT_FALSE(reader.ReadRecord(&record, &scratch));
    EXPECT_FALSE(reader.ReadRecord(&record, &scratch));
    ASSERT_EQ(reports.messages.size(), 1u);
    EXPECT_NE(reports.messages[0].find("IO error"), std::string::npos) << reports.messages[0];
  }
  {  // The second block cannot be read: "foo" was delivered, the record that needs it is lost.
    ReportCollector reports;
    FailingSource source(contents, 1);
    Reader reader(&source, &reports, true);
    std::string scratch;
    std::string_view record;
    ASSERT_TRUE(reader.ReadRecord(&record, &scratch));
    EXPECT_EQ(record, "foo");
    EXPECT_FALSE(reader.ReadRecord(&record, &scratch));
    ASSERT_EQ(reports.messages.size(), 1u);
    EXPECT_NE(reports.messages[0].find("IO error"), std::string::npos) << reports.messages[0];
  }
}

TEST_F(LogTest, ReaderWithoutAReporterStillSkipsDamage) {
  Write("foo");
  Write(BigString("x", kBlockSize));
  Write("baz");
  IncrementByte(kHeaderSize, 1);
  ASSERT_OK(env_->NewSequentialFile(path_, &source_));
  Reader reader(source_.get(), /*reporter=*/nullptr, /*checksum=*/true);
  std::string scratch;
  std::string_view record;
  ASSERT_TRUE(reader.ReadRecord(&record, &scratch));
  EXPECT_EQ(record, "baz");
  EXPECT_FALSE(reader.ReadRecord(&record, &scratch));
}

TEST_F(LogTest, ResumedWriterKeepsBlockAlignment) {
  Write("foo");
  Write("bar");
  const std::string spanning = BigString("x", kBlockSize + 100);
  Write(spanning);
  Resume();
  const std::string spanning_again = BigString("y", kBlockSize);
  Write("baz");
  Write(spanning_again);
  Write("qux");
  EXPECT_EQ(ReadAll(), (Records{"foo", "bar", spanning, "baz", spanning_again, "qux"}));
  ExpectNoReports();
}

TEST_F(LogTest, ResumingAfterATornTailLosesTheNewRecordsToo) {
  // The precondition of Writer(dest, dest_length): the file ends on a record boundary. Here it
  // ends inside a record, whose header goes on claiming the first byte of what is appended, so the
  // read reports a defect and drops the rest of the block, the new record with it.
  Write("foo");
  Write("0123456789");
  Truncate(FileSize() - 1);
  Resume();
  Write("bar");
  EXPECT_EQ(ReadAll(), Records{"foo"});
  EXPECT_EQ(reports_.messages, Records{Report("checksum mismatch")});
}

// The file that the second Writer finds ends `GetParam()` bytes before a block boundary. Seven or
// more is room for a header; fewer is padding; none is a fresh block.
class ResumeTest : public LogTest, public testing::WithParamInterface<size_t> {};

TEST_P(ResumeTest, ResumedWriterHandlesTheTailItFinds) {
  const size_t leftover = GetParam();
  const std::string first = BigString("a", kBlockSize - leftover - kHeaderSize);
  Write(first);
  ASSERT_EQ(FileSize(), kBlockSize - leftover);
  Resume();
  Write("b");
  Write("c");
  const std::string contents = Contents();
  for (size_t i = kBlockSize - leftover; leftover < kHeaderSize && i < kBlockSize; i++) {
    ASSERT_EQ(ByteAt(contents, i), 0) << "padding byte at offset " << i;
  }
  EXPECT_EQ(ReadAll(), (Records{first, "b", "c"}));
  ExpectNoReports();
}

INSTANTIATE_TEST_SUITE_P(TailLengths, ResumeTest,
                         testing::Values(size_t{0}, size_t{1}, size_t{3}, size_t{6}, size_t{7},
                                         size_t{8}, size_t{100}),
                         [](const testing::TestParamInfo<size_t>& param) {
                           return "Leftover" + std::to_string(param.param);
                         });

TEST_F(LogTest, RandomCorruptionNeverYieldsUnwrittenRecords) {
  // Flip a few bits anywhere in a log of random records. Every payload starts with its sequence
  // number, so no two are equal and a record that came back twice, or out of order, or fused from
  // two others, cannot pass for a later one. What comes back must be some of the records that were
  // written, in the order they were written, and nothing else.
  //
  // And the property recovery relies on: if nothing was reported, what was read is a prefix of
  // what was written. It does not follow that nothing was lost. The one place that damage could
  // hide is a length field in the last block that points past the end of the file but inside the
  // block, which looks like a torn tail; it is found when the record is whole, by its checksum, so
  // what is left of it is a length damaged together with the bytes that the checksum would be
  // found in. Those seeds are counted on their own, and each must have a flipped bit in the last
  // block.
  size_t seeds_that_reported = 0;
  size_t seeds_that_lost_records = 0;
  size_t silent_short_reads = 0;
  constexpr uint64_t kSeeds = 1000;
  for (uint64_t seed = 0; seed < kSeeds; seed++) {
    SCOPED_TRACE("seed " + std::to_string(seed));
    Random rnd(seed);
    Records written;
    std::map<std::string, size_t> position;
    for (size_t i = 0; i < 50; i++) {
      written.push_back(std::to_string(i) + ":" + RandomBinaryString(&rnd, rnd.Skewed(15)));
      position[written.back()] = i;
    }
    std::string contents = WriteContents(written);
    const size_t last_block = (contents.size() - 1) / kBlockSize;
    bool flipped_in_last_block = false;
    const uint32_t flips = 1 + rnd.Uniform(5);
    for (uint32_t i = 0; i < flips; i++) {
      const uint32_t bit = rnd.Uniform(static_cast<uint32_t>(contents.size() * 8));
      contents[bit / 8] = static_cast<char>(ByteAt(contents, bit / 8) ^ (1u << (bit % 8)));
      flipped_in_last_block = flipped_in_last_block || bit / 8 / kBlockSize == last_block;
    }

    ReportCollector reports;
    const Records got = ReadContents(contents, &reports);

    size_t next_allowed = 0;
    for (const std::string& record : got) {
      const auto found = position.find(record);
      ASSERT_NE(found, position.end()) << "a record of " << record.size() << " bytes was returned "
                                       << "that was never written";
      ASSERT_GE(found->second, next_allowed)
          << "record " << found->second << " came back twice or out of order";
      next_allowed = found->second + 1;
    }

    if (!reports.messages.empty()) {
      seeds_that_reported++;
    } else {
      ASSERT_LE(got.size(), written.size());
      EXPECT_TRUE(std::equal(got.begin(), got.end(), written.begin()))
          << "nothing was reported, yet what was read is not a prefix of what was written";
      if (got.size() < written.size()) {
        silent_short_reads++;
        EXPECT_TRUE(flipped_in_last_block)
            << "records were lost without a report and without damage in the last block";
      }
    }
    if (got.size() < written.size()) {
      seeds_that_lost_records++;
    }
  }
  // Damage that nothing noticed would make the checks above vacuous.
  EXPECT_GE(seeds_that_reported, kSeeds * 95 / 100);
  EXPECT_GE(seeds_that_lost_records, kSeeds * 99 / 100);
  EXPECT_LE(silent_short_reads, kSeeds / 100) << "silent loss is the rare, documented case";
}

TEST_F(LogTest, RandomDamageToALengthIsNeverSilent) {
  // One bit of the length of one record, whatever the records are and wherever the record falls,
  // in a log of up to a few blocks: the damage is reported, and what comes back is some of the
  // records, in order, and not the one that was damaged.
  for (uint64_t seed = 0; seed < 400; seed++) {
    SCOPED_TRACE("seed " + std::to_string(seed));
    Random rnd(seed);
    Records written;
    std::map<std::string, size_t> position;
    for (size_t i = 0; i < 15; i++) {
      written.push_back(std::to_string(i) + ":" + RandomBinaryString(&rnd, rnd.Skewed(13)));
      position[written.back()] = i;
    }
    std::string contents = WriteContents(written);
    const std::vector<size_t> offsets = RecordOffsets(contents);
    const size_t victim = rnd.Uniform(static_cast<uint32_t>(written.size()));
    const uint32_t bit = rnd.Uniform(16);
    contents[offsets[victim] + 4 + bit / 8] =
        static_cast<char>(ByteAt(contents, offsets[victim] + 4 + bit / 8) ^ (1u << (bit % 8)));

    ReportCollector reports;
    const Records got = ReadContents(contents, &reports);
    ASSERT_FALSE(reports.messages.empty()) << "bit " << bit << " of record " << victim;
    size_t next_allowed = 0;
    for (const std::string& record : got) {
      const auto found = position.find(record);
      ASSERT_NE(found, position.end()) << "a record that was never written came back";
      ASSERT_GE(found->second, next_allowed) << "a record came back twice or out of order";
      ASSERT_NE(found->second, victim) << "the record with the damaged length came back";
      next_allowed = found->second + 1;
    }
  }
}

TEST_F(LogTest, RandomCorruptionWithoutChecksumsStillTerminates) {
  // With checksums off the content of a record is not vouched for, but the structure still is: the
  // reader must neither read outside a block nor stop making progress. The sanitizers watch the
  // first, and a hang watches the second.
  for (uint64_t seed = 0; seed < 300; seed++) {
    SCOPED_TRACE("seed " + std::to_string(seed));
    Random rnd(seed);
    Records written;
    for (int i = 0; i < 20; i++) {
      written.push_back(RandomBinaryString(&rnd, rnd.Skewed(15)));
    }
    std::string contents = WriteContents(written);
    const uint32_t flips = 1 + rnd.Uniform(20);
    for (uint32_t i = 0; i < flips; i++) {
      const uint32_t bit = rnd.Uniform(static_cast<uint32_t>(contents.size() * 8));
      contents[bit / 8] = static_cast<char>(ByteAt(contents, bit / 8) ^ (1u << (bit % 8)));
    }
    ReportCollector reports;
    const Records got = ReadContents(contents, &reports, /*checksum=*/false);
    EXPECT_LE(got.size(), contents.size() / kHeaderSize + 1);
  }
}

}  // namespace
}  // namespace shale::log
