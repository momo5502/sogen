cmake_minimum_required(VERSION 3.26.4)

##########################################

option(MOMO_ENABLE_AVX2 "Enable AVX2 support" ON)
option(MOMO_ENABLE_SANITIZER "Enable sanitizer" OFF)
option(MOMO_ENABLE_CLANG_TIDY "Enable clang-tidy checks" OFF)
option(MOMO_ENABLE_RUST_CODE "Enable code parts written in rust" ON)
option(MOMO_EMSCRIPTEN_SUPPORT_NODEJS "Enable Node.js filesystem for emscripten compilation" OFF)
option(MOMO_BUILD_AS_LIBRARY "Configure and Build the emulator as a shared library (without the samples and tests)" OFF)

set(MOMO_REFLECTION_LEVEL "0" CACHE STRING "Reflection level for the build")
message(STATUS "Reflection level is set to: ${MOMO_REFLECTION_LEVEL}")
add_compile_definitions(MOMO_REFLECTION_LEVEL=${MOMO_REFLECTION_LEVEL})

##########################################

set(CMAKE_DISABLE_IN_SOURCE_BUILD ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
  set(CMAKE_OSX_DEPLOYMENT_TARGET 11.0)
endif()

# Prevent unicorn from generating universal binaries on macOS
# It doesn't support it, even if it thinks it does...
set(ENV{ARCHFLAGS} "nope")

##########################################

include(cmake/sccache.cmake)

##########################################

project(emulator LANGUAGES C CXX)
enable_testing()

##########################################

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

##########################################

include(cmake/utils.cmake)
include(cmake/compiler-env.cmake)

##########################################

momo_set_new_artifact_directory()

##########################################

momo_add_subdirectory_and_get_targets("deps" EXTERNAL_TARGETS)
momo_add_subdirectory_and_get_targets("src" OWN_TARGETS)

##########################################

momo_targets_set_folder("dependencies" ${EXTERNAL_TARGETS})

momo_targets_exclude_from_all(${EXTERNAL_TARGETS})
momo_targets_disable_warnings(${EXTERNAL_TARGETS})

momo_targets_expose_includes(${OWN_TARGETS})
momo_targets_set_warnings_as_errors(${OWN_TARGETS})
momo_targets_enable_clang_tidy(${OWN_TARGETS})
