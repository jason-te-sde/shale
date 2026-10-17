# The per-target hook every shale target (library, test, tool, fuzzer, example) goes through.
# It is deliberately not a directory-wide add_compile_options(): that would also reach the
# GoogleTest and Google Benchmark sources fetched later, and -Werror must never apply to them.

set(SHALE_WARNING_FLAGS
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
  -Wnon-virtual-dtor -Wold-style-cast -Werror)

# Applies the warning set, plus the SHALE_FLAW definition when one is selected. The flaw rides
# along here so that a target created by a later task cannot forget it.
function(shale_set_warnings target)
  target_compile_options(${target} PRIVATE
    ${SHALE_WARNING_FLAGS}
    $<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wthread-safety>)
  if(SHALE_FLAW_DEFINITION)
    target_compile_definitions(${target} PRIVATE ${SHALE_FLAW_DEFINITION})
  endif()
endfunction()
