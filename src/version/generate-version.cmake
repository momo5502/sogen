# Writes the build identity header. Runs on every build in script mode, since a new commit does not reconfigure the
# project, and replaces the header only when its content changes, so that unchanged builds recompile nothing.
#
# Inputs: SOURCE_DIR, OUTPUT, and BUILD_ID, which replaces the Git commit when the source has no history.

set(build_id "${BUILD_ID}")
set(modified false)
set(description "${BUILD_ID}")

if(NOT build_id)
  find_program(GIT_EXECUTABLE git)
  if(GIT_EXECUTABLE)
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
      WORKING_DIRECTORY "${SOURCE_DIR}"
      OUTPUT_VARIABLE build_id
      RESULT_VARIABLE result
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET)
    if(NOT result EQUAL 0)
      set(build_id "")
    endif()
  endif()

  if(build_id)
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no --ignore-submodules=dirty
      WORKING_DIRECTORY "${SOURCE_DIR}"
      OUTPUT_VARIABLE changes
      ERROR_QUIET)
    if(changes)
      set(modified true)
    endif()

    execute_process(
      COMMAND "${GIT_EXECUTABLE}" describe --always --dirty --tags
      WORKING_DIRECTORY "${SOURCE_DIR}"
      OUTPUT_VARIABLE description
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET)
  endif()
endif()

set(content "#pragma once

#include <string_view>

namespace sogen::version
{
    // The commit the build was made from, or the build ID it was configured with. Empty when neither is known.
    inline constexpr std::string_view build_id = \"${build_id}\";

    // Whether the source had uncommitted changes, which the build ID does not reflect.
    inline constexpr bool modified = ${modified};

    inline constexpr std::string_view description = \"${description}\";
}
")

set(existing "")
if(EXISTS "${OUTPUT}")
  file(READ "${OUTPUT}" existing)
endif()

if(NOT existing STREQUAL content)
  file(WRITE "${OUTPUT}" "${content}")
endif()
