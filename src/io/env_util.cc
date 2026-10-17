#include "io/env_util.h"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

#include "shale/env.h"
#include "shale/status.h"

namespace shale {

Status WriteStringToFile(Env* env, std::string_view data, const std::string& fname, bool sync) {
  std::unique_ptr<WritableFile> file;
  Status s = env->NewWritableFile(fname, &file);
  if (!s.ok()) {
    return s;
  }
  s = file->Append(data);
  if (s.ok() && sync) {
    s = file->Sync();
  }
  // Close in every case. A handle is only released by Close(), and it is no use after an error.
  const Status close_status = file->Close();
  if (s.ok()) {
    s = close_status;
  }
  file.reset();
  if (!s.ok()) {
    // The file is useless and may be half written. The failure to report is the first one; a
    // failure to clean up after it must not replace it.
    static_cast<void>(env->RemoveFile(fname));
  }
  return s;
}

Status ReadFileToString(Env* env, const std::string& fname, std::string* data) {
  data->clear();
  std::unique_ptr<SequentialFile> file;
  Status s = env->NewSequentialFile(fname, &file);
  if (!s.ok()) {
    return s;
  }
  constexpr size_t kChunkSize = size_t{64} * 1024;
  std::string scratch(kChunkSize, '\0');
  for (;;) {
    std::string_view chunk;
    s = file->Read(kChunkSize, &chunk, scratch.data());
    if (!s.ok()) {
      data->clear();
      return s;
    }
    if (chunk.empty()) {
      return Status::OK();
    }
    data->append(chunk);
  }
}

}  // namespace shale
