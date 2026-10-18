// Feeds the input to the version edit decoder as one record of a MANIFEST. An edit that decodes is
// encoded again and decoded a second time, and the two must be equal: whatever the decoder accepts
// is something the encoder can write back, so that a MANIFEST which was read can be rewritten by
// the engine without changing what it says.
//
// A MANIFEST is read at every open, so a damaged one has to come back as Corruption: never as a
// crash, and never as an edit that the encoder then refuses.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "core/version_edit.h"
#include "fuzz_util.h"
#include "shale/status.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::string_view input(reinterpret_cast<const char*>(data), size);

  // The edit that is decoded into is not empty, so that "leaves this edit as it was" on failure and
  // "replaces this edit" on success are both something that can be seen: a decoder that cleared the
  // edit before it failed, or that added to what was there, is not the one that is documented.
  shale::VersionEdit sentinel;
  sentinel.log_number = 1;
  sentinel.deleted_files.insert({0, 1});

  shale::VersionEdit edit = sentinel;
  const shale::Status s = edit.DecodeFrom(input);
  if (!s.ok()) {
    SHALE_FUZZ_CHECK(s.IsCorruption());
    SHALE_FUZZ_CHECK(s.ToString().find("VersionEdit: ") != std::string::npos);
    SHALE_FUZZ_CHECK(edit == sentinel);
    return 0;
  }
  shale::VersionEdit fresh;
  SHALE_FUZZ_CHECK(fresh.DecodeFrom(input).ok());
  SHALE_FUZZ_CHECK(edit == fresh);

  // Descriptions are for logs and for the dump tools, which are pointed at damaged files.
  shale::fuzz::Touch(edit.DebugString());

  std::string encoded;
  edit.EncodeTo(&encoded);
  shale::VersionEdit again;
  SHALE_FUZZ_CHECK(again.DecodeFrom(encoded).ok());
  SHALE_FUZZ_CHECK(again == edit);

  // One edit has one encoding, so what was written is what writing it again gives.
  std::string encoded_again;
  again.EncodeTo(&encoded_again);
  SHALE_FUZZ_CHECK(encoded_again == encoded);
  return 0;
}
