// The write path: writers queue, and the one at the front writes for all of those behind it.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <string_view>

#include "db/db_impl.h"
#include "io/log_writer.h"
#include "io/memtable.h"
#include "io/write_batch_internal.h"
#include "shale/env.h"
#include "shale/options.h"
#include "shale/status.h"
#include "shale/write_batch.h"
#include "util/check.h"
#include "util/dbformat.h"
#include "util/mutex.h"

namespace shale {

namespace {

// The most a group may hold when its leader's batch is bigger than kSmallLeader.
constexpr size_t kMaxGroupSize = size_t{1} << 20;

// A leader this small, or smaller, takes followers up to its own size and kSmallLeaderHeadroom
// more, which is under kMaxGroupSize: a write that is small is not made to wait for the disk to
// take a large one that queued after it.
constexpr size_t kSmallLeader = size_t{128} << 10;
constexpr size_t kSmallLeaderHeadroom = size_t{128} << 10;

}  // namespace

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
  if (WriteBatchInternal::Count(updates) == 0) {
    return Status::OK();
  }

  Writer w(&mutex_);
  w.batch = updates;
  w.sync = options.sync;
  writers_.push_back(&w);
  // Until a leader has written this batch, or this writer is the one at the front.
  while (!w.done && &w != writers_.front()) {
    w.cv.Wait();
  }
  if (w.done) {
    return w.status;
  }

  // This writer leads. Room first, and the group after: making room may give up the mutex while it
  // sleeps or waits, and the writers that queue meanwhile are in the group that is made after it.
  Writer* last_writer = &w;
  Status status = MakeRoomForWrite(/*force=*/false);
  if (status.ok()) {
    status = CommitGroup(&w, &last_writer);
  }
  FinishGroup(&w, last_writer, status);
  return status;
}

WriteBatch* DBImpl::BuildBatchGroup(Writer** last_writer, WriteBatch* merged, uint64_t room) {
  Writer* const first = writers_.front();
  WriteBatch* result = first->batch;
  size_t size = WriteBatchInternal::ByteSize(first->batch);
  const size_t max_size = size <= kSmallLeader ? size + kSmallLeaderHeadroom : kMaxGroupSize;
  uint64_t operations = WriteBatchInternal::Count(first->batch);
  *last_writer = first;
  for (auto it = std::next(writers_.begin()); it != writers_.end(); ++it) {
    Writer* const w = *it;
    if (w->batch == nullptr) {
      break;  // a request to switch the memtable leads for itself
    }
    if (w->sync && !first->sync) {
      break;  // the leader will not sync, and the record is acknowledged when it is written
    }
    const size_t w_size = WriteBatchInternal::ByteSize(w->batch);
    if (size + w_size > max_size) {
      break;
    }
    const uint64_t w_operations = WriteBatchInternal::Count(w->batch);
    if (operations + w_operations > room) {
      break;
    }
    if (result == first->batch) {
      // The first follower: the leader's batch is copied, and the leader's own is left as it is.
      *merged = *first->batch;
      result = merged;
    }
    WriteBatchInternal::Append(result, w->batch);
    size += w_size;
    operations += w_operations;
    *last_writer = w;
  }
  return result;
}

Status DBImpl::CommitGroup(Writer* leader, Writer** last_writer) {
  const SequenceNumber last = versions_.LastSequence();
  // `last` is at most kMaxSequenceNumber: a MANIFEST that says more is refused when it is read. A
  // batch that does not fit is refused too, instead of logged, since what a replay of it would do
  // is refuse it, and the last sequence number the MANIFEST then recorded is one that no open
  // accepts. The refusal is of this batch: a smaller one may still fit, so it is not an error that
  // stops the writes, and the writers behind it are not in its group.
  const uint64_t room = kMaxSequenceNumber - last;
  if (WriteBatchInternal::Count(leader->batch) > room) {
    *last_writer = leader;
    return Status::Corruption("no sequence numbers are left below the largest there can be");
  }

  WriteBatch merged;
  WriteBatch* const group = BuildBatchGroup(last_writer, &merged, room);
  // Every batch of the group is numbered after the one before it, in the order they queued, and
  // each keeps its own number, as it does when it is written alone. The group's own is the first.
  SequenceNumber next = last + 1;
  uint64_t user_bytes = 0;
  for (auto it = writers_.begin();; ++it) {
    Writer* const member = *it;
    WriteBatchInternal::SetSequence(member->batch, next);
    next += WriteBatchInternal::Count(member->batch);
    user_bytes += WriteBatchInternal::PayloadBytes(member->batch);
    if (member == *last_writer) {
      break;
    }
  }
  WriteBatchInternal::SetSequence(group, last + 1);
  const SequenceNumber group_last = next - 1;

  // What the leader uses with the mutex given up. Nothing replaces any of it until this group is
  // done: that is the leader's, or a Flush() that has the front of the queue after it.
  MemTable* const mem = mem_.get();
  log::Writer* const wal = log_.get();
  WritableFile* const wal_file = logfile_.get();
  const std::function<void()> hook = before_wal_write_hook_;
  const bool leader_syncs = leader->sync;

  mutex_.Unlock();
  if (hook) {
    hook();
  }
  // The log first, then the memtable: a write that a reader can see is one that the log holds. A
  // failed append or sync leaves the log in an unknown state, so it stops all writes: a later one
  // would be acknowledged into a log that may not hold it.
  const std::string_view record = WriteBatchInternal::Contents(group);
  const uint64_t wal_before = wal->bytes_written();
  Status s = wal->AddRecord(record);
  // What the file took, headers and padding with the record, and part of it for a record that
  // failed: the cost of the group to the log, whether or not what follows works.
  const uint64_t wal_bytes = wal->bytes_written() - wal_before;
  const bool appended = s.ok();
  bool synced = false;
  if (s.ok() && leader_syncs) {
    s = wal_file->Sync();
    synced = s.ok();
  }
  if (s.ok()) {
    // The batch is a well formed one, made by WriteBatch::Put() and Delete(), and its sequence
    // numbers were checked above, so there is nothing for this to refuse.
    const Status inserted = WriteBatchInternal::InsertInto(group, mem);
    SHALE_CHECK(inserted.ok(),
                "a batch that was logged could not be applied: " + inserted.ToString());
  }
  mutex_.Lock();

  counters_.wal_bytes_written += wal_bytes;
  if (appended) {
    log_unsynced_ = !synced;
    counters_.write_groups++;
  }
  if (synced) {
    counters_.wal_syncs++;
  }
  if (!s.ok()) {
    RecordBackgroundError(s);
    return s;
  }
  counters_.user_bytes_written += user_bytes;
  // After the memtable has all of it: a read that takes the last sequence number sees the whole
  // group or none of it.
  versions_.SetLastSequence(group_last);
  return Status::OK();
}

void DBImpl::FinishGroup(Writer* leader, Writer* last_writer, const Status& status) {
  while (true) {
    Writer* const ready = writers_.front();
    writers_.pop_front();
    if (ready != leader) {
      ready->status = status;
      ready->done = true;
      ready->cv.Signal();
    }
    if (ready == last_writer) {
      break;
    }
  }
  // Whoever is first now leads, and is waiting to be told.
  if (!writers_.empty()) {
    writers_.front()->cv.Signal();
  }
}

}  // namespace shale
