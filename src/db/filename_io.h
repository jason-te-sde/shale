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

/// Makes CURRENT name MANIFEST `descriptor_number`, atomically and durably.
///
/// The new contents, "MANIFEST-%06llu\n", are written to a temporary file in the directory and
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
