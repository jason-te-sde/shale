# One block per library. shale_add_library() is defined in cmake/ShaleTargets.cmake.

# Mutex and CondVar wrap the standard thread primitives, which need the platform thread library on
# some systems. It is linked publicly because their code lives in a header that users include.
find_package(Threads REQUIRED)

shale_add_library(shale_util
  SOURCES
    util/arena.cc
    util/check.cc
    util/status.cc
  LIBS Threads::Threads)
