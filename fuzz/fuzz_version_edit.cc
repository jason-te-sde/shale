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

  shale::VersionEdit edit;
  const shale::Status s = edit.DecodeFrom(input);
  if (!s.ok()) {
    // "Returns Corruption, with a message that starts "VersionEdit: ", and leaves this edit as it
    // was": it was empty.
    SHALE_FUZZ_CHECK(s.IsCorruption());
    SHALE_FUZZ_CHECK(edit == shale::VersionEdit{});
    return 0;
  }

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
