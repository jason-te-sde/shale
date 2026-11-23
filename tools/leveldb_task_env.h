#ifndef SHALE_TOOLS_LEVELDB_TASK_ENV_H_
#define SHALE_TOOLS_LEVELDB_TASK_ENV_H_

// The Env that the LevelDB engine of shale_bench opens its databases with: another Env, which
// counts the background tasks that LevelDB hands it. It is its own translation unit because
// LevelDB is built without RTTI, and a class that derives from one of its classes has to be built
// without it too.

#include <leveldb/env.h>

#include <atomic>
#include <cstdint>
#include <memory>

namespace shale {

/// Returns an Env that does everything that `target` does, and counts in `*in_flight` the tasks
/// handed to its Schedule() that have not returned. LevelDB runs every flush and every compaction
/// as such a task, and a task that finds more to do schedules the next one before it returns, so
/// the count is zero only when LevelDB has nothing left to run in the background.
/// @param target the Env to forward to; it must outlive the one returned.
/// @param in_flight the count, shared with each task, which may still be returning after the Env
///   returned is destroyed.
std::unique_ptr<leveldb::Env> NewTaskCountingEnv(leveldb::Env* target,
                                                 std::shared_ptr<std::atomic<uint64_t>> in_flight);

}  // namespace shale

#endif  // SHALE_TOOLS_LEVELDB_TASK_ENV_H_
