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

add_executable(basic basic.cc)
target_link_libraries(basic PRIVATE shale::shale)
# The warning set of every shale target, which cmake/ShaleWarnings.cmake holds. It is repeated here
# because this project has to build with nothing of the repository but this directory.
target_compile_options(basic PRIVATE
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
  -Wnon-virtual-dtor -Wold-style-cast -Werror
  $<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wthread-safety>)
