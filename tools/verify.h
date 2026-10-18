#ifndef SHALE_TOOLS_VERIFY_H_
#define SHALE_TOOLS_VERIFY_H_

// The offline check behind `shalectl verify`: reads every live table of a database that nothing has
// open, and says what is wrong with it. It writes no byte of what the database holds, but it is not
// read-only: it takes the LOCK, which needs write access to the directory. See VerifyDatabase().

#include <string>
#include <vector>

#include "core/version_edit.h"
#include "shale/env.h"
#include "shale/iterator.h"
#include "shale/status.h"

namespace shale {

/// Checks the database in `dbname`, and changes no file of it but one: it takes its LOCK, creating
/// the empty file if the directory has none, as DB::Open() would, so that it needs write access to
/// the directory and cannot check a database on read-only media. Then it reads the MANIFEST that
/// CURRENT names with VersionSet::Recover(), which writes nothing, and every table that the
/// recovered state names, in full and with checksums on. Each defect found is one line appended to
/// `*problems`: a MANIFEST or CURRENT that does not recover, and, for each table, the first of
/// these that is wrong with it: it is not there, its size is not the one the MANIFEST records, it
/// cannot be read, or its entries are not sorted and inside the range the MANIFEST records (see
/// CheckTableEntries()). A defect in the MANIFEST ends the check, since no tables are known then.
/// The write-ahead logs are not read.
///
/// Returns OK when the check ran to the end, whether or not it found anything: `*problems` is
/// what it found. Returns an error, with `*problems` empty, when it could not start:
/// InvalidArgument for a directory that holds no database (it has no CURRENT, and nothing is
/// created in it), and the IOError of a LOCK that another process or handle holds, which is a
/// database that is open, or that cannot be taken, which is a directory that cannot be written. A
/// caller tells damage from that failure by which of the two it got.
/// @param env the Env to read through.
/// @param dbname the directory of the database.
/// @param problems receives the defects found, one line each; cleared first.
Status VerifyDatabase(Env* env, const std::string& dbname, std::vector<std::string>* problems);

/// Reads every entry of `iter`, the entries of table `file` from its first to its last, and
/// returns the first thing wrong with them as a Corruption naming `path`, or OK. An entry is wrong
/// if its key is not an internal key, if it is not after the entry before it in
/// CompareInternalKey() order, if it is before `file.smallest` or after `file.largest`, which are
/// the range the MANIFEST says the table holds, or if the iterator stops with an error, which is
/// returned with `path` added to it unless it names the file already.
/// @param iter an iterator over the table, which this call positions.
/// @param file what the MANIFEST records about the table.
/// @param path the name of the table's file, for the message.
Status CheckTableEntries(Iterator* iter, const FileMetaData& file, const std::string& path);

}  // namespace shale

#endif  // SHALE_TOOLS_VERIFY_H_
