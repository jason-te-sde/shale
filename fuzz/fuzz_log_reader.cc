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

void ReadAll(const std::string& file_name, Env* env, size_t input_size, bool checksum) {
  std::unique_ptr<SequentialFile> file;
  SHALE_FUZZ_CHECK(env->NewSequentialFile(file_name, &file).ok());
  CountingReporter reporter;
  log::Reader reader(file.get(), &reporter, checksum);

  std::string scratch;
  std::string_view record;
  uint64_t records = 0;
  uint64_t returned_bytes = 0;
  uint64_t previous_offset = 0;
  while (reader.ReadRecord(&record, &scratch)) {
    // A record is made of fragments of the file, each with a header of its own, so it cannot be
    // longer than the file, and a reader that returns records without consuming bytes would not
    // stop: a record costs at least one header.
    SHALE_FUZZ_CHECK(record.size() <= input_size);
    records++;
    SHALE_FUZZ_CHECK(records <= input_size / log::kHeaderSize + 1);
    returned_bytes += record.size();
    SHALE_FUZZ_CHECK(returned_bytes <= input_size);

    // Records come back in file order, and start inside the file.
    const uint64_t offset = reader.LastRecordOffset();
    SHALE_FUZZ_CHECK(offset < input_size);
    SHALE_FUZZ_CHECK(records == 1 || offset > previous_offset);
    previous_offset = offset;

    fuzz::Touch(record);
  }
  // "so that the figures of a log add up to the bytes lost": what was dropped is part of the file.
  SHALE_FUZZ_CHECK(reporter.dropped <= input_size);
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
