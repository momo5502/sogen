cmake_minimum_required(VERSION 3.26.4)

##########################################

option(SOGEN_ENABLE_AVX2 "Enable AVX2 support" ON)
option(SOGEN_ENABLE_SANITIZER "Enable sanitizer" OFF)
option(SOGEN_ENABLE_CLANG_TIDY "Enable clang-tidy checks" OFF)
option(SOGEN_ENABLE_LTO "Enable interprocedural optimization (LTO/IPO)" ON)
option(SOGEN_ENABLE_RUST_CODE "Enable code parts written in rust" ON)
option(SOGEN_ENABLE_REFLECTION "Enable reflection using the reflect library" ON)
# SDL3 provides the host UI backend on every non-Emscripten platform, so it is
# required and enabled by default everywhere. Emscripten uses the web UI backend
# instead and skips SDL in deps/CMakeLists.txt. project() runs after this block,
# so EMSCRIPTEN is not known yet and cannot be branched on here.
option(SOGEN_ENABLE_SDL3 "Enable SDL3 dependency and backend scaffolding when submodule exists" ON)
# When ON, a system-installed SDL3 (find_package, e.g. libsdl3-dev on Ubuntu 25.04+,
# Homebrew, vcpkg) is used if present, falling back to the vendored deps/SDL submodule.
# Set OFF to always build the pinned submodule.
option(SOGEN_USE_SYSTEM_SDL3 "Prefer a system-installed SDL3 over the vendored deps/SDL submodule" ON)
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

# FEX-Emu backend (x86/x86-64 -> AArch64). FEX only JITs to ARM64, so the backend is restricted to
# ARM64 Linux/Android hosts. It is enabled automatically on those hosts whenever FEXCore is available.
# FEXCore is a large, ARM64-host-specific dependency that is not vendored, so we *probe* for it and
# skip the backend (rather than hard-failing) when absent - otherwise every arm64 build without FEX
# installed (e.g. the Android CI job) would break. Point -DSOGEN_FEX_ROOT=<path> at a FEX checkout/
# install, or make FEXCore discoverable via find_package(FEXCore), to enable it.
# FEX is vendored as a submodule (deps/FEX) and built in-tree. It requires Clang and only JITs to
# AArch64, and its headers need a libstdc++ with std::atomic_ref (the Android NDK's libc++ lacks it),
# so it is restricted to ARM64 Linux + Clang (not Android/NDK). Enabled automatically when those hold
# and the submodule is checked out.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT ANDROID AND CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64"
   AND CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/deps/FEX/CMakeLists.txt")
  set(SOGEN_ENABLE_FEX ON)
  message(STATUS "FEX backend: enabled (ARM64 Linux + Clang, deps/FEX present)")
else()
  set(SOGEN_ENABLE_FEX OFF)
endif()

##########################################

if(PROJECT_IS_TOP_LEVEL)
  set(SOGEN_IS_SUBPROJECT OFF)
else()
  set(SOGEN_IS_SUBPROJECT ON)
endif()

##########################################

if(SKBUILD OR CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
  set(SOGEN_BUILD_STATIC_DEFAULT ON)
else()
  set(SOGEN_BUILD_STATIC_DEFAULT ${SOGEN_IS_SUBPROJECT})
endif()
option(SOGEN_BUILD_STATIC "Build sogen as static libraries for embedding (e.g., IDA plugins)" ${SOGEN_BUILD_STATIC_DEFAULT})
option(SOGEN_DISABLE_NANOBIND_LEAK_WARNINGS "Disable nanobind leak warnings in the Python module" OFF)
if(SOGEN_IS_SUBPROJECT OR SKBUILD)
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
