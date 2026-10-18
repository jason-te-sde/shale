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
# benchmark's runs, and the histogram behind the latency columns. They are a library so that the
# tests reach them without running the tool, and run the workloads against an engine that lies. The
# headers are included by their bare names, as "histogram.h".
shale_add_library(shale_bench_support
  SOURCES
    bench_engine.cc
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
target_compile_definitions(shale_bench PRIVATE
  SHALE_BENCH_COMPILER_ID="${CMAKE_CXX_COMPILER_ID}"
  SHALE_BENCH_COMPILER_VERSION="${CMAKE_CXX_COMPILER_VERSION}"
  SHALE_BENCH_BUILD_TYPE="$<CONFIG>"
  SHALE_BENCH_SANITIZER="${SHALE_SANITIZE}")
set_target_properties(shale_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/tools")
shale_set_warnings(shale_bench)
