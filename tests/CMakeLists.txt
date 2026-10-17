include(FetchContent)

# The script tests need both. REQUIRED makes a machine without them fail here, at configure time,
# instead of silently running fewer tests.
find_program(SHALE_BASH bash REQUIRED)
find_program(SHALE_GIT git REQUIRED)

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
  target_compile_options(${dependency} PRIVATE -w)
endforeach()

# shale_add_test(<name> SOURCES <files...> LIBS <targets...>)
#
# Builds the executable <name>_test into <build>/tests/ and registers it with CTest under the same
# name. Tests may include internal headers from src/.
function(shale_add_test name)
  cmake_parse_arguments(ARG "" "" "SOURCES;LIBS" ${ARGN})
  if(NOT ARG_SOURCES OR ARG_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "usage: shale_add_test(<name> SOURCES <files...> LIBS <targets...>)")
  endif()
  set(target ${name}_test)
  add_executable(${target} ${ARG_SOURCES})
  target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/src")
  target_link_libraries(${target} PRIVATE ${ARG_LIBS} GTest::gtest_main)
  set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/tests")
  shale_set_warnings(${target})
  add_test(NAME ${target} COMMAND ${target})
  set_tests_properties(${target} PROPERTIES TIMEOUT 300)
endfunction()

shale_add_test(status SOURCES util/status_test.cc LIBS shale_util)

add_test(NAME layering_check_alarms_and_passes
         COMMAND "${SHALE_BASH}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/layering_test.sh")
set_tests_properties(layering_check_alarms_and_passes PROPERTIES TIMEOUT 120)

add_test(NAME hygiene_check_alarms_and_passes
         COMMAND "${SHALE_BASH}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/hygiene_test.sh")
set_tests_properties(hygiene_check_alarms_and_passes PROPERTIES TIMEOUT 120)

add_test(NAME cmake_options_accept_and_reject
         COMMAND "${SHALE_BASH}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/options_test.sh"
                 "${CMAKE_COMMAND}" "${CMAKE_GENERATOR}")
set_tests_properties(cmake_options_accept_and_reject PROPERTIES TIMEOUT 300)
