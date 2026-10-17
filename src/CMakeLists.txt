# One block per library. shale_add_library() is defined in cmake/ShaleTargets.cmake.

# Mutex and CondVar wrap the standard thread primitives, which need the platform thread library on
# some systems. It is linked publicly because their code lives in a header that users include.
find_package(Threads REQUIRED)

shale_add_library(shale_util
  SOURCES
    util/arena.cc
    util/check.cc
    util/coding.cc
    util/crc32c.cc
    util/dbformat.cc
    util/filename.cc
    util/hash.cc
    util/status.cc
  LIBS Threads::Threads)

shale_add_library(shale_io
  SOURCES
    io/block.cc
    io/block_builder.cc
    io/bloom.cc
    io/env.cc
    io/env_posix.cc
    io/env_util.cc
    io/iterator.cc
    io/log_reader.cc
    io/log_writer.cc
    io/mem_env.cc
    io/memtable.cc
    io/merging_iterator.cc
    io/scheduler.cc
    io/table.cc
    io/table_builder.cc
    io/table_format.cc
    io/two_level_iterator.cc
    io/write_batch.cc
  LIBS shale_util Threads::Threads)

# The pure kernel: no files, no clock, no threads, no locks. It links shale_util and nothing else, so
# a core source that reached for the I/O layer would fail to link, not just to pass review.
shale_add_library(shale_core
  SOURCES
    core/compaction_filter.cc
    core/compaction_picker.cc
    core/version_edit.cc
    core/version_state.cc
  LIBS shale_util)

# The database proper: the files a database directory holds and the in-memory view of them. Unlike
# the layers below it, it may use all of them, so it links the kernel and the I/O layer both.
shale_add_library(shale_db
  SOURCES
    db/builder.cc
    db/db_background.cc
    db/db_impl.cc
    db/db_write.cc
    db/filename_io.cc
    db/info_log.cc
    db/named_status.cc
    db/options.cc
    db/table_cache.cc
    db/version_set.cc
  LIBS shale_core shale_io)
