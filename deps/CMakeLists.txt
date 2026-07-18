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

add_library(cli11 INTERFACE)
target_include_directories(cli11 INTERFACE
  "${CMAKE_CURRENT_LIST_DIR}/CLI11/include"
)

if(NOT WIN)
  # CLI11's codecvt conversion path uses the deprecated std::wstring_convert, which fails under
  # -Werror on non-Windows toolchains (e.g. Emscripten). It is only needed for the Windows wide
  # command line, so use CLI11's locale-based fallback everywhere else.
  target_compile_definitions(cli11 INTERFACE CLI11_HAS_CODECVT=0)
endif()

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

# FEX-Emu (x86 -> AArch64 JIT). Built only when the FEX backend is enabled (ARM64 Linux/Darwin +
# Clang, resolved at the top level). FEX is not designed to be embedded via add_subdirectory: it
# assumes it is the top-level project (~60 uses of CMAKE_SOURCE_DIR) and configures its whole
# loader/tools tree. Instead build it standalone via ExternalProject and consume the resulting
# self-contained shared FEXCore library (the FEXCore_shared target). Only that target is built -
# not the FEX tools.
if(SOGEN_ENABLE_FEX)
  include(ExternalProject)

  set(_FEX_SRC "${CMAKE_CURRENT_SOURCE_DIR}/FEX")

  if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(_FEXCORE_SHARED_LIB "libFEXCore.dylib")
    set(_FEXCORE_OSX_ARGS -DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET})
  else()
    set(_FEXCORE_SHARED_LIB "libFEXCore.so")
    set(_FEXCORE_OSX_ARGS "")
  endif()

  # Propagate AddressSanitizer into the FEXCore build so the whole chain is instrumented
  # consistently (mismatched ASan instrumentation across shared libraries causes false positives).
  if(SOGEN_ENABLE_SANITIZER)
    set(_FEXCORE_SANITIZER_ARGS -DENABLE_ASAN=ON)
  else()
    set(_FEXCORE_SANITIZER_ARGS "")
  endif()

  ExternalProject_Add(fex_external
    SOURCE_DIR "${_FEX_SRC}"
    CMAKE_GENERATOR "Ninja"
    CMAKE_ARGS
      -DCMAKE_BUILD_TYPE=Release
      -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
      -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
      ${_FEXCORE_OSX_ARGS}
      ${_FEXCORE_SANITIZER_ARGS}
      -DENABLE_LTO=OFF
      -DENABLE_CCACHE=OFF
      -DBUILD_FEXCONFIG=OFF
      -DBUILD_THUNKS=OFF
      -DBUILD_FEX_LINUX_TESTS=OFF
      -DBUILD_TESTING=OFF
      -DENABLE_OFFLINE_TELEMETRY=OFF
      # Do not let FEX replace the process allocator; sogen owns it when FEXCore is embedded.
      -DENABLE_FEX_ALLOCATOR=OFF
      -DENABLE_JEMALLOC_GLIBC_ALLOC=OFF
    # Build only the self-contained shared core library, not FEX's loader/server tools.
    BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target FEXCore_shared
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS <BINARY_DIR>/FEXCore/Source/${_FEXCORE_SHARED_LIB}
  )

  ExternalProject_Get_property(fex_external BINARY_DIR)

  # CI builds and tests run in separate jobs/runners, exchanging only the artifacts output
  # directory (build/<preset>/artifacts/) as an uploaded/downloaded tarball - the ExternalProject's
  # own build tree (where fexcore's IMPORTED_LOCATION below points) never leaves the build job's
  # runner. Copy the shared library into the artifacts directory too, alongside fex-emulator's own
  # output, so the test job's @loader_path-relative rpath resolves it without needing that
  # build-tree path to exist.
  add_custom_command(
    TARGET fex_external POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy "${BINARY_DIR}/FEXCore/Source/${_FEXCORE_SHARED_LIB}" "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}"
    COMMENT "Copying libFEXCore shared library to the artifacts directory"
  )

  # CMake validates that every directory in an IMPORTED target's INTERFACE_INCLUDE_DIRECTORIES
  # exists at generate time. ${BINARY_DIR}/include (FEXCore's build-time-generated headers, see
  # below) doesn't exist yet on a fresh checkout - it's only created once the fex_external
  # ExternalProject actually builds - so create an empty placeholder now; the real generated
  # headers land in the same directory later, when FEXCore_shared builds.
  file(MAKE_DIRECTORY "${BINARY_DIR}/include")

  # Imported view of the FEXCore shared library for consumers (fex-emulator). The consuming target
  # must also add_dependencies(... fex_external) so the ExternalProject builds first.
  #
  # FEXCore's public headers transitively include FEX's vendored fmt (LogManager.h) and
  # unordered_dense (fextl/robin_map.h), so consumers need those include dirs too. FMT_HEADER_ONLY
  # keeps fmt fully inline in the consumer TU, avoiding a link dependency on fmt symbols that the
  # hidden-visibility FEXCore library does not re-export. FEXCore also generates some of its own
  # headers at build time (Config/ConfigValues.inl, IR/IRDefines.inc, ...) into its binary dir's
  # include/ - consumers need that include dir too.
  add_library(fexcore SHARED IMPORTED GLOBAL)
  set_target_properties(fexcore PROPERTIES
    IMPORTED_LOCATION "${BINARY_DIR}/FEXCore/Source/${_FEXCORE_SHARED_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${_FEX_SRC}/FEXCore/include;${BINARY_DIR}/include;${_FEX_SRC}/External/fmt/include;${_FEX_SRC}/External/unordered_dense/include"
    INTERFACE_COMPILE_DEFINITIONS "FMT_HEADER_ONLY=1")
endif()
