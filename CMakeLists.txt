cmake_minimum_required(VERSION 3.26.4)

##########################################

option(SOGEN_ENABLE_AVX2 "Enable AVX2 support" ON)
option(SOGEN_ENABLE_SANITIZER "Enable sanitizer" OFF)
option(SOGEN_ENABLE_CLANG_TIDY "Enable clang-tidy checks" OFF)
option(SOGEN_ENABLE_LTO "Enable interprocedural optimization (LTO/IPO)" ON)
option(SOGEN_ENABLE_RUST_CODE "Enable code parts written in rust" ON)
option(SOGEN_ENABLE_REFLECTION "Enable reflection using the reflect library" ON)
option(SOGEN_EMSCRIPTEN_MEMORY64 "Enable memory 64 support for emscripten builds" OFF)
option(SOGEN_EMSCRIPTEN_SUPPORT_NODEJS "Enable Node.js filesystem for emscripten compilation" OFF)

set(SOGEN_REFLECTION_LEVEL "0" CACHE STRING "Reflection level for the build")

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
sogen_check_reflection_support()

message(STATUS "Reflection level is set to: ${SOGEN_REFLECTION_LEVEL}")
add_compile_definitions(SOGEN_REFLECTION_LEVEL=${SOGEN_REFLECTION_LEVEL})

if(SOGEN_ENABLE_REFLECTION)
  add_compile_definitions(SOGEN_ENABLE_REFLECTION=1)
endif()

##########################################

if(PROJECT_IS_TOP_LEVEL)
  set(SOGEN_IS_SUBPROJECT OFF)
else()
  set(SOGEN_IS_SUBPROJECT ON)
endif()

##########################################

if(SKBUILD)
  set(SOGEN_BUILD_STATIC_DEFAULT ON)
else()
  set(SOGEN_BUILD_STATIC_DEFAULT ${SOGEN_IS_SUBPROJECT})
endif()
option(SOGEN_BUILD_STATIC "Build sogen as static libraries for embedding (e.g., IDA plugins)" ${SOGEN_BUILD_STATIC_DEFAULT})
option(SOGEN_DISABLE_NANOBIND_LEAK_WARNINGS "Disable nanobind leak warnings in the Python module" OFF)
if (SOGEN_IS_SUBPROJECT OR SKBUILD)
  set(SOGEN_BUILD_TOOLS_DEFAULT OFF)
else()
  set(SOGEN_BUILD_TOOLS_DEFAULT ON)
endif()
if(SKBUILD)
  set(SOGEN_ENABLE_PYTHON_BINDINGS_DEFAULT ON)
else()
  set(SOGEN_ENABLE_PYTHON_BINDINGS_DEFAULT OFF)
endif()
option(SOGEN_BUILD_TOOLS "Build Sogen standalone tools (analyzer, debugger, fuzzer, etc.)" ${SOGEN_BUILD_TOOLS_DEFAULT})
option(SOGEN_ENABLE_PYTHON_BINDINGS "Configure nanobind-based Python bindings" ${SOGEN_ENABLE_PYTHON_BINDINGS_DEFAULT})

##########################################

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

##########################################

include(cmake/utils.cmake)
include(cmake/compiler-env.cmake)

##########################################

sogen_set_new_artifact_directory()

##########################################

sogen_add_subdirectory_and_get_targets("deps" EXTERNAL_TARGETS)
sogen_add_subdirectory_and_get_targets("src" OWN_TARGETS)

##########################################

sogen_targets_set_folder("dependencies" ${EXTERNAL_TARGETS})

sogen_targets_exclude_from_all(${EXTERNAL_TARGETS})
sogen_targets_disable_warnings(${EXTERNAL_TARGETS})

sogen_targets_expose_includes(${OWN_TARGETS})
sogen_targets_set_warnings_as_errors(${OWN_TARGETS})
sogen_targets_enable_clang_tidy(${OWN_TARGETS})

if(SOGEN_ENABLE_PYTHON_BINDINGS)
  foreach(_nanobind_target
    nanobind
    nanobind-static
    nanobind-abi3
    nanobind-ft
    nanobind-abi3-ft
  )
    if(TARGET ${_nanobind_target})
      set_target_properties(${_nanobind_target} PROPERTIES
        C_CLANG_TIDY ""
        CXX_CLANG_TIDY ""
      )

      if(MSVC)
        target_compile_options(${_nanobind_target} PRIVATE /W0)
      else()
        target_compile_options(${_nanobind_target} PRIVATE -w)
      endif()
    endif()
  endforeach()
endif()
