#ifndef SHALE_UTIL_FILENAME_H_
#define SHALE_UTIL_FILENAME_H_

// The names of the files a database directory holds. Every file the engine creates has a name
// built here and recognised by ParseFileName(), so that recovery can tell its own files from
// anything else in the directory and delete only what it owns.
//
//   NNNNNN.log      write-ahead log
//   NNNNNN.sst      sorted table
//   MANIFEST-NNNNNN the list of live tables, as a log of changes
//   CURRENT         the name of the live MANIFEST
//   LOCK            held while a process has the database open
//   LOG, LOG.old    the informational log, and the one before the last restart
//   NNNNNN.dbtmp    a file being written that has not been renamed into place
//
// NNNNNN is a file number printed in decimal, padded with zeros to six digits; a larger number is
// printed in full.

#include <cstdint>
#include <string>
#include <string_view>

namespace shale {

/// The kind of file a name designates.
enum class FileType : uint8_t {
  kLogFile,         ///< A write-ahead log, `NNNNNN.log`.
  kTableFile,       ///< A sorted table, `NNNNNN.sst`.
  kDescriptorFile,  ///< A MANIFEST, `MANIFEST-NNNNNN`.
  kCurrentFile,     ///< The pointer to the live MANIFEST, `CURRENT`.
  kLockFile,        ///< The lock file, `LOCK`.
  kInfoLogFile,     ///< The informational log, `LOG` or `LOG.old`.
  kTempFile,        ///< A file not yet renamed into place, `NNNNNN.dbtmp`.
};

/// Returns the path of write-ahead log `number` in the database directory `dbname`: `dbname`, a
/// slash and the file name. The directory name is used exactly as given.
std::string LogFileName(const std::string& dbname, uint64_t number);

/// Returns the path of table `number` in `dbname`.
std::string TableFileName(const std::string& dbname, uint64_t number);

/// Returns the path of MANIFEST `number` in `dbname`.
std::string DescriptorFileName(const std::string& dbname, uint64_t number);

/// Returns the path of the CURRENT file in `dbname`.
std::string CurrentFileName(const std::string& dbname);

/// Returns the path of the LOCK file in `dbname`.
std::string LockFileName(const std::string& dbname);

/// Returns the path of the informational log in `dbname`.
std::string InfoLogFileName(const std::string& dbname);

/// Returns the path the informational log is renamed to when the database is opened again.
std::string OldInfoLogFileName(const std::string& dbname);

/// Returns the path of temporary file `number` in `dbname`.
std::string TempFileName(const std::string& dbname, uint64_t number);

/// Recognises a file name. `filename` is the bare name, with no directory part. On success it
/// stores the kind in `*type` and the file number in `*number`, which is 0 for the kinds that
/// have none, and returns true. A name that is not exactly one of the forms above returns false
/// and leaves both outputs untouched; that includes a number that does not fit in 64 bits.
bool ParseFileName(std::string_view filename, uint64_t* number, FileType* type);

}  // namespace shale

#endif  // SHALE_UTIL_FILENAME_H_
