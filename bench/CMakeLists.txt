# The micro benchmarks, built when SHALE_BUILD_BENCH is ON. shale_add_library() and
# shale_set_warnings() are defined in cmake/ShaleTargets.cmake. The workloads against a whole
# database are shale_bench, in tools/, which is built with the other tools; what is here times one
# component at a time.
include(FetchContent)

# Google Benchmark is fetched pinned, as GoogleTest is, and declared SYSTEM so that its headers are
# not held to our warnings. It is built without its own tests and installation, which this project
# does not use, and without -Werror, which it turns on by default: its sources are not ours to fix
# when a newer compiler has opinions about them.
FetchContent_Declare(benchmark
  GIT_REPOSITORY https://github.com/google/benchmark.git
  GIT_TAG v1.9.1
  GIT_SHALLOW TRUE
  SYSTEM)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BENCHMARK_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(benchmark)

# The warnings of shale are per target, so none of them reach Google Benchmark; this silences its
# own, as the tests do for GoogleTest, so that they do not stay in every build log.
foreach(dependency benchmark benchmark_main)
  if(TARGET ${dependency})
    target_compile_options(${dependency} PRIVATE -w)
  endif()
endforeach()

# The subjects are internal headers, so the include path is src/, as for the tests.
add_executable(micro_bench micro_bench.cc)
target_include_directories(micro_bench PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(micro_bench PRIVATE shale_io shale_util benchmark::benchmark_main)
set_target_properties(micro_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bench")
shale_set_warnings(micro_bench)
