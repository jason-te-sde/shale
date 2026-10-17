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
# the fault-injecting Env implements, and passes it on: a test that links shale_testkit does not
# name shale_io too, or the linker reports the library twice.
shale_add_library(shale_testkit
  SOURCES
    testkit/fault_env.cc
    testkit/test_util.cc
  LIBS shale_io shale_util GTest::gtest)
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
