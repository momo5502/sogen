set(SOGEN_BUILD_ID "" CACHE STRING "Identifies the build, e.g. in snapshots, when the source has no Git history")
if(NOT SOGEN_BUILD_ID MATCHES "^[A-Za-z0-9._+-]*$" OR SOGEN_BUILD_ID MATCHES "^.{64}")
  message(FATAL_ERROR "SOGEN_BUILD_ID must be at most 63 letters, digits or . _ + - characters")
endif()

set(SOGEN_VERSION_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/include")
set(SOGEN_VERSION_HEADER "${SOGEN_VERSION_INCLUDE_DIR}/sogen_version.hpp")

add_custom_target(sogen-version-header
  COMMAND "${CMAKE_COMMAND}"
    "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
    "-DOUTPUT=${SOGEN_VERSION_HEADER}"
    "-DBUILD_ID=${SOGEN_BUILD_ID}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/generate-version.cmake"
  BYPRODUCTS "${SOGEN_VERSION_HEADER}"
  VERBATIM)

add_library(sogen-version INTERFACE)
target_include_directories(sogen-version INTERFACE "${SOGEN_VERSION_INCLUDE_DIR}")
add_dependencies(sogen-version sogen-version-header)
