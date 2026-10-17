#include "db/compaction_job.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/compaction_filter.h"
#include "core/compaction_picker.h"
#include "core/version_edit.h"
#include "core/version_state.h"
#include "db/builder.h"
#include "db/named_status.h"
#include "db/table_cache.h"
#include "db/version_set.h"
#include "io/merging_iterator.h"
#include "io/table.h"
#include "io/table_builder.h"
#include "shale/env.h"
#include "shale/iterator.h"
#include "shale/status.h"
#include "util/check.h"
#include "util/dbformat.h"
#include "util/filename.h"

namespace shale {

namespace {

uint64_t SaturatingAdd(uint64_t a, uint64_t b) {
  return b > std::numeric_limits<uint64_t>::max() - a ? std::numeric_limits<uint64_t>::max()
                                                      : a + b;
}

// An iterator over one table that says which table it is over, and that does not pass on an entry
// that is no internal key. A corruption found in a block names no file, and by the time it has come
// up through the merge, the compaction and the background task, the person reading it has nothing
// else to tell which of the inputs it was. An entry whose key is too short to hold its tag or names
// a type there is not would abort the merging iterator's comparison, or the filter: here it ends
// the iteration, as damage in the table, and the iterator stays ended until it is positioned again.
class NamedTableIterator final : public Iterator {
 public:
  NamedTableIterator(std::unique_ptr<Iterator> inner, std::string fname)
      : inner_(std::move(inner)), fname_(std::move(fname)) {}

  bool Valid() const override { return !malformed_ && inner_->Valid(); }

  void SeekToFirst() override {
    malformed_ = false;
    inner_->SeekToFirst();
    Check();
  }

  void Seek(std::string_view target) override {
    malformed_ = false;
    inner_->Seek(target);
    Check();
  }

  void Next() override {
    SHALE_CHECK(Valid(), "NamedTableIterator::Next requires Valid");
    inner_->Next();
    Check();
  }

  std::string_view key() const override { return inner_->key(); }
  std::string_view value() const override { return inner_->value(); }

  Status status() const override {
    if (malformed_) {
      return Status::Corruption("an entry whose key is not an internal key", fname_);
    }
    Status s = inner_->status();
    if (s.ok()) {
      return s;
    }
    // A table that failed to open is already named, by the cache that opened it.
    if (s.ToString().find(fname_) != std::string::npos) {
      return s;
    }
    return WithFileName(s, fname_);
  }

 private:
  // Looks at the entry the iterator has just been moved to.
  void Check() {
    ParsedInternalKey parsed;
    malformed_ = inner_->Valid() && !ParseInternalKey(inner_->key(), &parsed);
  }

  const std::unique_ptr<Iterator> inner_;
  const std::string fname_;
  bool malformed_ = false;
};

// One output table while it is being written: the file, the builder over it, and what the file is
// once it is finished. A builder that is dropped before Finish() or Abandon() ends the process, so
// the destructor discards whatever was not finished, and every way out of the scope that holds one,
// an early return among them, is covered.
class OutputFile {
 public:
  OutputFile(const CompactionJobContext* ctx, uint64_t number)
      : ctx_(ctx), number_(number), fname_(TableFileName(ctx->dbname, number)) {}

  ~OutputFile() {
    if (!ended_) {
      Discard();
    }
  }

  OutputFile(const OutputFile&) = delete;
  OutputFile& operator=(const OutputFile&) = delete;

  // Creates the file. On failure nothing was created and there is nothing to discard.
  Status Open() {
    Status s = ctx_->env->NewWritableFile(fname_, &file_);
    if (!s.ok()) {
      ended_ = true;
      return s;
    }
    builder_ = std::make_unique<TableBuilder>(ctx_->table_options, file_.get());
    return Status::OK();
  }

  void Add(std::string_view key, std::string_view value) {
    if (num_entries_ == 0) {
      smallest_.assign(key);
    }
    largest_.assign(key);
    num_entries_++;
    builder_->Add(key, value);
  }

  // The first error the file gave, which stops the builder: the entries after it go nowhere.
  Status status() const { return builder_->status(); }

  // The bytes handed to the file so far, which is the size once the table is finished, less the
  // block that is still being filled and what follows the last block.
  uint64_t Size() const { return builder_->FileSize(); }

  // Finishes the table and makes it whole, durable and named, as SealTableFile() does for every
  // table: the file is synced and closed, then opened through the table cache to see that it is a
  // table that can be read, and only then is the directory synced. On any failure the file is
  // removed.
  Status Finish(FileMetaData* meta) {
    SHALE_CHECK(!ended_, "OutputFile::Finish: the output has already ended");
    ended_ = true;
    Status built = builder_->Finish();  // ends the builder, with an error too
    const uint64_t size = builder_->FileSize();
    Status s = SealTableFile(ctx_->dbname, ctx_->env, ctx_->table_cache, number_, size,
                             std::move(built), std::move(file_));
    if (!s.ok()) {
      return s;
    }
    meta->number = number_;
    meta->file_size = size;
    meta->smallest = std::move(smallest_);
    meta->largest = std::move(largest_);
    return Status::OK();
  }

 private:
  // Gives up the table: the builder is abandoned, the file closed and removed.
  void Discard() {
    ended_ = true;
    builder_->Abandon();
    static_cast<void>(file_->Close());
    file_.reset();
    ctx_->table_cache->Evict(number_);
    static_cast<void>(ctx_->env->RemoveFile(fname_));
  }

  const CompactionJobContext* const ctx_;
  const uint64_t number_;
  const std::string fname_;
  std::unique_ptr<WritableFile> file_;
  // Declared after the file, which it writes to, so that it is destroyed first.
  std::unique_ptr<TableBuilder> builder_;
  std::string smallest_;
  std::string largest_;
  uint64_t num_entries_ = 0;
  bool ended_ = false;  // Finish() has run or the output was discarded
};

}  // namespace

CompactionJob::CompactionJob(CompactionJobContext ctx, CompactionPlan plan,
                             std::shared_ptr<const Version> input_version,
                             std::vector<SequenceNumber> snapshots)
    : ctx_(std::move(ctx)),
      plan_(std::move(plan)),
      input_version_(std::move(input_version)),
      snapshots_(std::move(snapshots)) {
  SHALE_CHECK(ctx_.env != nullptr && ctx_.table_cache != nullptr,
              "CompactionJob needs an Env and a table cache");
  SHALE_CHECK(ctx_.allocate_file_number != nullptr, "CompactionJob needs a way to number files");
  SHALE_CHECK(input_version_ != nullptr, "CompactionJob needs the version of its inputs");
}

Status CompactionJob::Run() {
  SHALE_CHECK(!started_, "CompactionJob::Run is called once");
  started_ = true;
  const Status s = RunImpl();
  if (!s.ok() || stopped_) {
    RemoveOutputs();
  }
  return s;
}

Status CompactionJob::RunImpl() {
  // The tables are read to be written again, so a block that fails its checksum is an error and
  // not a copy of the damage, and they are read once, so they are not worth a place in the block
  // cache.
  TableReadOptions read_options;
  read_options.verify_checksums = true;
  read_options.fill_cache = false;

  // The inputs, in the order of their priority when two hold the same internal key: the files of
  // `level` before those of the level below, which are older, and at level 0, whose files overlap,
  // the newest first. Below level 0 the files of a level are disjoint and their order is free.
  std::vector<const FileMetaData*> inputs;
  if (plan_.level == 0) {
    for (auto it = plan_.inputs[0].rbegin(); it != plan_.inputs[0].rend(); ++it) {
      inputs.push_back(&*it);
    }
  } else {
    for (const FileMetaData& file : plan_.inputs[0]) {
      inputs.push_back(&file);
    }
  }
  for (const FileMetaData& file : plan_.inputs[1]) {
    inputs.push_back(&file);
  }
  std::vector<std::unique_ptr<Iterator>> children;
  children.reserve(inputs.size());
  for (const FileMetaData* file : inputs) {
    stats_.bytes_read = SaturatingAdd(stats_.bytes_read, file->file_size);
    children.push_back(std::make_unique<NamedTableIterator>(
        ctx_.table_cache->NewIterator(read_options, file->number, file->file_size),
        TableFileName(ctx_.dbname, file->number)));
  }
  const std::unique_ptr<Iterator> input =
      NewMergingIterator(std::move(children), CompareInternalKey);

  CompactionFilter filter(snapshots_);
  const VersionState& state = input_version_->state();
  const int output_level = plan_.level + 1;
  const std::vector<FileMetaData>& grandparents = plan_.grandparents;
  const uint64_t max_grandparent_overlap = MaxGrandparentOverlapBytes(ctx_.target_file_size);

  std::unique_ptr<OutputFile> output;
  std::string last_user_key;
  bool has_last_user_key = false;
  // The grandparents the stream has passed, and the bytes of those since this output began.
  size_t grandparent_index = 0;
  uint64_t overlapped_bytes = 0;
  bool seen_key = false;

  const auto finish_output = [&]() -> Status {
    FileMetaData meta;
    Status s = output->Finish(&meta);
    output.reset();
    if (!s.ok()) {
      return s;
    }
    stats_.bytes_written = SaturatingAdd(stats_.bytes_written, meta.file_size);
    outputs_.push_back(std::move(meta));
    if (ctx_.between_outputs) {
      ctx_.between_outputs();
    }
    return Status::OK();
  };

  for (input->SeekToFirst(); input->Valid(); input->Next()) {
    // Before the entry reaches the filter, which aborts on input that is out of order: a child that
    // has found damage, in the order of its keys or elsewhere, is not skipped over, and the entries
    // that follow it are not trusted.
    Status s = input->status();
    if (!s.ok()) {
      return s;
    }
    ParsedInternalKey parsed;
    if (!ParseInternalKey(input->key(), &parsed)) {
      // The iterators over the tables end the iteration at such an entry, naming the table, so
      // this is not reached; it is a Corruption and not an abort for the sake of whoever changes
      // that.
      return Status::Corruption("a compaction input has an entry whose key is not an internal key");
    }
    // The filter asks the question of a deletion only: it drops one that nothing below can need,
    // and keeps every other entry that a reader can see. A search of the levels for an entry it
    // would not look at is the cost of a lookup in each level for every value of the input.
    const bool is_base_level = parsed.type == ValueType::kDeletion &&
                               IsBaseLevelForKey(state, output_level, parsed.user_key);
    if (filter.Next(parsed, is_base_level) == CompactionFilter::Decision::kDrop) {
      continue;
    }

    // The files two levels down that the entries so far have passed. What an output overlaps of
    // them is what the next compaction of its level will have to merge it with.
    while (grandparent_index < grandparents.size() &&
           BytewiseCompare(parsed.user_key,
                           ExtractUserKey(grandparents[grandparent_index].largest)) > 0) {
      if (seen_key) {
        overlapped_bytes =
            SaturatingAdd(overlapped_bytes, grandparents[grandparent_index].file_size);
      }
      grandparent_index++;
    }
    seen_key = true;

    // The output is closed before an entry that begins a new user key and finds it big enough, or
    // over too much of the level below, and never between the entries of one user key: the levels
    // below 0 hold disjoint ranges, and two files that share a user key overlap.
    const bool new_user_key = !has_last_user_key || parsed.user_key != last_user_key;
    if (output != nullptr && new_user_key &&
        (output->Size() >= ctx_.target_file_size || overlapped_bytes > max_grandparent_overlap)) {
      s = finish_output();
      if (!s.ok()) {
        return s;
      }
      overlapped_bytes = 0;
    }
    if (output == nullptr) {
      if (ctx_.should_stop && ctx_.should_stop()) {
        stopped_ = true;
        return Status::OK();  // Run() removes what was made
      }
      const uint64_t number = ctx_.allocate_file_number();
      if (number >= kMaxFileNumber) {
        return Status::Corruption("no file number is left below the largest there can be");
      }
      output = std::make_unique<OutputFile>(&ctx_, number);
      s = output->Open();
      if (!s.ok()) {
        return s;
      }
    }
    output->Add(input->key(), input->value());
    s = output->status();
    if (!s.ok()) {
      return s;
    }
    if (new_user_key) {
      last_user_key.assign(parsed.user_key);
      has_last_user_key = true;
    }
  }
  // The iterator may be invalid because it failed, which is not the end of the input.
  Status s = input->status();
  if (!s.ok()) {
    return s;
  }
  if (output != nullptr) {
    s = finish_output();
    if (!s.ok()) {
      return s;
    }
  }
  stats_.entries_kept = filter.stats().kept;
  stats_.shadowed_dropped = filter.stats().shadowed_dropped;
  stats_.tombstones_dropped = filter.stats().tombstones_dropped;
  return Status::OK();
}

VersionEdit CompactionJob::MakeEdit() const {
  VersionEdit edit;
  for (size_t i = 0; i < plan_.inputs.size(); i++) {
    const int level = plan_.level + static_cast<int>(i);
    for (const FileMetaData& file : plan_.inputs[i]) {
      edit.deleted_files.emplace(level, file.number);
    }
  }
  for (const FileMetaData& file : outputs_) {
    edit.new_files.emplace_back(plan_.level + 1, file);
  }
  edit.compact_pointers.emplace_back(plan_.level, plan_.next_compact_pointer);
  return edit;
}

void CompactionJob::RemoveOutputs() {
  for (const FileMetaData& file : outputs_) {
    ctx_.table_cache->Evict(file.number);
    static_cast<void>(ctx_.env->RemoveFile(TableFileName(ctx_.dbname, file.number)));
  }
  outputs_.clear();
}

}  // namespace shale
