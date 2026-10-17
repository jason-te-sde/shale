#ifndef SHALE_IO_ENV_UTIL_H_
#define SHALE_IO_ENV_UTIL_H_

#include <string>
#include <string_view>

#include "shale/env.h"
#include "shale/status.h"

namespace shale {

/// Creates the file `fname`, or empties it if it exists, and writes `data` to it.
///
/// With `sync` the bytes are durable when this returns OK; the directory entry is not, which is
/// Env::SyncDir(). If anything fails the file is removed, so a caller never finds half of it.
/// @param env the Env to write through.
/// @param data the contents of the file.
/// @param fname the path of the file.
/// @param sync whether to make the bytes durable before returning.
Status WriteStringToFile(Env* env, std::string_view data, const std::string& fname, bool sync);

/// Reads the whole of the file `fname` into `*data`, replacing what `*data` held. On failure
/// `*data` is empty.
/// @param env the Env to read through.
/// @param fname the path of the file.
/// @param data receives the contents.
Status ReadFileToString(Env* env, const std::string& fname, std::string* data);

}  // namespace shale

#endif  // SHALE_IO_ENV_UTIL_H_
