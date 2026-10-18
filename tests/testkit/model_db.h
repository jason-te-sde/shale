#ifndef SHALE_TESTKIT_MODEL_DB_H_
#define SHALE_TESTKIT_MODEL_DB_H_

// The reference model of the database that the deterministic simulator compares it with. It lives
// in the shale_testkit library, which only test targets link.

#include <cstdint>
#include <map>
#include <optional>
#include <string>

#include "shale/write_batch.h"

namespace shale {

/// What a database holds, as the plainest thing that can hold it: a sorted map from key to value.
///
/// The simulator applies to it every write it makes to the database and takes its answer as the
/// right one. It has no sequence numbers, no levels and no files, so there is nothing in it for a
/// bug of the engine to be shared with. Keys are held in bytewise order, as the database orders
/// them, so a scan of the database and a walk of the map go in step.
///
/// Apply() and the single writes take effect whatever the database did with the same write. It is
/// for the caller to apply only writes that the database accepted or may have accepted, which are
/// not the ones it refused with InvalidArgument. A ModelDB is not thread-safe.
class ModelDB {
 public:
  /// The contents: key to value, in bytewise order of key.
  using State = std::map<std::string, std::string>;

  /// Sets `key` to `value`.
  void Put(std::string key, std::string value);

  /// Removes `key`. It is not an error if there is none.
  void Delete(const std::string& key);

  /// Applies every operation of `batch` in order, so that a later operation on a key supersedes an
  /// earlier one. A batch built through WriteBatch::Put() and Delete() cannot be malformed; one
  /// that is aborts the process, since a model that applied half of it would be a wrong answer
  /// that looks like a right one.
  void Apply(const WriteBatch& batch);

  /// Returns the value of `key`, or nullopt if it has none.
  std::optional<std::string> Get(const std::string& key) const;

  /// Returns the contents.
  const State& state() const;

  /// Replaces the contents with `state`: after a crash and a recovery, the state that the database
  /// came back with.
  void Reset(State state);

  /// Returns a 64-bit digest of `state`, for a trace that has to say which contents it saw without
  /// printing them, and for comparing two runs.
  ///
  /// Every key and every value is hashed with its length in front of it, in key order, so that
  /// where one ends and the next begins is part of what the digest sees: {"ab": ""} and
  /// {"a": "b"} differ, as do {"a": "1", "b": "2"} and {"a": "2", "b": "1"}. Equal maps have equal
  /// digests, and the value is the same on every platform. It is a digest and not a proof: two
  /// states can collide, so a check that must be exact compares the states.
  static uint64_t Digest(const State& state);

 private:
  State state_;
};

}  // namespace shale

#endif  // SHALE_TESTKIT_MODEL_DB_H_
