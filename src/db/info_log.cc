#include "db/info_log.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "shale/env.h"
#include "util/filename.h"
#include "util/mutex.h"

namespace shale {

InfoLog::InfoLog(Env* env, const std::string& dbname) : env_(env) {
  const std::string current = InfoLogFileName(dbname);
  if (env_->FileExists(current)) {
    // A failure to keep the old log is no reason to lose the new one: the file is overwritten.
    static_cast<void>(env_->RenameFile(current, OldInfoLogFileName(dbname)));
  }
  std::unique_ptr<WritableFile> file;
  if (env_->NewWritableFile(current, &file).ok()) {
    MutexLock lock(&mu_);
    file_ = std::move(file);
  }
}

InfoLog::~InfoLog() {
  MutexLock lock(&mu_);
  if (file_ != nullptr) {
    static_cast<void>(file_->Close());
  }
}

void InfoLog::Log(std::string_view line) {
  // The time is read inside the lock, so that the lines of the file are in the order of the clock.
  MutexLock lock(&mu_);
  if (file_ == nullptr) {
    return;
  }
  const uint64_t micros = env_->NowMicros();
  std::string text = std::to_string(micros / 1000000);
  const std::string fraction = std::to_string(micros % 1000000);
  text.push_back('.');
  text.append(6 - fraction.size(), '0');
  text.append(fraction);
  text.push_back(' ');
  text.append(line);
  text.push_back('\n');
  if (!file_->Append(text).ok() || !file_->Flush().ok()) {
    // Nothing is valid on this handle but Close(), and nobody to tell.
    static_cast<void>(file_->Close());
    file_.reset();
  }
}

}  // namespace shale
