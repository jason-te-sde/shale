#ifndef SHALE_TESTS_DB_DB_TEST_UTIL_H_
#define SHALE_TESTS_DB_DB_TEST_UTIL_H_

// What the tests of the open database share: a fixture that holds a database over the
// fault-injecting Env and a scheduler only the test drives, and the helpers that put a database
// directory into a state the engine itself would never write, to see what Open makes of it.

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "db/db_impl.h"
#include "shale/db.h"
#include "shale/options.h"
#include "shale/scheduler.h"
#include "shale/status.h"
#include "testkit/fault_env.h"
#include "testkit/test_util.h"
#include "util/dbformat.h"

namespace shale {

/// A test that works on one database, "/db", over a FaultInjectionEnv with seed 1 and a
/// ManualScheduler. The database is open when the test body starts and closed when it ends.
class DBTestBase : public ::testing::Test {
 protected:
  /// Creates the Env and opens the database with CurrentOptions().
  DBTestBase();

  /// Closes the database.
  ~DBTestBase() override;

  /// Returns the options a test starts from: the fixture's Env and scheduler, and
  /// create_if_missing, so that the first open creates the database.
  Options CurrentOptions();

  /// Closes the database, if it is open, and opens it with `options`. On failure the database is
  /// closed and the Status is returned.
  Status TryReopen(const Options& options);

  /// Closes the database and opens it with CurrentOptions(), failing the test if that fails.
  void Reopen();

  /// Closes the database and opens it with `options`, failing the test if that fails.
  void Reopen(const Options& options);

  /// Closes the database, if it is open.
  void Close();

  /// Writes `k` = `v`; the write is synced unless `sync` says otherwise.
  Status Put(std::string_view k, std::string_view v, bool sync = true);

  /// Deletes `k`, with a synced write.
  Status Delete(std::string_view k);

  /// Returns the value of `k` as of `snapshot` (the newest if it is null), "NOT_FOUND" if there is
  /// none, or the text of the Status of a read that failed.
  std::string Get(std::string_view k, const Snapshot* snapshot = nullptr);

  /// Returns every entry the database holds, as "k1=v1,k2=v2", from a scan of an iterator over it.
  /// If the iterator fails, returns the text of its Status.
  std::string Contents(const Snapshot* snapshot = nullptr);

  /// Returns the number of table files at `level`.
  int NumTableFilesAtLevel(int level);

  /// Returns the names of the files in the database directory, sorted.
  std::vector<std::string> DbFiles();

  /// Returns the open database as the class under test.
  DBImpl* dbfull();

  std::unique_ptr<FaultInjectionEnv> env_;  ///< The Env everything runs on.
  ManualScheduler scheduler_;               ///< The scheduler the database is given.
  std::string dbname_;                      ///< The database directory.
  std::unique_ptr<DB> db_;                  ///< The open database, or null after Close().
};

/// Creates the database `dbname` in `env` as an empty one whose MANIFEST records `last_sequence`
/// and `next_file_number`, as a database that had been written to for a long time would. `dbname`
/// must not hold a database. The numbers are those a MANIFEST may hold, and no more: the point is
/// to reach the ceilings that no test can reach by writing.
Status ForgeEmptyDatabase(Env* env, const std::string& dbname, SequenceNumber last_sequence,
                          uint64_t next_file_number);

/// Returns the encoding of a write batch with the sequence number `sequence` and one put for each
/// of `puts`, in order: the bytes that the log stores for it.
std::string EncodedBatch(SequenceNumber sequence, const Entries& puts);

/// Writes `records` to the file `fname` as the records of a write-ahead log, synced, so that Open
/// finds a log that the database never wrote.
Status WriteLogFile(Env* env, const std::string& fname, const std::vector<std::string>& records);

/// Returns the offset in `fname` of the start of each record of the write-ahead log, as the log
/// reader finds them.
std::vector<uint64_t> LogRecordOffsets(Env* env, const std::string& fname);

/// Returns the path of the write-ahead log with the highest number in `dbname`, or an empty string
/// if there is none.
std::string NewestLogFile(Env* env, const std::string& dbname);

}  // namespace shale

#endif  // SHALE_TESTS_DB_DB_TEST_UTIL_H_
