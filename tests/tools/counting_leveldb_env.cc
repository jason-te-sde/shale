#include "counting_leveldb_env.h"

#include <leveldb/env.h>
#include <leveldb/slice.h>
#include <leveldb/status.h>

#include <atomic>
#include <memory>
#include <string>

namespace shale {
namespace {

class CountingFile final : public leveldb::WritableFile {
 public:
  CountingFile(leveldb::WritableFile* target, std::atomic<int>* log_syncs, bool is_log)
      : target_(target), log_syncs_(log_syncs), is_log_(is_log) {}

  leveldb::Status Append(const leveldb::Slice& data) override { return target_->Append(data); }
  leveldb::Status Close() override { return target_->Close(); }
  leveldb::Status Flush() override { return target_->Flush(); }
  leveldb::Status Sync() override {
    if (is_log_) {
      log_syncs_->fetch_add(1);
    }
    return target_->Sync();
  }

 private:
  const std::unique_ptr<leveldb::WritableFile> target_;
  std::atomic<int>* const log_syncs_;
  const bool is_log_;
};

}  // namespace

CountingLevelDBEnv::CountingLevelDBEnv() : leveldb::EnvWrapper(leveldb::Env::Default()) {}

leveldb::Status CountingLevelDBEnv::NewWritableFile(const std::string& name,
                                                    leveldb::WritableFile** file) {
  return Wrap(target()->NewWritableFile(name, file), name, file);
}

leveldb::Status CountingLevelDBEnv::NewAppendableFile(const std::string& name,
                                                      leveldb::WritableFile** file) {
  return Wrap(target()->NewAppendableFile(name, file), name, file);
}

leveldb::Status CountingLevelDBEnv::Wrap(const leveldb::Status& made, const std::string& name,
                                         leveldb::WritableFile** file) {
  if (made.ok()) {
    const bool is_log = name.size() > 4 && name.compare(name.size() - 4, 4, ".log") == 0;
    *file = new CountingFile(*file, &log_syncs_, is_log);
  }
  return made;
}

}  // namespace shale
