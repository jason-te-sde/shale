# Build options, toolchain floors, and the flags that must reach every target (sanitizers,
# coverage). Included once from the top-level CMakeLists.txt, so everything below runs in that
# directory scope and applies to every target created afterwards, fetched dependencies included.

option(SHALE_BUILD_TESTS "Build the unit tests" ${PROJECT_IS_TOP_LEVEL})
option(SHALE_BUILD_TOOLS "Build the command-line tools" ON)
option(SHALE_BUILD_BENCH "Build the benchmarks" OFF)
option(SHALE_FUZZ "Build the fuzz targets (Clang only)" OFF)
option(SHALE_COVERAGE "Instrument the build for coverage" OFF)

set(SHALE_SANITIZE "" CACHE STRING
  "Sanitizers to enable: empty, \"address,undefined\" or \"thread\"")
set_property(CACHE SHALE_SANITIZE PROPERTY STRINGS "" "address,undefined" "thread")

# The deliberate flaws the verification harness injects. A flaw build compiles one of them in; it
# exists so that a test suite can prove it would notice the bug.
set(SHALE_FLAW_NAMES
  skip_wal_sync
  skip_dir_sync
  drop_tombstones_early
  ignore_snapshots_in_compaction
  skip_manifest_sync
  delete_live_files)
list(JOIN SHALE_FLAW_NAMES ", " SHALE_FLAW_NAMES_TEXT)
set(SHALE_FLAW "" CACHE STRING
  "Compile in one deliberate flaw: empty or one of ${SHALE_FLAW_NAMES_TEXT}")
set_property(CACHE SHALE_FLAW PROPERTY STRINGS "" ${SHALE_FLAW_NAMES})

# Compiler floors: GCC 13, Clang 17, AppleClang 15.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  set(_shale_compiler_floor 13)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  set(_shale_compiler_floor 17)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
  set(_shale_compiler_floor 15)
else()
  message(FATAL_ERROR
    "Unsupported compiler '${CMAKE_CXX_COMPILER_ID}'; shale needs GCC, Clang or AppleClang")
endif()
if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS _shale_compiler_floor)
  message(FATAL_ERROR
    "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} is too old; "
    "shale needs ${CMAKE_CXX_COMPILER_ID} ${_shale_compiler_floor} or newer")
endif()

if(SHALE_FUZZ AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  message(FATAL_ERROR
    "SHALE_FUZZ needs Clang (libFuzzer), but the compiler is ${CMAKE_CXX_COMPILER_ID}")
endif()

# Sanitizers and coverage change the runtime, so they apply to fetched dependencies too: an
# uninstrumented GoogleTest next to an instrumented test makes ThreadSanitizer report false races.
if(NOT SHALE_SANITIZE STREQUAL "")
  if(NOT SHALE_SANITIZE MATCHES "^(address,undefined|thread)$")
    message(FATAL_ERROR
      "SHALE_SANITIZE='${SHALE_SANITIZE}' is not one of: '', 'address,undefined', 'thread'")
  endif()
  add_compile_options(-fsanitize=${SHALE_SANITIZE} -fno-omit-frame-pointer)
  add_link_options(-fsanitize=${SHALE_SANITIZE})
  if(SHALE_SANITIZE STREQUAL "address,undefined")
    # By default UndefinedBehaviorSanitizer reports and carries on, which would leave the test green.
    add_compile_options(-fno-sanitize-recover=all)
    add_link_options(-fno-sanitize-recover=all)
  endif()
endif()

if(SHALE_COVERAGE)
  add_compile_options(--coverage)
  add_link_options(--coverage)
endif()

if(NOT SHALE_FLAW STREQUAL "")
  if(NOT SHALE_FLAW IN_LIST SHALE_FLAW_NAMES)
    message(FATAL_ERROR "SHALE_FLAW='${SHALE_FLAW}' is not one of: ${SHALE_FLAW_NAMES_TEXT}")
  endif()
  string(TOUPPER "${SHALE_FLAW}" _shale_flaw_upper)
  set(SHALE_FLAW_DEFINITION "SHALE_FLAW_${_shale_flaw_upper}=1")
endif()
