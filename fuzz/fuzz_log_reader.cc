// Feeds the input to the write-ahead log reader as the bytes of a log file, and reads every record
// out of it, once with the checksums checked and once without. Without them the reader's handling
// of the structure of the log is reached too: a fuzzer cannot compute a CRC32C, so with them on
// nearly every mutation of a record ends at "checksum mismatch".
//
// The reader reads a log that a crash or a bad disk may have damaged, so whatever the bytes are it
// has to end, return only what the file can hold, and report defects as Corruption.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "fuzz_util.h"
#include "io/log_format.h"
#include "io/log_reader.h"
#include "shale/env.h"
#include "shale/status.h"

namespace shale {
namespace {

// Counts what the reader reports, and checks what a report must be.
class CountingReporter : public log::Reader::Reporter {
 public:
  void Corruption(size_t bytes, const Status& status) override {
    // "always an error, a Corruption unless a read failed", and a MemEnv read does not fail.
    SHALE_FUZZ_CHECK(status.IsCorruption());
    dropped += bytes;
  }

  uint64_t dropped = 0;
};

// The fewest bytes of header that a record of `size` bytes costs: it is cut into fragments, each
// at most a block less its header, and every fragment has a header. An empty record is one.
uint64_t LeastHeaderBytes(size_t size) {
  constexpr size_t kMostPayload = log::kBlockSize - log::kHeaderSize;
  const uint64_t fragments = size == 0 ? 1 : (size + kMostPayload - 1) / kMostPayload;
  return fragments * log::kHeaderSize;
}

void ReadAll(const std::string& file_name, Env* env, size_t input_size, bool checksum) {
  std::unique_ptr<SequentialFile> file;
  SHALE_FUZZ_CHECK(env->NewSequentialFile(file_name, &file).ok());
  CountingReporter reporter;
  log::Reader reader(file.get(), &reporter, checksum);

  std::string scratch;
  std::string_view record;
  uint64_t records = 0;
  uint64_t returned_bytes = 0;
  uint64_t header_bytes = 0;  // the least that the headers of the records returned took
  uint64_t previous_end = 0;  // the least offset at which the next record may start
  while (reader.ReadRecord(&record, &scratch)) {
    // A record is made of fragments of the file, each with a header of its own, so it cannot be
    // longer than the file, and a reader that returns records without consuming bytes would not
    // stop: a record costs at least one header.
    SHALE_FUZZ_CHECK(record.size() <= input_size);
    records++;
    SHALE_FUZZ_CHECK(records <= input_size / log::kHeaderSize + 1);
    returned_bytes += record.size();
    header_bytes += LeastHeaderBytes(record.size());

    // Records come back in file order, and each one lies in the file after the one before: it
    // begins where its first fragment does, and its fragments hold its bytes and their headers.
    const uint64_t offset = reader.LastRecordOffset();
    SHALE_FUZZ_CHECK(offset >= previous_end);
    previous_end = offset + record.size() + LeastHeaderBytes(record.size());
    SHALE_FUZZ_CHECK(previous_end <= input_size);

    fuzz::Touch(record);
  }
  // What a log gives up is a record, which the reader returns, or a defect, which it reports with
  // the bytes it made it drop, "so that the figures of a log add up to the bytes lost": the two do
  // not share a byte, and the headers of the records returned are in the file too. A reader that
  // returns the bytes of a fragment in two records, or in one and in a report, or that joins a
  // record that a defect interrupted to the next one, counts some byte twice and fails this.
  SHALE_FUZZ_CHECK(returned_bytes + header_bytes + reporter.dropped <= input_size);
  // A log that has ended stays ended.
  SHALE_FUZZ_CHECK(!reader.ReadRecord(&record, &scratch));
}

}  // namespace
}  // namespace shale

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::string name = "/wal.log";
  const std::string_view bytes(reinterpret_cast<const char*>(data), size);
  const std::unique_ptr<shale::Env> env = shale::fuzz::EnvWithFile(name, bytes);
  shale::ReadAll(name, env.get(), size, /*checksum=*/true);
  shale::ReadAll(name, env.get(), size, /*checksum=*/false);
  return 0;
}
