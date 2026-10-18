#ifndef SHALE_FUZZ_VERSION_SET_INPUT_H_
#define SHALE_FUZZ_VERSION_SET_INPUT_H_

// The input of fuzz_version_set, and the way make_corpus writes one. The encoding is shared so that
// a seed cannot drift from what the target reads.
//
// The target writes CURRENT and a MANIFEST into a MemEnv and recovers a VersionSet from them. A
// fuzzer cannot compute a CRC32C, so bytes that were the MANIFEST as they are would end at the
// first record's checksum, and bytes that were CURRENT would never name a MANIFEST: the code behind
// both would not be reached. So the input describes the two files and the target writes them
// properly.
//
//   byte 0   layout:
//              bit 0     CURRENT is valid: it names MANIFEST-<n> as the engine writes it, with its
//                        crc32c line, where n is picked by bits 1 to 3 from kManifestNumbers;
//                        clear, CURRENT is the bytes that follow
//              bits 1-3  the index into kManifestNumbers
//              bit 4     the MANIFEST is a log of records, each a varint32 length and that many
//                        bytes, which the target frames and checksums; clear, the MANIFEST is the
//                        rest of the input as it is
//              bits 5-7  how many records go to the MANIFEST: 1 + 2 * these; the records after
//                        them are edits applied to what the recovery gives, one by one
//   byte 1   an index into kHostileNumbers, the file number that the recovered version set is told
//            was used; a value outside the table says none
//   then, if bit 0 of the layout is clear: one byte n, and n bytes of CURRENT
//   then the MANIFEST: the records, or the bytes.
//
// A missing byte reads as zero, so every input means something.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "db/version_set.h"
#include "util/coding.h"

namespace shale::fuzz {

/// The numbers that a valid CURRENT may name: small ones, and the ones at the edge of what a
/// database accepts, where a MANIFEST is refused as numbered too high.
inline constexpr uint64_t kManifestNumbers[8] = {
    1, 2, 5, 123456, uint64_t{1} << 40, kMaxFileNumber - 1, kMaxFileNumber, kMaxFileNumber + 1,
};

/// The numbers that a file may be reported found with, for MarkFileNumberUsed(): none, and ones
/// that leave the counter at, next to, or far past the ceiling. A name in a database directory is
/// data.
inline constexpr uint64_t kHostileNumbers[8] = {
    0,
    1,
    uint64_t{1} << 32,
    kMaxFileNumber - 2,
    kMaxFileNumber - 1,
    kMaxFileNumber,
    kMaxFileNumber + 1,
    ~uint64_t{0},
};

/// What an input says, after it is read.
struct VersionSetInput {
  bool current_valid = false;
  unsigned manifest_number_index = 0;
  bool framed = false;
  unsigned manifest_records = 1;
  unsigned hostile_index = 8;        // outside the table: none
  std::string current;               // CURRENT as given, when it is not valid
  std::string manifest;              // the MANIFEST as given, when it is not framed
  std::vector<std::string> records;  // the records, when it is
};

/// Reads `bytes` as the encoding above.
inline VersionSetInput DecodeVersionSetInput(std::string_view bytes) {
  VersionSetInput in;
  size_t pos = 0;
  const auto take = [&]() -> uint8_t {
    return pos < bytes.size() ? static_cast<uint8_t>(bytes[pos++]) : uint8_t{0};
  };
  const uint8_t layout = take();
  in.current_valid = (layout & 1U) != 0;
  in.manifest_number_index = (layout >> 1) & 7U;
  in.framed = (layout & 0x10U) != 0;
  in.manifest_records = 1U + 2U * (static_cast<unsigned>(layout) >> 5);
  in.hostile_index = take();
  if (!in.current_valid) {
    const size_t wanted = take();
    const size_t length = std::min(wanted, bytes.size() - std::min(pos, bytes.size()));
    in.current = std::string(bytes.substr(std::min(pos, bytes.size()), length));
    pos += length;
  }
  std::string_view rest = bytes.substr(std::min(pos, bytes.size()));
  if (!in.framed) {
    in.manifest = std::string(rest);
    return in;
  }
  while (!rest.empty()) {
    uint32_t length = 0;
    if (!GetVarint32(&rest, &length)) {
      break;
    }
    const size_t n = std::min<size_t>(length, rest.size());
    in.records.emplace_back(rest.substr(0, n));
    rest.remove_prefix(n);
  }
  return in;
}

/// Writes `in` as the encoding above, for make_corpus.
inline std::string EncodeVersionSetInput(const VersionSetInput& in) {
  std::string out;
  const unsigned split = (in.manifest_records - 1U) / 2U;
  out.push_back(static_cast<char>((in.current_valid ? 1U : 0U) | (in.manifest_number_index << 1) |
                                  (in.framed ? 0x10U : 0U) | (split << 5)));
  out.push_back(static_cast<char>(in.hostile_index));
  if (!in.current_valid) {
    out.push_back(static_cast<char>(in.current.size()));
    out.append(in.current);
  }
  if (!in.framed) {
    out.append(in.manifest);
    return out;
  }
  for (const std::string& record : in.records) {
    PutVarint32(&out, static_cast<uint32_t>(record.size()));
    out.append(record);
  }
  return out;
}

}  // namespace shale::fuzz

#endif  // SHALE_FUZZ_VERSION_SET_INPUT_H_
