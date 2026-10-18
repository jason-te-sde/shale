#include "db/filename_io.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include "io/env_util.h"
#include "shale/env.h"
#include "shale/status.h"
#include "util/crc32c.h"
#include "util/filename.h"

namespace shale {

namespace {

// The second line of CURRENT: "crc32c:", eight hex digits, a newline.
constexpr const char* kCurrentChecksumPrefix = "crc32c:";
constexpr size_t kCurrentChecksumLine = 7 + 8 + 1;

}  // namespace

Status SyncDirectory(Env* env, const std::string& dbname) {
#if defined(SHALE_FLAW_SKIP_DIR_SYNC)
  // Flaw skip_dir_sync: the directory is reported synced without being synced.
  static_cast<void>(env);
  static_cast<void>(dbname);
  return Status::OK();
#else
  return env->SyncDir(dbname);
#endif
}

std::string CurrentFileContents(const std::string& dbname, uint64_t descriptor_number) {
  // CURRENT names the MANIFEST by its bare name: it sits in the directory it points into, and a
  // path would stop being right if the directory were moved.
  const std::string manifest = DescriptorFileName(dbname, descriptor_number);
  std::string contents = manifest.substr(dbname.size() + 1);
  contents.push_back('\n');
  char checksum[kCurrentChecksumLine + 1];
  std::snprintf(
      checksum, sizeof checksum, "%s%08x\n", kCurrentChecksumPrefix,
      static_cast<unsigned>(crc32c::Mask(crc32c::Value(contents.data(), contents.size()))));
  contents.append(checksum);
  return contents;
}

Status ParseCurrentFile(const std::string& dbname, const std::string& contents, uint64_t* number) {
  if (contents.empty() || contents.back() != '\n') {
    return Status::Corruption("CURRENT file does not end with newline");
  }
  // The name line, newline included, and then exactly the checksum line.
  const size_t newline = contents.find('\n');
  const std::string name_line = contents.substr(0, newline + 1);
  const std::string checksum_line = contents.substr(newline + 1);
  const std::string_view prefix = kCurrentChecksumPrefix;
  bool well_formed = checksum_line.size() == kCurrentChecksumLine &&
                     checksum_line.compare(0, prefix.size(), prefix) == 0;
  uint32_t stored = 0;
  for (size_t i = prefix.size(); well_formed && i + 1 < kCurrentChecksumLine; i++) {
    const char c = checksum_line[i];
    if (c >= '0' && c <= '9') {
      stored = (stored << 4) | static_cast<uint32_t>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      stored = (stored << 4) | static_cast<uint32_t>(c - 'a' + 10);
    } else {
      well_formed = false;
    }
  }
  if (!well_formed) {
    return Status::Corruption("CURRENT file is malformed");
  }
  if (stored != crc32c::Mask(crc32c::Value(name_line.data(), name_line.size()))) {
    return Status::Corruption("CURRENT checksum mismatch");
  }

  std::string name = name_line;
  name.pop_back();
  uint64_t parsed = 0;
  FileType type = FileType::kCurrentFile;
  // The name must be the one SetCurrentFile() writes: a number spelled another way names the same
  // file to the parser and a different one to the directory.
  if (!ParseFileName(name, &parsed, &type) || type != FileType::kDescriptorFile ||
      DescriptorFileName(dbname, parsed) != dbname + "/" + name) {
    return Status::Corruption("CURRENT names an invalid file");
  }
  *number = parsed;
  return Status::OK();
}

Status SetCurrentFile(Env* env, const std::string& dbname, uint64_t descriptor_number) {
  const std::string contents = CurrentFileContents(dbname, descriptor_number);

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
