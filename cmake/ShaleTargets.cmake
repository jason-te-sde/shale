# The functions every shale target is created through, so that src/, tests/, tools/, fuzz/ and
# bench/ all get the same warnings, flaw definition and include paths.

# shale_set_warnings(<target>)
#
# The per-target hook for every shale target (library, test, tool, fuzzer, example). Applies the
# warning set, plus the SHALE_FLAW definition when one is selected. The flaw rides along here so
# that a target created by a later change cannot forget it.
function(shale_set_warnings target)
  target_compile_options(${target} PRIVATE
    ${SHALE_WARNING_FLAGS}
    $<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wthread-safety>)
  if(SHALE_FLAW_DEFINITION)
    target_compile_definitions(${target} PRIVATE ${SHALE_FLAW_DEFINITION})
  endif()
endfunction()

# shale_add_library(<name> SOURCES <files...> [LIBS <targets...>])
#
# A static library whose sources may include internal headers as "util/foo.h". Those headers live
# under src/, are visible only to the library itself and to tests, and are never installed. The
# public headers under include/ are visible to everything that links the library: from the source
# tree while it is built, and from <prefix>/include once it is installed, which is what a library
# that is exported has to say about its include directory.
function(shale_add_library name)
  cmake_parse_arguments(ARG "" "" "SOURCES;LIBS" ${ARGN})
  if(NOT ARG_SOURCES OR ARG_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "usage: shale_add_library(<name> SOURCES <files...> [LIBS <targets...>])")
  endif()
  add_library(${name} STATIC ${ARG_SOURCES})
  target_compile_features(${name} PUBLIC cxx_std_20)
  target_include_directories(${name}
    PUBLIC
      "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
      "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    PRIVATE "${PROJECT_SOURCE_DIR}/src")
  if(ARG_LIBS)
    target_link_libraries(${name} PUBLIC ${ARG_LIBS})
  endif()
  shale_set_warnings(${name})
endfunction()

# shale_add_test(<name> SOURCES <files...> LIBS <targets...>)
#
# Builds the executable <name>_test into <build>/tests/ and registers it with CTest under the same
# name. Tests may include internal headers from src/. Needs GoogleTest, which tests/CMakeLists.txt
# fetches when SHALE_BUILD_TESTS is ON.
function(shale_add_test name)
  cmake_parse_arguments(ARG "" "" "SOURCES;LIBS" ${ARGN})
  if(NOT ARG_SOURCES OR ARG_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "usage: shale_add_test(<name> SOURCES <files...> LIBS <targets...>)")
  endif()
  if(NOT TARGET GTest::gtest_main)
    message(FATAL_ERROR "shale_add_test(${name}) needs GoogleTest; configure with SHALE_BUILD_TESTS=ON")
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
