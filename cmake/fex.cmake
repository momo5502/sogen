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
