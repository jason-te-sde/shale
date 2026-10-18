# The libFuzzer targets: one for every decoder that reads bytes off a disk, and one that drives the
# whole database with a random program. Included only when SHALE_FUZZ is on, which needs Clang and
# AddressSanitizer with UndefinedBehaviorSanitizer (cmake/ShaleOptions.cmake checks both). Every
# target keeps its files in a MemEnv, so a fuzz run never writes to the disk.
#
#   cmake --preset fuzz && cmake --build --preset fuzz --target fuzz_targets
#   build/fuzz/fuzz/fuzz_table -max_total_time=60 build/fuzz/fuzz/corpus/fuzz_table

set(SHALE_FUZZ_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}")
set(SHALE_FUZZ_CORPUS_DIR "${SHALE_FUZZ_OUTPUT_DIR}/corpus")

# The seeds are written at build time and the directory is not committed: a seed is a valid file
# of its format, and the writers that make one are the engine's own, so a seed cannot go stale.
add_executable(make_corpus make_corpus.cc)
target_include_directories(make_corpus PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(make_corpus PRIVATE shale_core shale_io shale_util)
set_target_properties(make_corpus PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${SHALE_FUZZ_OUTPUT_DIR}")
shale_set_warnings(make_corpus)

add_custom_command(
  OUTPUT "${SHALE_FUZZ_CORPUS_DIR}/.seeded"
  COMMAND make_corpus "${SHALE_FUZZ_CORPUS_DIR}"
  COMMAND "${CMAKE_COMMAND}" -E touch "${SHALE_FUZZ_CORPUS_DIR}/.seeded"
  DEPENDS make_corpus
  COMMENT "Writing the seed corpus"
  VERBATIM)
add_custom_target(fuzz_corpus ALL DEPENDS "${SHALE_FUZZ_CORPUS_DIR}/.seeded")

# Builds every fuzz target and the seed corpus, and nothing else: the fuzz build instruments the
# test suite as well, which a fuzz run does not need.
add_custom_target(fuzz_targets)

# shale_add_fuzz_target(<name> LIBS <targets...>)
#
# Builds <name> from <name>.cc into the fuzz output directory. "-fsanitize=fuzzer" links the
# libFuzzer runtime, which supplies main(); the address and undefined sanitizers come from the
# SHALE_SANITIZE flags that reach every target.
function(shale_add_fuzz_target name)
  cmake_parse_arguments(ARG "" "" "LIBS" ${ARGN})
  add_executable(${name} ${name}.cc)
  target_include_directories(${name} PRIVATE "${PROJECT_SOURCE_DIR}/src")
  target_compile_options(${name} PRIVATE -fsanitize=fuzzer)
  target_link_options(${name} PRIVATE -fsanitize=fuzzer)
  target_link_libraries(${name} PRIVATE ${ARG_LIBS})
  set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${SHALE_FUZZ_OUTPUT_DIR}")
  add_dependencies(${name} fuzz_corpus)
  add_dependencies(fuzz_targets ${name})
  shale_set_warnings(${name})
endfunction()

shale_add_fuzz_target(fuzz_log_reader LIBS shale_io)
shale_add_fuzz_target(fuzz_block LIBS shale_io)
shale_add_fuzz_target(fuzz_table LIBS shale_io)
shale_add_fuzz_target(fuzz_version_edit LIBS shale_core)
shale_add_fuzz_target(fuzz_db_ops LIBS shale_db shale_testkit)
