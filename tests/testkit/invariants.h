#ifndef SHALE_TESTKIT_INVARIANTS_H_
#define SHALE_TESTKIT_INVARIANTS_H_

// The checks that the deterministic simulator runs after its steps: what a database must satisfy
// whatever was done to it. They live in the shale_testkit library, which only test targets link.
//
// Every check has one of three outcomes. It returns OK when the invariant holds. It returns a
// Corruption whose message starts with the invariant's id ("I1: ", "I2: ", ...) when it does not:
// an alarm, which IsInvariantViolation() recognises. Or, when the database or the Env fails to
// answer a question the check asks, it returns that failure as it came, without an id, because a
// read that fails says nothing about what the database holds. The simulator allows such a failure
// while a fault that could cause it is armed, and calls it an error when none is.

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

/// Returns true if `s` is an alarm of one of the checks below: a Corruption whose message starts
/// with an invariant's id, a letter I, digits and ": ". It is false for OK, and for the failure of
/// a read, which is a Corruption too when a table is damaged but has no id.
bool IsInvariantViolation(const Status& s);

/// I1: every key of `model` reads back, through `db->Get()` as of `snapshot`, with the value the
/// model holds, and a scan of the whole database as of `snapshot` serves exactly the entries of
/// `model`, in key order, with nothing missing, nothing extra and nothing out of place.
///
/// A null `snapshot` reads the newest state. An alarm names the key and says what was returned and
/// what was expected, and whether the read was at a snapshot. A Get or a scan that fails is
/// returned as it is: it is not an answer to compare.
/// @param db the database to read.
/// @param model what the database must hold at `snapshot`.
/// @param snapshot the snapshot to read at, or null.
Status CheckReadsMatch(DB* db, const ModelDB::State& model, const Snapshot* snapshot);

/// I2: the version is sound and its files are there.
///
/// - `state` passes CheckInvariants(): the levels are in order, below level 0 their ranges do not
///   overlap, no file number is used twice.
/// - `last_sequence`, the database's newest sequence number, is not below `previous_last_sequence`,
///   what it was when the check last ran, and is not below what the tables hold, `state`'s.
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
/// @param last_sequence the database's newest sequence number, from DBImpl::TEST_LastSequence().
/// @param previous_last_sequence the value `last_sequence` had at the previous check; 0 for none.
///   Pass 0 after the database was reopened: a crash may have lost writes that were not synced, so
///   the sequence number of the reopened database may be below what it was.
Status CheckVersion(Env* env, const std::string& dbname, const VersionState& state,
                    const std::set<uint64_t>& live_files, SequenceNumber last_sequence,
                    SequenceNumber previous_last_sequence);

/// I3: every table file in the directory `dbname` is in `live_files`. Only for a database whose
/// background work is idle and that has no error to explain a table left behind: while a flush or
/// a compaction is running, the table it is writing is not named by any version yet.
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
/// Returns InvalidArgument if `min_prefix` is above uncertain.size(), which no database can meet
/// and which is the caller's mistake. A scan that fails is returned as it is.
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
