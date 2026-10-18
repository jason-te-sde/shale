#ifndef SHALE_FUZZ_DB_OPS_PROGRAM_H_
#define SHALE_FUZZ_DB_OPS_PROGRAM_H_

// The operation program that fuzz_db_ops runs, and the way make_corpus writes one. The encoding
// is shared so that a seed cannot drift from what the target reads.
//
// A program is a byte string. Each operation is one byte, which picks the operation by its value
// modulo six, followed by the operand bytes that the operation takes:
//
//   put      key, length    sets the key to a value of `length` bytes
//   delete   key
//   get      key
//   flush
//   compact  low, high      compacts the keys from the smaller of the two to the larger
//   scan     key            reads everything, and then everything from the key on
//
// A key operand is a byte modulo 16, a length one modulo 65, so every byte means something and a
// mutation of one never makes a program invalid. A program that ends inside an operation stops.

#include <cstddef>
#include <cstdint>
#include <string>

namespace shale::fuzz {

/// The operations of a program, in the order of their codes.
enum class DbOp : uint8_t { kPut, kDelete, kGet, kFlush, kCompact, kScan };

/// How many operations there are; an operation byte is taken modulo this.
inline constexpr unsigned kNumDbOps = 6;

/// How many different keys a program names.
inline constexpr unsigned kNumDbKeys = 16;

/// The longest value a put writes, in bytes.
inline constexpr unsigned kMaxDbValueLength = 64;

/// How long the padding is of a key that has some: see DbOpKey().
inline constexpr size_t kDbKeyPadding = 4096;

/// Returns the key a key operand names: "key00" to "key15", and for three keys in four, those whose
/// number is not a multiple of four, as many padding bytes after that as kDbKeyPadding says.
///
/// The padding is there for the data and not for the keys. Values are at most 64 bytes, so sixteen
/// short keys never hold the 64 KiB that level 1 may grow to before the engine moves a file down to
/// level 2, and a program would only ever make levels 0 and 1: no compaction with something below
/// its output, no file moved down a level, and no deletion that has to be kept because an older
/// value is deeper. With twelve keys of 4 KiB a program that puts most of them fills level 1.
inline std::string DbOpKey(unsigned operand) {
  const unsigned index = operand % kNumDbKeys;
  std::string key = "key";
  key.push_back(static_cast<char>('0' + index / 10));
  key.push_back(static_cast<char>('0' + index % 10));
  if (index % 4 != 0) {
    key.append(kDbKeyPadding, 'p');
  }
  return key;
}

}  // namespace shale::fuzz

#endif  // SHALE_FUZZ_DB_OPS_PROGRAM_H_
