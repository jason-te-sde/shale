#ifndef SHALE_UTIL_CRC32C_H_
#define SHALE_UTIL_CRC32C_H_

#include <cstddef>
#include <cstdint>

namespace shale::crc32c {

/// True when this build computes Extend() with the CPU's CRC32C instructions, and false when
/// Extend() is ExtendPortable(). Only 64-bit ARM builds that enable the CRC extension have them;
/// every other target, x86 included, uses the table-driven code.
#if defined(__ARM_FEATURE_CRC32)
inline constexpr bool kHasHardwareSupport = true;
#else
inline constexpr bool kHasHardwareSupport = false;
#endif

/// Returns the CRC32C (Castagnoli, polynomial 0x1edc6f41) of `init_crc` extended by the `n` bytes
/// at `data`: Extend(Value(a), b) equals Value(a followed by b). `data` may be null when `n` is
/// zero. Uses the CPU's instructions where the build has them, see kHasHardwareSupport.
uint32_t Extend(uint32_t init_crc, const char* data, size_t n);

/// The same function as Extend(), computed with eight lookup tables, in every build. It is the
/// reference the hardware path is tested against, and the whole implementation on targets without
/// one.
uint32_t ExtendPortable(uint32_t init_crc, const char* data, size_t n);

/// Returns the CRC32C of the `n` bytes at `data`.
inline uint32_t Value(const char* data, size_t n) { return Extend(0, data, n); }

/// Returns a masked form of `crc`, for storing a checksum inside the data it covers. A CRC of
/// bytes that contain that same CRC comes out as a fixed value, which defeats the check;
/// rotating and offsetting the stored copy avoids it. The formula is part of the file format:
/// ((crc >> 15) | (crc << 17)) + 0xa282ead8.
uint32_t Mask(uint32_t crc);

/// Returns the CRC that Mask() turned into `masked`.
uint32_t Unmask(uint32_t masked);

}  // namespace shale::crc32c

#endif  // SHALE_UTIL_CRC32C_H_
