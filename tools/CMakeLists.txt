# The command-line tools. shale_add_library() and shale_set_warnings() are defined in
# cmake/ShaleTargets.cmake, so that the tools get the same warnings as the library.

# What shalectl does with its command line, and the offline check of a database. They are libraries
# so that the tests can reach them without running the tool; the headers are included by their bare
# names, as "cli_args.h".
shale_add_library(shale_cli SOURCES cli_args.cc LIBS shale_util)
target_include_directories(shale_cli PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

shale_add_library(shale_verify SOURCES verify.cc LIBS shale_db)
target_include_directories(shale_verify PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

# What shale_bench is made of apart from its command line: the adapter that lets one set of
# workloads drive the engine, the workloads and the checks of their answers, the medians over a
# benchmark's runs, the table and the JSON that show them, and the histogram behind the latency
# columns. They are a library so that the
# tests reach them without running the tool, and run the workloads against an engine that lies. The
# headers are included by their bare names, as "histogram.h".
shale_add_library(shale_bench_support
  SOURCES
    bench_engine.cc
    bench_report.cc
    bench_stats.cc
    bench_workloads.cc
    histogram.cc
  LIBS shale_db)
target_include_directories(shale_bench_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(shalectl shalectl.cc)
# The dumps read the engine's own formats, which only the internal headers describe.
target_include_directories(shalectl PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(shalectl PRIVATE shale_cli shale_verify shale_db)
set_target_properties(shalectl PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/tools")
shale_set_warnings(shalectl)

# The crash test forks a writer, kills it and reopens the database. It reaches the engine through the
# public interface, and the internal headers only for file names, the byte coding and the seeded
# generator.
add_executable(shale_crashtest shale_crashtest.cc)
target_include_directories(shale_crashtest PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(shale_crashtest PRIVATE shale_cli shale_db)
set_target_properties(shale_crashtest PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/tools")
shale_set_warnings(shale_crashtest)

# The benchmark tool: the command line, the table and the JSON around shale_bench_support. What it
# was built with is recorded in its output, so a number that is quoted says which compiler and build
# type made it.
add_executable(shale_bench shale_bench.cc)
target_include_directories(shale_bench PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(shale_bench PRIVATE shale_bench_support shale_cli shale_db)

# The LevelDB engine that shale is measured against, for the comparison in docs/benchmarks.md. It is
# built when the benchmarks are asked for and LevelDB is installed, which a CI runner has not been
# asked to do: a build without it is the build that CI tests, and --engine=leveldb then says so
# instead of failing to link. LevelDB's own CMake package is what is looked for, so that a
# Homebrew installation is found with -DCMAKE_PREFIX_PATH=$(brew --prefix leveldb).
set(SHALE_BENCH_HAS_LEVELDB OFF)
if(SHALE_BUILD_BENCH)
  # LevelDB's package names Threads::Threads in the target it exports and does not look for it
  # itself, so it is looked for first; without it the generate step fails once LevelDB is found.
  find_package(Threads)
  find_package(leveldb CONFIG QUIET)
  if(leveldb_FOUND)
    set(SHALE_BENCH_HAS_LEVELDB ON)
    # Homebrew's LevelDB asks for "snappy" by that bare name, which the linker resolves only on a
    # path it already searches, and /opt/homebrew/lib is not one. A target of that name, made from
    # the library that find_library() finds, gives the link its full path.
    if(NOT TARGET snappy)
      find_library(SHALE_BENCH_SNAPPY_LIBRARY snappy)
      if(SHALE_BENCH_SNAPPY_LIBRARY)
        add_library(snappy UNKNOWN IMPORTED)
        set_target_properties(snappy PROPERTIES IMPORTED_LOCATION "${SHALE_BENCH_SNAPPY_LIBRARY}")
      endif()
    endif()
  endif()
endif()
# The tests are added after this directory and ask whether the engine is there.
set(SHALE_BENCH_HAS_LEVELDB "${SHALE_BENCH_HAS_LEVELDB}" PARENT_SCOPE)

if(SHALE_BENCH_HAS_LEVELDB)
  message(STATUS "shale_bench: the LevelDB engine is built (found leveldb ${leveldb_VERSION})")
  shale_add_library(shale_bench_leveldb SOURCES bench_leveldb.cc LIBS shale_bench_support leveldb::leveldb)
  target_include_directories(shale_bench_leveldb PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(shale_bench PRIVATE shale_bench_leveldb)
  target_compile_definitions(shale_bench PRIVATE SHALE_BENCH_HAVE_LEVELDB=1)
elseif(SHALE_BUILD_BENCH)
  message(STATUS "shale_bench: no LevelDB found, so --engine=leveldb is not built")
endif()
target_compile_definitions(shale_bench PRIVATE
  SHALE_BENCH_COMPILER_ID="${CMAKE_CXX_COMPILER_ID}"
  SHALE_BENCH_COMPILER_VERSION="${CMAKE_CXX_COMPILER_VERSION}"
  SHALE_BENCH_BUILD_TYPE="$<CONFIG>"
  SHALE_BENCH_SANITIZER="${SHALE_SANITIZE}"
  SHALE_BENCH_COVERAGE=$<BOOL:${SHALE_COVERAGE}>
  SHALE_BENCH_FLAW="${SHALE_FLAW}")
set_target_properties(shale_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/tools")
shale_set_warnings(shale_bench)
