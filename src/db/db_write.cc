// The write path: one writer at a time, under the database mutex.

#include <cstdint>
#include <string_view>

#include "db/db_impl.h"
#include "io/write_batch_internal.h"
#include "shale/options.h"
#include "shale/status.h"
#include "shale/write_batch.h"
#include "util/check.h"
#include "util/dbformat.h"
#include "util/mutex.h"

namespace shale {

Status DBImpl::Put(const WriteOptions& options, std::string_view key, std::string_view value) {
  WriteBatch batch;
  batch.Put(key, value);
  return Write(options, &batch);
}

Status DBImpl::Delete(const WriteOptions& options, std::string_view key) {
  WriteBatch batch;
  batch.Delete(key);
  return Write(options, &batch);
}

Status DBImpl::Write(const WriteOptions& options, WriteBatch* updates) {
  if (updates == nullptr) {
    return Status::InvalidArgument("the batch is null");
  }
  // Before anything is touched: a batch with an oversized entry is refused whole, with no sequence
  // number taken and nothing logged.
  if (WriteBatchInternal::HasOversizedEntry(updates)) {
    return Status::InvalidArgument("key or value exceeds the size limit");
  }

  MutexLock lock(&mutex_);
  if (!bg_error_.ok()) {
    return bg_error_;
  }
  const uint32_t count = WriteBatchInternal::Count(updates);
  if (count == 0) {
    return Status::OK();
  }
  const SequenceNumber last = versions_.LastSequence();
  // `last` is at most kMaxSequenceNumber: a MANIFEST that says more is refused when it is read. A
  // batch that does not fit is refused too, instead of logged, since what a replay of it would do
  // is refuse it, and the last sequence number the MANIFEST then recorded is one that no open
  // accepts. The refusal is of this batch: a smaller one may still fit, so it is not an error that
  // stops the writes.
  if (count > kMaxSequenceNumber - last) {
    return Status::Corruption("no sequence numbers are left below the largest there can be");
  }
  const SequenceNumber sequence = last + 1;
  WriteBatchInternal::SetSequence(updates, sequence);

  // The log first, then the memtable: a write that a reader can see is one that the log holds. A
  // failed append or sync leaves the log in an unknown state, so it stops all writes: a later one
  // would be acknowledged into a log that may not hold it.
  Status s = log_->AddRecord(WriteBatchInternal::Contents(updates));
  if (s.ok() && options.sync) {
    s = logfile_->Sync();
  }
  if (!s.ok()) {
    RecordBackgroundError(s);
    return s;
  }
  // The batch is a well formed one, made by WriteBatch::Put() and Delete(), and its sequence
  // numbers were checked above, so there is nothing for this to refuse.
  const Status inserted = WriteBatchInternal::InsertInto(updates, mem_.get());
  SHALE_CHECK(inserted.ok(),
              "a batch that was logged could not be applied: " + inserted.ToString());
  versions_.SetLastSequence(sequence + count - 1);
  return Status::OK();
}

}  // namespace shale
