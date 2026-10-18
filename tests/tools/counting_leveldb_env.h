#ifndef SHALE_TESTS_TOOLS_COUNTING_LEVELDB_ENV_H_
#define SHALE_TESTS_TOOLS_COUNTING_LEVELDB_ENV_H_

// A LevelDB Env for the test of the LevelDB engine of shale_bench: the default Env, which counts
// the syncs of the files that its caller writes. It is its own translation unit because LevelDB is
// built without RTTI, and a class that derives from one of its classes has to be built without it
// too: GoogleTest is built with it, so the two cannot share a file.

#include <leveldb/env.h>

#include <atomic>
#include <string>

namespace shale {

// The default Env, with every file that is written through it wrapped in one that counts how many
// times it is synced, for the files whose name ends in ".log": the write-ahead log, which is the
// file that a synced write goes through.
class CountingLevelDBEnv final : public leveldb::EnvWrapper {
 public:
  CountingLevelDBEnv();

  leveldb::Status NewWritableFile(const std::string& name, leveldb::WritableFile** file) override;
  leveldb::Status NewAppendableFile(const std::string& name, leveldb::WritableFile** file) override;

  // The number of syncs of logs so far, from any thread.
  int log_syncs() const { return log_syncs_.load(); }

 private:
  leveldb::Status Wrap(const leveldb::Status& made, const std::string& name,
                       leveldb::WritableFile** file);

  std::atomic<int> log_syncs_{0};
};

}  // namespace shale

#endif  // SHALE_TESTS_TOOLS_COUNTING_LEVELDB_ENV_H_
