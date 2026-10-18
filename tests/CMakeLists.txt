include(FetchContent)

# The script tests need both. REQUIRED makes a machine without them fail here, at configure time,
# instead of silently running fewer tests.
find_program(SHALE_BASH bash REQUIRED)
find_program(SHALE_GIT git REQUIRED)
message(STATUS "Script tests run with bash ${SHALE_BASH} and git ${SHALE_GIT}")

FetchContent_Declare(googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.15.2
  GIT_SHALLOW TRUE
  SYSTEM)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# GoogleTest compiles with its own warning flags, and newer compilers have opinions about its
# sources. That is not our code to fix, and it must not leave warnings in every build log.
foreach(dependency gtest gtest_main gmock gmock_main)
  if(TARGET ${dependency})
    target_compile_options(${dependency} PRIVATE -w)
  endif()
endforeach()

# Helpers every layer's tests share. A later test-support file is added to SOURCES. The library
# exports the tests/ directory so a test can include "testkit/test_util.h". It links shale_io, which
# the fault-injecting Env implements, and shale_core, whose version rules the invariant checkers
# apply, and passes both on: a test that links shale_testkit does not name them too, or the linker
# reports the library twice. It does not link shale_db: the checkers see a database only through the
# DB interface, so a test that needs the engine itself names shale_db, as the simulator's do.
shale_add_library(shale_testkit
  SOURCES
    testkit/fault_env.cc
    testkit/invariants.cc
    testkit/model_db.cc
    testkit/test_util.cc
  LIBS shale_core shale_io shale_util GTest::gtest)
target_include_directories(shale_testkit PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

shale_add_test(status SOURCES util/status_test.cc LIBS shale_util)
shale_add_test(arena SOURCES util/arena_test.cc LIBS shale_util)
shale_add_test(random SOURCES util/random_test.cc LIBS shale_util)
shale_add_test(check SOURCES util/check_test.cc LIBS shale_util)
shale_add_test(mutex SOURCES util/mutex_test.cc LIBS shale_util)
shale_add_test(coding SOURCES util/coding_test.cc LIBS shale_util)
shale_add_test(crc32c SOURCES util/crc32c_test.cc LIBS shale_util)
shale_add_test(hash SOURCES util/hash_test.cc LIBS shale_util)
shale_add_test(dbformat SOURCES util/dbformat_test.cc LIBS shale_util)
shale_add_test(filename SOURCES util/filename_test.cc LIBS shale_util)

shale_add_test(env_contract SOURCES io/env_contract_test.cc LIBS shale_testkit)
shale_add_test(env_posix SOURCES io/env_posix_test.cc LIBS shale_testkit)
shale_add_test(exclusive_use SOURCES io/exclusive_use_test.cc LIBS shale_io)
shale_add_test(mem_env SOURCES io/mem_env_test.cc LIBS shale_testkit)
shale_add_test(fault_env SOURCES io/fault_env_test.cc LIBS shale_testkit)
shale_add_test(scheduler SOURCES io/scheduler_test.cc LIBS shale_io)
shale_add_test(log SOURCES io/log_test.cc LIBS shale_testkit)
shale_add_test(skiplist SOURCES io/skiplist_test.cc LIBS shale_testkit)
shale_add_test(write_batch SOURCES io/write_batch_test.cc LIBS shale_testkit)
shale_add_test(memtable SOURCES io/memtable_test.cc LIBS shale_testkit)
shale_add_test(block SOURCES io/block_test.cc LIBS shale_testkit)
shale_add_test(iterator_util SOURCES io/iterator_util_test.cc LIBS shale_testkit)
shale_add_test(bloom SOURCES io/bloom_test.cc LIBS shale_io)
shale_add_test(lru_cache SOURCES io/lru_cache_test.cc LIBS shale_io)
shale_add_test(table_format SOURCES io/table_format_test.cc LIBS shale_testkit)
shale_add_test(table_builder SOURCES io/table_builder_test.cc LIBS shale_testkit)
shale_add_test(two_level_iterator SOURCES io/two_level_iterator_test.cc LIBS shale_testkit)
shale_add_test(table SOURCES io/table_test.cc LIBS shale_testkit)
shale_add_test(merging_iterator SOURCES io/merging_iterator_test.cc LIBS shale_testkit)

# The core's tests link shale_core and nothing else, so that the core cannot grow a dependency on the
# I/O layer or the test kit without the link failing.
shale_add_test(version_edit SOURCES core/version_edit_test.cc LIBS shale_core)
shale_add_test(version_state SOURCES core/version_state_test.cc LIBS shale_core)
shale_add_test(compaction_picker SOURCES core/compaction_picker_test.cc LIBS shale_core)
shale_add_test(compaction_filter SOURCES core/compaction_filter_test.cc LIBS shale_core)

# The database layer's tests run against the fault-injecting Env, which the test kit provides.
shale_add_test(builder SOURCES db/builder_test.cc LIBS shale_db shale_testkit)
shale_add_test(compaction_job SOURCES db/compaction_job_test.cc LIBS shale_db shale_testkit)
shale_add_test(version_set SOURCES db/version_set_test.cc LIBS shale_db shale_testkit)
shale_add_test(options SOURCES db/options_test.cc LIBS shale_db)
shale_add_test(snapshot_list SOURCES db/snapshot_list_test.cc LIBS shale_db)
shale_add_test(info_log SOURCES db/info_log_test.cc LIBS shale_db shale_testkit)

# The fixture that the tests of the open database share: a database over the fault-injecting Env and
# a scheduler that only the test drives. It links shale_db and the test kit, so a test that links it
# names neither.
shale_add_library(shale_dbtest
  SOURCES
    db/db_test_util.cc
  LIBS shale_db shale_testkit)

shale_add_test(db_basic SOURCES db/db_basic_test.cc LIBS shale_dbtest)
shale_add_test(db_recovery SOURCES db/db_recovery_test.cc LIBS shale_dbtest)
shale_add_test(db_flush SOURCES db/db_flush_test.cc LIBS shale_dbtest)
shale_add_test(db_compaction SOURCES db/db_compaction_test.cc LIBS shale_dbtest)
shale_add_test(db_iter SOURCES db/db_iter_test.cc LIBS shale_dbtest)
shale_add_test(db_snapshot SOURCES db/db_snapshot_test.cc LIBS shale_dbtest)
shale_add_test(db_write SOURCES db/db_write_test.cc LIBS shale_dbtest)
shale_add_test(db_properties SOURCES db/db_properties_test.cc LIBS shale_dbtest)

# The deterministic simulator's reference model and the checkers of its invariants. Each checker is
# tested against a database that lies, so that it is seen to alarm as well as to stay quiet.
shale_add_test(model_db SOURCES sim/model_db_test.cc LIBS shale_testkit)
shale_add_test(invariants SOURCES sim/invariants_test.cc LIBS shale_db shale_testkit)

# The deterministic simulator: a seeded driver over the fault-injecting Env and a manual scheduler
# that runs random operations against a database and checks it against the model after every one.
# It is a library of its own because it drives the engine through DBImpl's test hooks, which the
# test kit does not link. The regression seeds are read from the source tree.
shale_add_library(shale_simulator
  SOURCES
    testkit/simulator.cc
  LIBS shale_db shale_testkit)

shale_add_test(sim SOURCES sim/sim_test.cc LIBS shale_simulator)
target_compile_definitions(sim_test PRIVATE
  SHALE_REGRESSION_SEEDS_PATH="${CMAKE_CURRENT_SOURCE_DIR}/sim/regression_seeds.txt")
# Fifty seeds of two thousand steps, with the runs of the other tests, take about a minute in a
# debug build and some ten minutes under ThreadSanitizer, which is more than the default of five
# minutes a test is given.
set_tests_properties(sim_test PROPERTIES TIMEOUT 1800)
shale_add_test(determinism SOURCES sim/determinism_test.cc LIBS shale_simulator)

# The database under real threads, with its own scheduler thread and an in-memory Env. These are
# the tests that ThreadSanitizer is for, and the sanitize job runs the whole suite three times under
# it, these included. The label lets one run them alone: ctest -L stress.
shale_add_test(concurrency SOURCES stress/concurrency_test.cc LIBS shale_dbtest)
set_tests_properties(concurrency_test PROPERTIES LABELS stress)

# Clang's thread-safety analysis is what makes SHALE_GUARDED_BY more than a comment, and nothing
# else would notice if it silently stopped working. So the build itself is tested: one translation
# unit that reads a guarded member without the lock must be rejected, and the same unit with the
# lock must be accepted. GCC has no such analysis, and the annotations expand to nothing there.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  set(_thread_safety_command
    "${CMAKE_CXX_COMPILER}" -std=c++20 -fsyntax-only -Wthread-safety -Werror
    -I${PROJECT_SOURCE_DIR}/src)
  add_test(NAME thread_safety_rejects_unguarded_access
    COMMAND ${_thread_safety_command}
            "${CMAKE_CURRENT_SOURCE_DIR}/compile_fail/guarded_access_bad.cc")
  set_tests_properties(thread_safety_rejects_unguarded_access PROPERTIES WILL_FAIL TRUE)
  add_test(NAME thread_safety_accepts_guarded_access
    COMMAND ${_thread_safety_command}
            "${CMAKE_CURRENT_SOURCE_DIR}/compile_fail/guarded_access_good.cc")
endif()

add_test(NAME layering_check_alarms_and_passes
         COMMAND "${SHALE_BASH}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/layering_test.sh")
set_tests_properties(layering_check_alarms_and_passes PROPERTIES TIMEOUT 120)

add_test(NAME hygiene_check_alarms_and_passes
         COMMAND "${SHALE_BASH}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/hygiene_test.sh")
set_tests_properties(hygiene_check_alarms_and_passes PROPERTIES TIMEOUT 300)

add_test(NAME cmake_options_accept_and_reject
         COMMAND "${SHALE_BASH}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/options_test.sh"
                 "${CMAKE_COMMAND}" "${CMAKE_GENERATOR}" "${CMAKE_CXX_COMPILER}")
set_tests_properties(cmake_options_accept_and_reject PROPERTIES TIMEOUT 300)
