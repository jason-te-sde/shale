#include "leveldb_task_env.h"

#include <leveldb/env.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>

namespace shale {
namespace {

// A task that LevelDB scheduled, and the count it leaves when it returns.
struct CountedTask {
  void (*function)(void* arg);
  void* arg;
  std::shared_ptr<std::atomic<uint64_t>> in_flight;
};

void RunCountedTask(void* arg) {
  const std::unique_ptr<CountedTask> task(static_cast<CountedTask*>(arg));
  task->function(task->arg);
  task->in_flight->fetch_sub(1, std::memory_order_acq_rel);
}

class TaskCountingEnv final : public leveldb::EnvWrapper {
 public:
  TaskCountingEnv(leveldb::Env* target, std::shared_ptr<std::atomic<uint64_t>> in_flight)
      : leveldb::EnvWrapper(target), in_flight_(std::move(in_flight)) {}

  void Schedule(void (*function)(void* arg), void* arg) override {
    in_flight_->fetch_add(1, std::memory_order_acq_rel);
    auto task = std::make_unique<CountedTask>(CountedTask{function, arg, in_flight_});
    target()->Schedule(&RunCountedTask, task.release());
  }

 private:
  const std::shared_ptr<std::atomic<uint64_t>> in_flight_;
};

}  // namespace

std::unique_ptr<leveldb::Env> NewTaskCountingEnv(leveldb::Env* target,
                                                 std::shared_ptr<std::atomic<uint64_t>> in_flight) {
  return std::make_unique<TaskCountingEnv>(target, std::move(in_flight));
}

}  // namespace shale
