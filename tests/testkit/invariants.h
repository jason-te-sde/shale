#ifndef SHALE_TESTKIT_INVARIANTS_H_
#define SHALE_TESTKIT_INVARIANTS_H_

// The checks that the deterministic simulator runs after its steps: what a database must satisfy
// whatever was done to it. They live in the shale_testkit library, which only test targets link.
//
// Every check ends in one of four outcomes, which Classify() tells apart:
// - OK, when the invariant holds.
// - An alarm, when it does not: a Corruption whose message starts with the invariant's id ("I1: ",
//   "I2: ", ...). It is a finding.
// - A failure to read, when the database or the Env fails to answer a question the check asks: the
//   failure comes back as it came, without an id, because a read that fails says nothing about
//   what the database holds. The simulator allows it while a fault that could cause it is armed,
//   and calls it an error when none is.
// - A caller's bug, when the check is given arguments that no database could satisfy.

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "core/version_state.h"
#include "shale/db.h"
#include "shale/env.h"
#include "shale/status.h"
#include "shale/write_batch.h"
#include "testkit/model_db.h"
#include "util/dbformat.h"

namespace shale {

/// What the Status returned by one of the checks below means. The checks return their findings as
/// Statuses, and a caller that looked only at ok() would take a failure to read for a finding, or a
/// finding for a failure to read, so it switches on this instead.
enum class CheckOutcome : uint8_t {
  /// The invariant holds.
  kOk,
  /// The invariant does not: the database gave a wrong answer, or its files are wrong. The Status
  /// is a Corruption whose message starts with the invariant's id, a letter I, digits and ": ".
  kInvariantViolation,
  /// The database or the Env failed to answer a question the check had to ask: a Get or a scan
  /// that returned an error, a directory that could not be listed, a size that could not be read.
  /// It says nothing about the invariant. It is the Status of the failure as it came; a Corruption
  /// from a damaged table is one, and has no id.
  kReadFailure,
  /// The check was given arguments that no database could satisfy, so it did not run. It is an
  /// InvalidArgument whose message starts with "caller bug: ".
  kCallerBug,
};

/// Returns what `s`, the result of one of the checks below, means: kOk for OK; kInvariantViolation
/// for a Corruption whose message starts with an invariant's id; kCallerBug for an InvalidArgument
/// whose message starts with "caller bug: "; and kReadFailure for every other failure, the
/// engine's own Corruption and InvalidArgument among them.
CheckOutcome Classify(const Status& s);

/// I1: every key of `model` reads back, through `db->Get()` as of `snapshot`, with the value the
/// model holds, and a scan of the whole database as of `snapshot` serves exactly the entries of
/// `model`, in key order, with nothing missing, nothing extra and nothing out of place.
///
/// A null `snapshot` reads the newest state. An alarm names the key and says what was returned and
/// what was expected, and whether the read was at a snapshot. A Get or a scan that fails is
/// returned as it is, and Classify() calls it a kReadFailure: it is not an answer to compare.
/// @param db the database to read.
/// @param model what the database must hold at `snapshot`.
/// @param snapshot the snapshot to read at, or null.
Status CheckReadsMatch(DB* db, const ModelDB::State& model, const Snapshot* snapshot);

/// I2: the version is sound and its files are there.
///
/// - `state` passes CheckInvariants(): the levels are in order, below level 0 their ranges do not
///   overlap, no file number is used twice.
/// - `last_sequence`, the sequence number the database has reached, is not below
///   `previous_last_sequence`, what it had reached when the check last ran, and is not below the
///   sequence number of the newest write that the tables hold, `state`'s.
/// - Every file of `state` is in `live_files`: a file that the version names but the live set does
///   not is one the next sweep may delete.
/// - Every file of `live_files` exists in `env` as a table of `dbname`, and a file that `state`
///   records has the size it records. A file that is live but not in `state`, because an older
///   version that a reader holds names it, is only checked for existing.
///
/// A size that the Env cannot report is returned as the Env's failure.
/// @param env the Env the database runs on.
/// @param dbname the database directory.
/// @param state the current version, as DBImpl::TEST_VersionState() returns it.
/// @param live_files the files of every version alive, as DBImpl::TEST_LiveFiles() returns them.
/// @param last_sequence the sequence number the database has reached, from
///   DBImpl::TEST_LastSequence().
/// @param previous_last_sequence a number that `last_sequence` must not be below. Between two
///   checks of one open database it is what `last_sequence` was at the previous one. Across a
///   reopen the sequence number may legitimately go back, because a crash can lose writes that were
///   not synced; what it cannot go below is the durable floor, the value TEST_LastSequence() had
///   when the newest synced write, or Flush(), was acknowledged. Pass that after a reopen, so that
///   the check can alarm on a reopened database that lost a durable write. 0 is always legal, and
///   switches the leg off.
Status CheckVersion(Env* env, const std::string& dbname, const VersionState& state,
                    const std::set<uint64_t>& live_files, SequenceNumber last_sequence,
                    SequenceNumber previous_last_sequence);

/// I3: every table file in the directory `dbname` is in `live_files`.
///
/// It holds only at a moment when nothing can explain a table left behind, which is all of these:
/// - Background work is idle: while a flush or a compaction runs, the table it is writing is not
///   named by any version yet, and one that has finished leaves its inputs to the sweep after it.
/// - No error is set (property "shale.background-error" is "OK"): with one, the sweep does not run.
/// - No kRemove fault was injected since the last sweep. The sweep that deletes the tables nothing
///   names swallows a RemoveFile that fails, and a listing that fails, and tries again at its next
///   run, without setting an error. A correct database has an orphan until then.
/// - After a reopen, only with the Env's faults cleared: faults stay armed across a crash, and one
///   that fires during the open's own sweep leaves a table as above.
/// - No iterator has been destroyed since the last sweep. An iterator holds the version it was made
///   at, and the tables of that version are in the live set until it is gone; once it is, they stay
///   in the directory until a flush or a compaction ends in a sweep, which destroying the iterator
///   does not run.
/// Where any of them does not hold, do not call it; an orphan then is not a finding.
/// @param env the Env the database runs on.
/// @param dbname the database directory.
/// @param live_files the files of every version alive.
Status CheckNoOrphanTables(Env* env, const std::string& dbname,
                           const std::set<uint64_t>& live_files);

/// I4: the whole contents of `db` equal `durable` with the first k batches of `uncertain` applied,
/// for some k from `min_prefix` to uncertain.size(). A batch is applied whole or not at all, in
/// the order it was written: a database that holds one write and not an earlier one, or half of a
/// batch, matches no k.
///
/// It is what a database must come back as after a crash: everything that was made durable, then a
/// prefix of the writes that were not, and at least as many of those as were acknowledged to a
/// caller that could count on them (`min_prefix`). On OK `*matched`, if not null, is the state that
/// matched; it is left alone otherwise. Where several k give the same state it is that state.
///
/// Returns an InvalidArgument whose message starts with "caller bug: " if `min_prefix` is above
/// uncertain.size(), which no database can meet and which is the caller's mistake; Classify() calls
/// it a kCallerBug. A scan that fails is returned as it is, a kReadFailure.
/// @param db the database to read, as it was reopened.
/// @param durable the model at the last durable point.
/// @param uncertain the writes since, in order, each of which the database may or may not have.
/// @param min_prefix how many of them must have survived.
/// @param matched receives the matched state; may be null.
Status CheckRecoveredPrefix(DB* db, const ModelDB::State& durable,
                            const std::vector<WriteBatch>& uncertain, size_t min_prefix,
                            ModelDB::State* matched);

}  // namespace shale

#endif  // SHALE_TESTKIT_INVARIANTS_H_
