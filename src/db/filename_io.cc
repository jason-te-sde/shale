#include "db/filename_io.h"

#include <cstdint>
#include <string>

#include "io/env_util.h"
#include "shale/env.h"
#include "shale/status.h"
#include "util/filename.h"

namespace shale {

Status SyncDirectory(Env* env, const std::string& dbname) { return env->SyncDir(dbname); }

Status SetCurrentFile(Env* env, const std::string& dbname, uint64_t descriptor_number) {
  // CURRENT names the MANIFEST by its bare name: it sits in the directory it points into, and a
  // path would stop being right if the directory were moved.
  const std::string manifest = DescriptorFileName(dbname, descriptor_number);
  std::string contents = manifest.substr(dbname.size() + 1);
  contents.push_back('\n');

  // The temporary file is named after the MANIFEST, a number that no other file of the directory
  // has, so two switches never write to one name. WriteStringToFile() removes it if it fails.
  const std::string temp = TempFileName(dbname, descriptor_number);
  Status s = WriteStringToFile(env, contents, temp, /*sync=*/true);
  if (!s.ok()) {
    return s;
  }
  s = env->RenameFile(temp, CurrentFileName(dbname));
  if (!s.ok()) {
    // The failure to report is the rename's; a failure to clean up after it must not replace it.
    static_cast<void>(env->RemoveFile(temp));
    return s;
  }
  return SyncDirectory(env, dbname);
}

}  // namespace shale
