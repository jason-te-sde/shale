# A standalone project that uses an installed shale, the way a project of someone else does. It is
# not part of the main build: CI and scripts/preflight.sh install shale into a scratch prefix and
# configure this directory against it with -DCMAKE_PREFIX_PATH=<prefix>, so what is built here sees
# only what the installation holds.
cmake_minimum_required(VERSION 3.25)

project(shale_examples LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(shale 0.1 REQUIRED)

# The warning set of every shale target, which cmake/ShaleWarnings.cmake holds. It is repeated here
# because this project has to build with nothing of the repository but this directory.
set(example_warnings
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
  -Wnon-virtual-dtor -Wold-style-cast -Werror
  $<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wthread-safety>)

# A tour of the API, with every status checked.
add_executable(basic basic.cc)
target_link_libraries(basic PRIVATE shale::shale)
target_compile_options(basic PRIVATE ${example_warnings})

# The program of the README, fifteen lines. The three lines of CMake that name it are the ones the
# README shows for a project that uses shale, and tests/docs/readme_test.cc keeps both in step.
add_executable(quickstart quickstart.cc)
target_link_libraries(quickstart PRIVATE shale::shale)
target_compile_options(quickstart PRIVATE ${example_warnings})
