# The warning set for every shale target. It is applied per target, by shale_set_warnings() in
# ShaleTargets.cmake, and deliberately not with a directory-wide add_compile_options(): that would
# also reach the GoogleTest and Google Benchmark sources fetched later, and -Werror must never
# apply to them.

set(SHALE_WARNING_FLAGS
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
  -Wnon-virtual-dtor -Wold-style-cast -Werror)
