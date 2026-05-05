cmake_minimum_required(VERSION 3.26.4)

##########################################

option(MOMO_ENABLE_AVX2 "Enable AVX2 support" ON)
option(MOMO_ENABLE_SANITIZER "Enable sanitizer" OFF)
option(MOMO_ENABLE_CLANG_TIDY "Enable clang-tidy checks" OFF)
option(MOMO_ENABLE_RUST_CODE "Enable code parts written in rust" ON)
option(MOMO_ENABLE_REFLECTION "Enable reflection using the reflect library" ON)
option(MOMO_EMSCRIPTEN_MEMORY64 "Enable memory 64 support for emscripten builds" OFF)
option(MOMO_EMSCRIPTEN_SUPPORT_NODEJS "Enable Node.js filesystem for emscripten compilation" OFF)

set(MOMO_REFLECTION_LEVEL "0" CACHE STRING "Reflection level for the build")

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

project(sogen LANGUAGES C CXX)
enable_testing()

include(cmake/reflection-check.cmake)
momo_check_reflection_support()

message(STATUS "Reflection level is set to: ${MOMO_REFLECTION_LEVEL}")
add_compile_definitions(MOMO_REFLECTION_LEVEL=${MOMO_REFLECTION_LEVEL})

if(MOMO_ENABLE_REFLECTION)
  add_compile_definitions(MOMO_ENABLE_REFLECTION=1)
endif()

##########################################

if(PROJECT_IS_TOP_LEVEL)
  set(MOMO_IS_SUBPROJECT OFF)
else()
  set(MOMO_IS_SUBPROJECT ON)
endif()

##########################################

option(SOGEN_BUILD_STATIC "Build sogen as static libraries for embedding (e.g., IDA plugins)" ${MOMO_IS_SUBPROJECT})
if (MOMO_IS_SUBPROJECT)
  set(SOGEN_BUILD_TOOLS_DEFAULT OFF)
else()
  set(SOGEN_BUILD_TOOLS_DEFAULT ON)
endif()
option(SOGEN_BUILD_TOOLS "Build Sogen standalone tools (analyzer, debugger, fuzzer, etc.)" ${SOGEN_BUILD_TOOLS_DEFAULT})

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
