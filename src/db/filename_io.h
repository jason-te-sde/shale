#ifndef SHALE_DB_FILENAME_IO_H_
#define SHALE_DB_FILENAME_IO_H_

// The two operations that make a change to a database directory itself durable: pointing CURRENT at
// a MANIFEST, and syncing the directory. Everything else the engine does to the directory is
// ordinary file I/O, and these are the ones whose order a crash can expose.

#include <cstdint>
#include <string>

#include "shale/env.h"
#include "shale/status.h"

namespace shale {

/// Returns the contents of the CURRENT file that names MANIFEST `descriptor_number` in `dbname`,
/// which is the format of the file: two lines,
///
///     MANIFEST-%06llu
///     crc32c:%08x
///
/// the bare name of the MANIFEST, and the masked CRC32C (crc32c::Mask) of the first line with its
/// newline, in eight lower-case hex digits. Both lines end in a newline. The checksum is there
/// because CURRENT is one small file, read once, whose one flipped bit can make it name another
/// MANIFEST that exists, which the database would then open as if it were the live one: with it, a
/// CURRENT is the one that was written or it is refused.
/// @param dbname the database directory.
/// @param descriptor_number the number of the MANIFEST.
std::string CurrentFileContents(const std::string& dbname, uint64_t descriptor_number);

/// Reads the contents of a CURRENT file: the number of the MANIFEST that it names in `*number`, or
/// the reason it is refused. Only the form that CurrentFileContents() makes is accepted, byte for
/// byte, so no single flipped bit of a CURRENT is: Corruption "CURRENT file does not end with
/// newline" for contents that are empty or do not end with one; "CURRENT file is malformed" for
/// anything that is not a name line and then a checksum line of "crc32c:" and eight lower-case hex
/// digits; "CURRENT checksum mismatch" for a checksum that is not the one of the name line; and
/// "CURRENT names an invalid file" for a name line that is not the name of a MANIFEST of `dbname`
/// spelled as SetCurrentFile() spells it. They are checked in that order, so that a damaged name
/// is reported as damage and not as a name.
/// @param dbname the database directory.
/// @param contents what CURRENT holds.
/// @param number receives the number of the MANIFEST; left alone unless the status is OK.
Status ParseCurrentFile(const std::string& dbname, const std::string& contents, uint64_t* number);

/// Makes CURRENT name MANIFEST `descriptor_number`, atomically and durably.
///
/// The new contents, CurrentFileContents(), are written to a temporary file in the directory and
/// synced, the temporary file is renamed over CURRENT, and the directory is synced. A reader, and a
/// recovery after a crash, therefore finds CURRENT naming the old MANIFEST or the new one, whole,
/// and never an empty or half-written file, which a write in place would leave. The file is the
/// only record of which MANIFEST is live, so a CURRENT that could not be read would be a database
/// that could not be opened.
///
/// On a failure before the rename, CURRENT is as it was and the temporary file is removed. On a
/// failure of the directory sync, the rename has been done and whether it will survive a power
/// loss is not known: CURRENT names the old MANIFEST or the new, and the caller must not rely on
/// either.
/// @param env the Env to write through.
/// @param dbname the database directory.
/// @param descriptor_number the number of the MANIFEST that becomes the live one; it must exist.
Status SetCurrentFile(Env* env, const std::string& dbname, uint64_t descriptor_number);

/// Makes the creations, removals and renames in the database directory durable. It is the engine's
/// only call to Env::SyncDir(), so that the places that need it are the places that call this.
/// @param env the Env to sync through.
/// @param dbname the database directory.
Status SyncDirectory(Env* env, const std::string& dbname);

}  // namespace shale

#endif  // SHALE_DB_FILENAME_IO_H_
