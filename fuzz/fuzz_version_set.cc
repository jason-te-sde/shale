// Writes the input as CURRENT and a MANIFEST into a MemEnv, and recovers a VersionSet from them.
// The layout of the input, which lets a fuzzer reach the code behind the checksums of both files,
// is in version_set_input.h.
//
// A MANIFEST is read at every open, and everything in it is data: its numbers, which a database
// counts files and writes from, among them. So a recovery has to answer OK or Corruption and never
// abort, whatever the files hold, and what it accepts has to be a state that the engine can go on
// from. Where it answers OK the target goes on from it as the engine does: it is told of a file
// that was found in the directory, starts a new MANIFEST, and applies the edits that the input has
// left, each of which is applied or refused with Corruption. What it writes in doing so must be
// something that a recovery accepts again, with the tables it was installed with: a database that
// wrote a MANIFEST which it then refuses is a database that cannot be opened.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/version_edit.h"
#include "core/version_state.h"
#include "db/filename_io.h"
#include "db/table_cache.h"
#include "db/version_set.h"
#include "fuzz_util.h"
#include "io/env_util.h"
#include "io/log_writer.h"
#include "io/table.h"
#include "shale/env.h"
#include "shale/status.h"
#include "util/dbformat.h"
#include "util/filename.h"
#include "version_set_input.h"

namespace shale {
namespace {

constexpr char kDbName[] = "/db";

// What a VersionSet needs to read tables, which none of the states here has any file for.
struct Tables {
  explicit Tables(Env* env) : cache(size_t{1} << 16), table_cache(kDbName, env, &cache, 20) {}

  BlockCache cache;
  TableCache table_cache;
};

// Writes `records` to `fname` as the log of a MANIFEST.
void WriteRecords(Env* env, const std::string& fname, const std::vector<std::string>& records,
                  size_t count) {
  std::unique_ptr<WritableFile> file;
  SHALE_FUZZ_CHECK(env->NewWritableFile(fname, &file).ok());
  log::Writer writer(file.get());
  for (size_t i = 0; i < records.size() && i < count; i++) {
    SHALE_FUZZ_CHECK(writer.AddRecord(records[i]).ok());
  }
  SHALE_FUZZ_CHECK(file->Close().ok());
}

// What recovery accepts has to be a state that the engine can go on from.
void CheckSound(const VersionSet& versions) {
  const VersionState& state = versions.current()->state();
  SHALE_FUZZ_CHECK(CheckInvariants(state).ok());
  SHALE_FUZZ_CHECK(versions.LastSequence() <= kMaxSequenceNumber);
  SHALE_FUZZ_CHECK(versions.NextFileNumber() <= kMaxFileNumber + 1);
  SHALE_FUZZ_CHECK(versions.ManifestFileNumber() < versions.NextFileNumber());
  SHALE_FUZZ_CHECK(versions.LogNumber() < versions.NextFileNumber());
  for (const auto& level : state.files) {
    for (const FileMetaData& file : level) {
      SHALE_FUZZ_CHECK(file.number < versions.NextFileNumber());
    }
  }
}

// Whether two states have the same tables and the same marks. The counter is left out: a recovery
// puts it above the number of the MANIFEST it read, which the state that wrote that MANIFEST need
// not have been.
bool SameTables(const VersionState& a, const VersionState& b) {
  return a.files == b.files && a.compact_pointers == b.compact_pointers &&
         a.log_number == b.log_number && a.last_sequence == b.last_sequence;
}

// A failure of a call that no fault can fail: the input was refused, which is Corruption.
void CheckRefusal(const Status& s) { SHALE_FUZZ_CHECK(s.IsCorruption()); }

void Run(std::string_view bytes) {
  const fuzz::VersionSetInput in = fuzz::DecodeVersionSetInput(bytes);

  const std::unique_ptr<Env> env = NewMemEnv();
  SHALE_FUZZ_CHECK(env->CreateDir(kDbName).ok());
  const uint64_t number =
      in.current_valid ? fuzz::kManifestNumbers[in.manifest_number_index & 7U] : 1;
  const std::string current = in.current_valid ? CurrentFileContents(kDbName, number) : in.current;
  SHALE_FUZZ_CHECK(WriteStringToFile(env.get(), current, CurrentFileName(kDbName), false).ok());
  const std::string manifest = DescriptorFileName(kDbName, number);
  if (in.framed) {
    WriteRecords(env.get(), manifest, in.records, in.manifest_records);
  } else {
    SHALE_FUZZ_CHECK(WriteStringToFile(env.get(), in.manifest, manifest, false).ok());
  }

  Tables tables(env.get());
  VersionSet versions(kDbName, env.get(), &tables.table_cache);
  const Status recovered = versions.Recover();
  if (!recovered.ok()) {
    CheckRefusal(recovered);
    return;
  }
  CheckSound(versions);

  // A file that was found in the directory, which a database numbers from: its name is data.
  if (in.hostile_index < 8) {
    versions.MarkFileNumberUsed(fuzz::kHostileNumbers[in.hostile_index]);
    CheckSound(versions);
  }

  // What a database does next: starts a new MANIFEST, and then applies edits to it.
  const Status started = versions.WriteNewManifest(VersionEdit{});
  if (started.ok()) {
    CheckSound(versions);
    for (size_t i = in.manifest_records; i < in.records.size(); i++) {
      VersionEdit edit;
      if (!edit.DecodeFrom(in.records[i]).ok()) {
        continue;
      }
      // The files that an edit adds, and the log it names, were numbered by the database before it
      // made the edit, from the counter that this version set keeps.
      if (edit.log_number.has_value()) {
        versions.MarkFileNumberUsed(*edit.log_number);
      }
      for (const auto& [level, file] : edit.new_files) {
        versions.MarkFileNumberUsed(file.number);
      }
      // And the writes that a flush says its table holds were made by the database before it made
      // the edit, which a sequence number beyond the largest no database reaches.
      if (edit.last_sequence.has_value()) {
        if (*edit.last_sequence <= kMaxSequenceNumber) {
          versions.SetLastSequence(std::max(versions.LastSequence(), *edit.last_sequence));
        } else {
          edit.last_sequence.reset();
        }
      }
      const Status applied = versions.LogAndApply(std::move(edit));
      if (!applied.ok()) {
        CheckRefusal(applied);
      }
      CheckSound(versions);
    }
  } else {
    CheckRefusal(started);
  }

  // Whatever was written, a recovery must accept again, and find the tables that are installed: a
  // refused start or edit wrote nothing, and a successful one wrote what CURRENT now names.
  Tables again_tables(env.get());
  VersionSet again(kDbName, env.get(), &again_tables.table_cache);
  SHALE_FUZZ_CHECK(again.Recover().ok());
  CheckSound(again);
  SHALE_FUZZ_CHECK(SameTables(again.current()->state(), versions.current()->state()));
}

}  // namespace
}  // namespace shale

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  shale::Run(std::string_view(reinterpret_cast<const char*>(data), size));
  return 0;
}
