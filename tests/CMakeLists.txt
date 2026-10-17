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

shale_add_test(status SOURCES util/status_test.cc LIBS shale_util)
shale_add_test(arena SOURCES util/arena_test.cc LIBS shale_util)
shale_add_test(random SOURCES util/random_test.cc LIBS shale_util)
shale_add_test(check SOURCES util/check_test.cc LIBS shale_util)
shale_add_test(mutex SOURCES util/mutex_test.cc LIBS shale_util)
shale_add_test(coding SOURCES util/coding_test.cc LIBS shale_util)
shale_add_test(crc32c SOURCES util/crc32c_test.cc LIBS shale_util)
shale_add_test(hash SOURCES util/hash_test.cc LIBS shale_util)

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
