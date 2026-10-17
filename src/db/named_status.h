#ifndef SHALE_DB_NAMED_STATUS_H_
#define SHALE_DB_NAMED_STATUS_H_

// Errors that say which file they are about. A Corruption from a table or a MANIFEST has to name
// it: the code that found it knows the file, and the person reading the log, or a Status that has
// travelled up through a read, a flush and a compaction, no longer does.

#include <string>

#include "shale/status.h"

namespace shale {

/// Returns `s` with `file` added to its message, as the detail the Status factories take, so that
/// "Corruption: bad block" becomes "Corruption: bad block: /db/000007.sst". The code is the same
/// as `s`'s, and so is the message; only the detail is new.
///
/// An OK status is returned as it is. So is an IOError: the Env names the path in every one it
/// returns, and naming it a second time would only make the message longer.
/// @param s the status to add the file to.
/// @param file the path of the file the status is about.
Status WithFileName(const Status& s, const std::string& file);

}  // namespace shale

#endif  // SHALE_DB_NAMED_STATUS_H_
