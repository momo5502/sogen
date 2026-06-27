set(UNICORN_ARCH "x86" CACHE STRING "")
add_subdirectory(unicorn)

##########################################

option(BASE64_ENABLE_TESTING "" OFF)
add_subdirectory(base64)

##########################################

option(FLATBUFFERS_BUILD_TESTS "" OFF)
option(FLATBUFFERS_INSTALL "" OFF)
add_subdirectory(flatbuffers)

if(MSVC)
target_compile_options(flatc PRIVATE /MD$<$<CONFIG:Debug>:d>)
endif()

##########################################

if(SOGEN_ENABLE_REFLECTION)
  add_library(reflect INTERFACE)
  target_include_directories(reflect INTERFACE
    "${CMAKE_CURRENT_LIST_DIR}/reflect"
  )
endif()

##########################################

add_subdirectory(minidump_cpp)

##########################################

add_library(vulkan-headers INTERFACE)
target_include_directories(vulkan-headers INTERFACE
  "${CMAKE_CURRENT_LIST_DIR}/Vulkan-Headers/include"
)

##########################################

if(SOGEN_ENABLE_SDL3 AND NOT EMSCRIPTEN)
  # Prefer a system-installed SDL3 (find_package) when allowed and present;
  # otherwise build the vendored submodule. Emscripten uses the web UI backend
  # and is excluded above.
  if(SOGEN_USE_SYSTEM_SDL3)
    find_package(SDL3 CONFIG QUIET)
  endif()

  if(TARGET SDL3::SDL3)
    message(STATUS "Using system SDL3 ${SDL3_VERSION}")
  elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/SDL/CMakeLists.txt")
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT SDL_UNIX_CONSOLE_BUILD)
      # Require both display backends on Linux: Wayland for desktop sessions, and X11 for environments
      # that only expose Xwayland (e.g. the Steam Deck's Gaming Mode under gamescope, which does not set
      # WAYLAND_DISPLAY). Forcing them on turns a missing dev package into a hard configure error instead
      # of a silently dropped backend that leaves the app with no window at runtime. The Python wheel build
      # opts out via SDL_UNIX_CONSOLE_BUILD, since it intentionally builds without these dev libraries.
      set(SDL_WAYLAND ON CACHE BOOL "" FORCE)
      set(SDL_X11 ON CACHE BOOL "" FORCE)
    endif()
    add_subdirectory(SDL)
    message(STATUS "Using vendored SDL3 submodule (deps/SDL)")
  endif()
endif()

##########################################

if(SOGEN_ENABLE_PYTHON_BINDINGS)
  if (NOT TARGET Python::Module)
    if(CMAKE_VERSION VERSION_LESS 3.18)
      set(_SOGEN_PYTHON_DEV_MODULE Development)
    else()
      set(_SOGEN_PYTHON_DEV_MODULE Development.Module)
    endif()

    find_package(Python 3.9 REQUIRED COMPONENTS Interpreter ${_SOGEN_PYTHON_DEV_MODULE} OPTIONAL_COMPONENTS Development.SABIModule)
  endif()

  if (NOT TARGET Python::Module)
    add_library(Python::Module INTERFACE IMPORTED)
    if (TARGET Python::SABIModule)
      set_property(TARGET Python::Module PROPERTY INTERFACE_LINK_LIBRARIES Python::SABIModule)
    elseif (TARGET Python::Python)
      set_property(TARGET Python::Module PROPERTY INTERFACE_LINK_LIBRARIES Python::Python)
    endif()
  endif()

  add_subdirectory(nanobind)
endif()

##########################################

option(CAPSTONE_BUILD_MACOS_THIN "" ON)
option(CAPSTONE_X86_SUPPORT "" ON)
option(CAPSTONE_X86_ATT_DISABLE "" ON)
option(CAPSTONE_ARCHITECTURE_DEFAULT "" OFF)
option(CAPSTONE_BUILD_STATIC_MSVC_RUNTIME "" OFF)
add_subdirectory(capstone)

##########################################

include(googletest.cmake)
include(zstd.cmake)

##########################################

# FEX-Emu (x86 -> AArch64 JIT). Only built when the FEX backend is enabled (ARM64 Linux + Clang,
# resolved at the top level). FEX is a full CMake project of its own; add it EXCLUDE_FROM_ALL so only
# the FEXCore_Base library and its transitive dependencies build when fex-emulator links them - not
# FEX's FEXLoader/FEXServer tools. Optional/embedder-hostile features are trimmed off.
if(SOGEN_ENABLE_FEX)
  set(BUILD_FEXCONFIG OFF CACHE BOOL "" FORCE)
  set(BUILD_THUNKS OFF CACHE BOOL "" FORCE)
  set(BUILD_FEX_LINUX_TESTS OFF CACHE BOOL "" FORCE)
  set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
  set(ENABLE_LTO OFF CACHE BOOL "" FORCE)
  set(ENABLE_CCACHE OFF CACHE BOOL "" FORCE)
  set(ENABLE_OFFLINE_TELEMETRY OFF CACHE BOOL "" FORCE)
  # Do not let FEX replace the process allocator; sogen owns it when FEXCore is embedded.
  set(ENABLE_FEX_ALLOCATOR OFF CACHE BOOL "" FORCE)
  set(ENABLE_JEMALLOC_GLIBC_ALLOC OFF CACHE BOOL "" FORCE)
  add_subdirectory(FEX EXCLUDE_FROM_ALL)
endif()
