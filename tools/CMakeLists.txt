# The command-line tools. shale_add_library() and shale_set_warnings() are defined in
# cmake/ShaleTargets.cmake, so that the tools get the same warnings as the library.

# What shalectl does with its command line, and the offline check of a database. They are libraries
# so that the tests can reach them without running the tool; the headers are included by their bare
# names, as "cli_args.h".
shale_add_library(shale_cli SOURCES cli_args.cc LIBS shale_util)
target_include_directories(shale_cli PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

shale_add_library(shale_verify SOURCES verify.cc LIBS shale_db)
target_include_directories(shale_verify PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(shalectl shalectl.cc)
# The dumps read the engine's own formats, which only the internal headers describe.
target_include_directories(shalectl PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(shalectl PRIVATE shale_cli shale_verify shale_db)
set_target_properties(shalectl PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/tools")
shale_set_warnings(shalectl)
