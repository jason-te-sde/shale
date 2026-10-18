cmake_minimum_required(VERSION 3.25)

project(shale
  VERSION 0.1.0
  DESCRIPTION "An LSM-tree storage engine written from scratch in C++20"
  LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

include(ShaleOptions)
include(ShaleWarnings)
include(ShaleTargets)

if(SHALE_BUILD_TESTS)
  enable_testing()
endif()

add_subdirectory(src)

if(SHALE_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

if(SHALE_BUILD_TESTS)
  add_subdirectory(tests)
endif()

find_package(Doxygen QUIET)
if(DOXYGEN_FOUND)
  # Doxygen resolves INPUT and OUTPUT_DIRECTORY against its working directory.
  add_custom_target(docs
    COMMAND "${DOXYGEN_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/docs/Doxyfile"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    COMMENT "Building the API documentation (warnings are errors)"
    VERBATIM)
endif()
