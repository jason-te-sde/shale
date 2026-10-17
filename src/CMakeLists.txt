# shale_add_library(<name> SOURCES <files...> [LIBS <targets...>])
#
# A static library whose sources may include internal headers as "util/foo.h". Those headers live
# under src/, are visible only to the library itself and to tests, and are never installed. The
# public headers under include/ are visible to everything that links the library.
function(shale_add_library name)
  cmake_parse_arguments(ARG "" "" "SOURCES;LIBS" ${ARGN})
  if(NOT ARG_SOURCES OR ARG_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "usage: shale_add_library(<name> SOURCES <files...> [LIBS <targets...>])")
  endif()
  add_library(${name} STATIC ${ARG_SOURCES})
  target_compile_features(${name} PUBLIC cxx_std_20)
  target_include_directories(${name}
    PUBLIC "${PROJECT_SOURCE_DIR}/include"
    PRIVATE "${PROJECT_SOURCE_DIR}/src")
  if(ARG_LIBS)
    target_link_libraries(${name} PUBLIC ${ARG_LIBS})
  endif()
  shale_set_warnings(${name})
endfunction()

shale_add_library(shale_util SOURCES util/status.cc)
