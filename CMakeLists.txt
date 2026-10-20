cmake_minimum_required(VERSION 3.25)

project(shale
  VERSION 0.1.1
  DESCRIPTION "An LSM-tree storage engine written from scratch in C++20"
  LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

include(GNUInstallDirs)
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

# The fuzz targets link the test kit's reference model, so they need the tests' subdirectory.
if(SHALE_FUZZ)
  if(NOT SHALE_BUILD_TESTS)
    message(FATAL_ERROR "SHALE_FUZZ needs SHALE_BUILD_TESTS=ON: fuzz_db_ops links the test kit")
  endif()
  add_subdirectory(fuzz)
endif()

if(SHALE_BUILD_BENCH)
  add_subdirectory(bench)
endif()

# The installation: the public headers, the four libraries, and a CMake package for them, so that a
# project of someone else builds against shale with find_package(shale) and the target shale::shale.
# Nothing under src/ is installed: those headers are the libraries' own, and the libraries do not
# expose them.
include(CMakePackageConfigHelpers)
set(SHALE_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/shale")

install(DIRECTORY include/shale
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  FILES_MATCHING PATTERN "*.h")
install(TARGETS shale shale_util shale_io shale_core shale_db
  EXPORT shaleTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(EXPORT shaleTargets
  NAMESPACE shale::
  DESTINATION "${SHALE_INSTALL_CMAKEDIR}")

configure_package_config_file(cmake/shaleConfig.cmake.in
  "${PROJECT_BINARY_DIR}/shaleConfig.cmake"
  INSTALL_DESTINATION "${SHALE_INSTALL_CMAKEDIR}")
# An installed release satisfies a request for any version up to its own that has the same major
# version: 0.1.0 is found for find_package(shale 0.1), and is refused for find_package(shale 1.0).
write_basic_package_version_file("${PROJECT_BINARY_DIR}/shaleConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMajorVersion)
install(FILES
    "${PROJECT_BINARY_DIR}/shaleConfig.cmake"
    "${PROJECT_BINARY_DIR}/shaleConfigVersion.cmake"
  DESTINATION "${SHALE_INSTALL_CMAKEDIR}")

find_package(Doxygen QUIET)
if(DOXYGEN_FOUND)
  # Doxygen resolves INPUT and OUTPUT_DIRECTORY against its working directory.
  add_custom_target(docs
    COMMAND "${DOXYGEN_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/docs/Doxyfile"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    COMMENT "Building the API documentation (warnings are errors)"
    VERBATIM)
endif()
