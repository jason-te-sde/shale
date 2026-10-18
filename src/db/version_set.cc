#include "db/version_set.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/version_edit.h"
#include "core/version_state.h"
#include "db/filename_io.h"
#include "db/named_status.h"
#include "db/table_cache.h"
#include "io/env_util.h"
#include "io/iterator_util.h"
#include "io/log_reader.h"
#include "io/log_writer.h"
#include "io/table.h"
#include "io/two_level_iterator.h"
#include "shale/env.h"
#include "shale/iterator.h"
#include "shale/status.h"
#include "util/check.h"
#include "util/coding.h"
#include "util/dbformat.h"
#include "util/filename.h"

namespace shale {

namespace {

// An iterator over the files of one level below 0, for a two-level iterator to read them one after
// another. Its key for a file is the file's largest key, which is what a two-level iterator needs
// of an index: no less than any key in what it names. Its value is the file's number and size, as
// two fixed64.
//
// It points into the state of the Version, which the caller of Version::AddIterators() keeps.
class LevelFileIterator final : public Iterator {
 public:
  explicit LevelFileIterator(const std::vector<FileMetaData>* files)
      : files_(files), index_(files->size()) {}

  bool Valid() const override { return index_ < files_->size(); }

  void SeekToFirst() override { Position(0); }

  // The first file that can hold `target` or a larger key, which is the first whose largest key is
  // not before it.
  void Seek(std::string_view target) override { Position(FindFile(*files_, target)); }

  void Next() override {
    SHALE_CHECK(Valid(), "LevelFileIterator::Next requires Valid");
    Position(index_ + 1);
  }

  std::string_view key() const override {
    SHALE_CHECK(Valid(), "LevelFileIterator::key requires Valid");
    return (*files_)[index_].largest;
  }

  std::string_view value() const override {
    SHALE_CHECK(Valid(), "LevelFileIterator::value requires Valid");
    return value_;
  }

  Status status() const override { return Status::OK(); }

 private:
  void Position(size_t index) {
    index_ = index;
    if (Valid()) {
      const FileMetaData& file = (*files_)[index_];
      value_.clear();
      PutFixed64(&value_, file.number);
      PutFixed64(&value_, file.file_size);
    }
  }

  const std::vector<FileMetaData>* const files_;
  size_t index_;       // files_->size() when not valid
  std::string value_;  // the current file's number and size
};

// An iterator over the entries of the files of one level below 0, in order, which opens each file
// as the iteration reaches it and holds only that one.
std::unique_ptr<Iterator> NewLevelIterator(const std::vector<FileMetaData>* files,
                                           const TableReadOptions& options,
                                           TableCache* table_cache) {
  return NewTwoLevelIterator(
      std::make_unique<LevelFileIterator>(files),
      [options, table_cache](std::string_view file_value) -> std::unique_ptr<Iterator> {
        uint64_t number = 0;
        uint64_t file_size = 0;
        if (!GetFixed64(&file_value, &number) || !GetFixed64(&file_value, &file_size) ||
            !file_value.empty()) {
          return NewErrorIterator(Status::Corruption("a level's file index entry is malformed"));
        }
        return table_cache->NewIterator(options, number, file_size);
      });
}

// Keeps the first thing the log reader reports. A report is a defect in the MANIFEST, and none is
// benign: the reader drops the rest of a block after one, and what it drops may be edits that were
// acknowledged.
class FirstReport final : public log::Reader::Reporter {
 public:
  void Corruption(size_t /*bytes*/, const Status& status) override {
    if (status_.ok()) {
      status_ = status;
    }
  }

  const Status& status() const { return status_; }

 private:
  Status status_;
};

}  // namespace

Version::Version(VersionState state, TableCache* table_cache)
    : state_(std::move(state)), table_cache_(table_cache) {}

Status Version::Get(const TableReadOptions& options, const LookupKey& key, std::string* value,
                    int* files_probed) const {
  int probed = 0;
  Status status;
  // Looks in one file. Returns true when the lookup is decided, with `status` the answer: the
  // value was found, the key was deleted, or the file could not be read.
  const auto consult = [&](const FileMetaData& file) {
    probed++;
    TableGetResult result;
    status = table_cache_->Get(options, file.number, file.file_size, key.internal_key(), &result);
    if (!status.ok()) {
      return true;
    }
    switch (result.kind) {
      case TableGetResult::Kind::kFound:
        *value = std::move(result.value);
        return true;
      case TableGetResult::Kind::kDeleted:
        status = Status::NotFound("key deleted");
        return true;
      case TableGetResult::Kind::kNotFound:
        return false;
    }
    return false;
  };
  const auto finish = [&](const Status& result) {
    if (files_probed != nullptr) {
      *files_probed = probed;
    }
    return result;
  };

  const std::string_view user_key = key.user_key();

  // Level 0: the files overlap, so any of them may hold the key, and a later one holds the newer
  // entries. The state keeps them by number, oldest first, and a higher number is a later flush.
  const std::vector<FileMetaData>& level0 = state_.files[0];
  for (auto it = level0.rbegin(); it != level0.rend(); ++it) {
    if (BytewiseCompare(user_key, ExtractUserKey(it->smallest)) < 0 ||
        BytewiseCompare(user_key, ExtractUserKey(it->largest)) > 0) {
      continue;
    }
    if (consult(*it)) {
      return finish(status);
    }
  }

  // The other levels: the files are disjoint, so the only one that can hold the key is the first
  // whose largest key is not before it, if its smallest key is not after.
  for (size_t level = 1; level < state_.files.size(); level++) {
    const std::vector<FileMetaData>& files = state_.files[level];
    const size_t index = FindFile(files, key.internal_key());
    if (index == files.size() ||
        BytewiseCompare(user_key, ExtractUserKey(files[index].smallest)) < 0) {
      continue;
    }
    if (consult(files[index])) {
      return finish(status);
    }
  }
  return finish(Status::NotFound("key not found"));
}

void Version::AddIterators(const TableReadOptions& options,
                           std::vector<std::unique_ptr<Iterator>>* iters) const {
  const std::vector<FileMetaData>& level0 = state_.files[0];
  for (auto it = level0.rbegin(); it != level0.rend(); ++it) {
    iters->push_back(table_cache_->NewIterator(options, it->number, it->file_size));
  }
  for (size_t level = 1; level < state_.files.size(); level++) {
    if (!state_.files[level].empty()) {
      iters->push_back(NewLevelIterator(&state_.files[level], options, table_cache_));
    }
  }
}

int Version::NumFiles(int level) const {
  SHALE_CHECK(level >= 0 && level < kNumLevels, "Version::NumFiles: level is out of range");
  return static_cast<int>(state_.files[static_cast<size_t>(level)].size());
}

VersionSet::VersionSet(std::string dbname, Env* env, TableCache* table_cache)
    : dbname_(std::move(dbname)),
      env_(env),
      table_cache_(table_cache),
      current_(std::make_shared<const Version>(VersionState{}, table_cache)),
      next_file_number_(VersionState{}.next_file_number) {
  versions_.push_back(current_);
}

VersionSet::~VersionSet() {
  if (manifest_file_ != nullptr) {
    manifest_log_.reset();
    // Nothing is waiting in the file: every record was synced as it was appended. A failure here
    // has nothing to lose, and nobody to tell.
    static_cast<void>(manifest_file_->Close());
  }
}

Status VersionSet::OpenNewManifest(uint64_t number, const VersionEdit& snapshot) {
  const std::string fname = DescriptorFileName(dbname_, number);
  std::unique_ptr<WritableFile> file;
  Status s = env_->NewWritableFile(fname, &file);
  if (!s.ok()) {
    return s;
  }
  auto writer = std::make_unique<log::Writer>(file.get());
  std::string record;
  snapshot.EncodeTo(&record);
  s = writer->AddRecord(record);
  if (s.ok()) {
    s = file->Sync();
  }
  if (s.ok()) {
    // The file's bytes are durable, and its directory entry is not until now. CURRENT must not
    // name a MANIFEST whose entry a crash could still take back, and that is not a promise to be
    // had from the order the directory's changes are kept in: one sync for each is cheap, and a
    // MANIFEST is started when a database is opened.
    s = SyncDirectory(env_, dbname_);
  }
  if (!s.ok()) {
    // CURRENT has not been touched, so nothing names this file and it is of no use to anyone.
    writer.reset();
    static_cast<void>(file->Close());
    file.reset();
    static_cast<void>(env_->RemoveFile(fname));
    return s;
  }

  s = SetCurrentFile(env_, dbname_, number);
  if (!s.ok()) {
    writer.reset();
    static_cast<void>(file->Close());
    file.reset();
    if (CurrentMayNameNewManifest()) {
      // The rename happened, and the directory sync that failed was what would have made it
      // durable, so CURRENT names this file now and may after a crash, or it names the old one.
      // Both files are kept, and nothing more is written to either until a switch succeeds.
      manifest_error_ = s;
    } else {
      // CURRENT was not changed, so nothing names this file and it is of no use to anyone. The
      // version set goes on with the MANIFEST it has.
      static_cast<void>(env_->RemoveFile(fname));
    }
    return s;
  }

  if (manifest_file_ != nullptr) {
    manifest_log_.reset();
    static_cast<void>(manifest_file_->Close());  // superseded: what it held is in the snapshot
  }
  manifest_file_ = std::move(file);
  manifest_log_ = std::move(writer);
  manifest_file_number_ = number;
  manifest_error_ = Status::OK();
  return Status::OK();
}

bool VersionSet::CurrentMayNameNewManifest() {
  const std::string current_name = CurrentFileName(dbname_);
  if (!env_->FileExists(current_name)) {
    return false;
  }
  std::string contents;
  if (!ReadFileToString(env_, current_name, &contents).ok()) {
    return true;  // not known, and deleting the file that CURRENT names would be the worst answer
  }
  // Only what proves that the rename did not happen lets the new file go: CURRENT is read to be
  // the contents that name the MANIFEST that was open before it. A read that matches nothing is not
  // proof of that, since a read can come back wrong, and what the rename that went through made of
  // it cannot be undone by a read that is wrong: the new file stays, and a later sweep deletes it
  // if CURRENT turns out not to name it.
  if (manifest_file_number_ != 0) {
    return contents != CurrentFileContents(dbname_, manifest_file_number_);
  }
  return true;
}

void VersionSet::Install(VersionState state) {
  auto version = std::make_shared<const Version>(std::move(state), table_cache_);
  std::erase_if(versions_, [](const std::weak_ptr<const Version>& v) { return v.expired(); });
  versions_.push_back(version);
  current_ = std::move(version);
}

Status VersionSet::CreateNew() {
  SHALE_CHECK(!loaded_, "VersionSet::CreateNew: this version set is already loaded");
  if (!manifest_error_.ok()) {
    // An earlier CreateNew() failed after CURRENT may have come to name MANIFEST-000001, and
    // writing it again would empty a file that CURRENT names. The database is whole or it is not
    // there, and which is for a Recover() to find out.
    return manifest_error_;
  }
  const VersionState empty;
  constexpr uint64_t kFirstManifest = 1;
  Status s = OpenNewManifest(kFirstManifest, SnapshotEdit(empty));
  if (!s.ok()) {
    return s;
  }
  // The version set already holds the empty state, and the counters are those of its snapshot.
  loaded_ = true;
  return Status::OK();
}

Status VersionSet::Recover() {
  SHALE_CHECK(!loaded_, "VersionSet::Recover is called once, on a version set not loaded yet");

  std::string current;
  Status s = ReadFileToString(env_, CurrentFileName(dbname_), &current);
  if (!s.ok()) {
    return s;
  }
  uint64_t number = 0;
  s = ParseCurrentFile(dbname_, current, &number);
  if (!s.ok()) {
    return WithFileName(s, CurrentFileName(dbname_));
  }

  const std::string manifest = DescriptorFileName(dbname_, number);
  std::unique_ptr<SequentialFile> file;
  s = env_->NewSequentialFile(manifest, &file);
  if (!s.ok()) {
    return s;
  }
  FirstReport report;
  log::Reader reader(file.get(), &report, /*checksum=*/true);
  VersionState state;
  size_t records = 0;
  std::string_view record;
  std::string scratch;
  while (reader.ReadRecord(&record, &scratch)) {
    // A defect is reported before the record that follows it is returned, and what follows a gap
    // is not what the history said. Applying it could fail for the gap's sake, with an error about
    // an edit that is intact, so the defect is what is reported.
    if (!report.status().ok()) {
      break;
    }
    VersionEdit edit;
    s = edit.DecodeFrom(record);
    if (s.ok()) {
      s = ApplyEdit(state, edit, &state);
    }
    if (!s.ok()) {
      return WithFileName(s, manifest);
    }
    records++;
  }
  // The end of the log is silent only when the file stops short, as a crash leaves it. A report is
  // a defect, and the records after it were dropped with the rest of its block, so a recovery that
  // carried on from here would have lost edits without saying so.
  if (!report.status().ok()) {
    return WithFileName(report.status(), manifest);
  }
  if (records == 0) {
    // Nothing recovered from an empty file is not "an empty database": it is a database whose
    // tables are all there and not named, and the first flush would write over their numbers.
    return WithFileName(Status::Corruption("the MANIFEST holds no record"), manifest);
  }
  if (state.last_sequence > kMaxSequenceNumber) {
    return WithFileName(Status::Corruption("last_sequence is above the largest sequence number"),
                        manifest);
  }

  // The numbers a database accepts stop at kMaxFileNumber: a MANIFEST numbered at it or above, or a
  // counter above it, is forged or damaged, and what it would leave is a counter within a few files
  // of the end of the 64 bits. The counter is kept at or below the ceiling from here on, so the
  // MANIFEST that a switch writes is one that this accepts.
  if (number >= kMaxFileNumber || state.next_file_number > kMaxFileNumber) {
    return WithFileName(Status::Corruption("a file number is beyond the largest there can be"),
                        manifest);
  }
  // ApplyEdit() has seen to it that every file and the log are numbered below next_file_number. The
  // MANIFEST that was read is the one number it knows nothing of, and numbers given out from here
  // on must be above it.
  const uint64_t next = std::max(state.next_file_number, number + 1);
  next_file_number_ = next;
  last_sequence_ = state.last_sequence;
  manifest_file_number_ = number;
  loaded_ = true;
  Install(std::move(state));
  return Status::OK();
}

Status VersionSet::WriteNewManifest(VersionEdit edit) {
  SHALE_CHECK(loaded_, "VersionSet::WriteNewManifest needs CreateNew() or Recover() first");
  if (next_file_number_ >= kMaxFileNumber) {
    // The new MANIFEST takes the number the counter is at and the snapshot records the counter one
    // above it, which Recover() accepts up to kMaxFileNumber and no further. A switch that wrote
    // more would be a database that cannot be opened. How the counter got here is data, the
    // numbers of the files in the directory among it, and is no reason to abort.
    return Status::Corruption("no file number is left below the largest there can be");
  }
  // The number is taken first, so that the counter the snapshot carries is above it.
  const uint64_t number = NewFileNumber();
  edit.next_file_number = next_file_number_;
  edit.last_sequence = last_sequence_;
  VersionState next;
  Status s = ApplyEdit(current_->state(), edit, &next);
  if (!s.ok()) {
    return s;
  }
  s = OpenNewManifest(number, SnapshotEdit(next));
  if (!s.ok()) {
    return s;
  }
  Install(std::move(next));
  return Status::OK();
}

Status VersionSet::LogAndApply(VersionEdit edit) {
  SHALE_CHECK(manifest_log_ != nullptr,
              "VersionSet::LogAndApply needs an open MANIFEST: CreateNew() or WriteNewManifest()");
  if (!manifest_error_.ok()) {
    return manifest_error_;
  }
  if (next_file_number_ > kMaxFileNumber) {
    // Only the numbers given out can have taken the counter here, past what Recover() accepts, and
    // writing it would make the database unopenable.
    return Status::Corruption("no file number is left below the largest there can be");
  }
  edit.next_file_number = next_file_number_;
  edit.last_sequence = last_sequence_;
  VersionState next;
  Status s = ApplyEdit(current_->state(), edit, &next);
  if (!s.ok()) {
    return s;
  }

  // Durable first, installed second: a version that readers have seen must be one that a crash
  // leaves in the MANIFEST.
  std::string record;
  edit.EncodeTo(&record);
  s = manifest_log_->AddRecord(record);
#if defined(SHALE_FLAW_SKIP_MANIFEST_SYNC)
  // Flaw skip_manifest_sync: the edit is installed without making the MANIFEST durable.
#else
  if (s.ok()) {
    s = manifest_file_->Sync();
  }
#endif
  if (!s.ok()) {
    // What the file holds is unknown, and a Sync() after a failed one proves nothing. Nothing more
    // goes into it.
    manifest_error_ = s;
    return s;
  }
  Install(std::move(next));
  return Status::OK();
}

uint64_t VersionSet::NewFileNumber() {
  // Nothing read from disk can bring the counter near this: a MANIFEST's counter stops at
  // kMaxFileNumber, which is 2^56 numbers short of it. It is here so that a bug cannot wrap the
  // counter round to numbers that are in use.
  SHALE_CHECK(next_file_number_ != std::numeric_limits<uint64_t>::max(),
              "VersionSet: the file numbers are used up");
  return next_file_number_++;
}

void VersionSet::MarkFileNumberUsed(uint64_t number) {
  // A number at or above the ceiling is not used, so that a file named for one cannot take the
  // counter to where the next MANIFEST is refused. Nothing the database makes is numbered that
  // high, so no file of its own is passed over.
  if (number < kMaxFileNumber && number >= next_file_number_) {
    next_file_number_ = number + 1;
  }
}

void VersionSet::SetLastSequence(SequenceNumber sequence) {
  SHALE_CHECK(sequence >= last_sequence_,
              "VersionSet::SetLastSequence would move the last sequence number back");
  last_sequence_ = sequence;
}

std::set<uint64_t> VersionSet::LiveFiles() {
  std::erase_if(versions_, [](const std::weak_ptr<const Version>& v) { return v.expired(); });
  std::set<uint64_t> live;
  for (const std::weak_ptr<const Version>& weak : versions_) {
    // A version that was dropped since the pruning is no longer there to lock, and is not live.
    const std::shared_ptr<const Version> version = weak.lock();
    if (version == nullptr) {
      continue;
    }
    for (const std::vector<FileMetaData>& level : version->state().files) {
      for (const FileMetaData& file : level) {
        live.insert(file.number);
      }
    }
  }
  return live;
}

}  // namespace shale
