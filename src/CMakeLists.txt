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
    io/env.cc
    io/env_posix.cc
    io/env_util.cc
    io/log_reader.cc
    io/log_writer.cc
    io/mem_env.cc
    io/scheduler.cc
  LIBS shale_util Threads::Threads)
