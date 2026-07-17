include_guard()
include(CheckCXXCompilerFlag)

##########################################
# System identification

set(OSX OFF)
set(LINUX OFF)
set(WIN OFF)

if(CMAKE_SYSTEM_NAME MATCHES "Linux")
    set(LINUX ON)
elseif(CMAKE_SYSTEM_NAME MATCHES "Darwin")
    set(OSX ON)
elseif(CMAKE_SYSTEM_NAME MATCHES "Windows")
    set(WIN ON)
endif()

##########################################

cmake_policy(SET CMP0069 NEW) 
set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

##########################################

if(SOGEN_ENABLE_LTO AND NOT SOGEN_ENABLE_CLANG_TIDY AND NOT MINGW AND NOT CMAKE_SYSTEM_NAME MATCHES "Emscripten")
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_DEBUG OFF)
endif()

##########################################

if(SOGEN_BUILD_STATIC)
    add_compile_definitions(SOGEN_BUILD_STATIC=1)
else()
    add_compile_definitions(SOGEN_BUILD_STATIC=0)
endif()

##########################################

if(SOGEN_ENABLE_FUZZING)
    add_compile_definitions(SOGEN_ENABLE_FUZZING)
endif()

##########################################

set(SOGEN_ENABLE_RUST OFF)
if(SOGEN_ENABLE_RUST_CODE AND NOT MINGW AND NOT CMAKE_SYSTEM_NAME MATCHES "Emscripten")
  find_program(CARGO cargo)
  if(CARGO)
    set(SOGEN_ENABLE_RUST ON)
  else()
    message(STATUS "cargo not found; disabling Rust-backed components")
  endif()
endif()

##########################################

if(SOGEN_ENABLE_RUST)
  add_compile_definitions(SOGEN_ENABLE_RUST_CODE=1)
else()
  add_compile_definitions(SOGEN_ENABLE_RUST_CODE=0)
endif()

##########################################

if(UNIX)
  sogen_add_c_and_cxx_compile_options(
    -fvisibility=hidden
    -ftrivial-auto-var-init=zero
  )
endif()

##########################################

if(MINGW)
  add_link_options(
    -static-libstdc++
    -static-libgcc
    -static
    -lwinpthread
  )

  sogen_add_c_and_cxx_compile_options(
    -Wno-array-bounds
  )
endif()

##########################################

if(LINUX OR APPLE)
  sogen_add_c_and_cxx_compile_options(
    -fdiagnostics-color=always
  )

  sogen_add_c_and_cxx_release_compile_options(
    -ffunction-sections
    -fdata-sections
    -fstack-protector-strong
  )
  
  if(LINUX)
    add_link_options(
      -Wl,--no-undefined
      -Wl,-z,now
      -Wl,-z,noexecstack
      -static-libstdc++
    )

    sogen_add_release_link_options(
      -Wl,--gc-sections
    )
  else()
    sogen_add_release_link_options(
      -dead_strip
    )
  endif()

  add_compile_definitions(
    _REENTRANT
    _THREAD_SAFE
  )

  if(NOT SOGEN_ENABLE_SANITIZER)
    # Hardened toolchains (e.g. Gentoo hardened) predefine _FORTIFY_SOURCE via
    # their spec files. Redefining it triggers a -Werror "redefined" failure, so
    # only set it when the compiler hasn't already done so. This also lets users
    # keep their own (often stronger) level.
    include(CheckCXXSourceCompiles)
    check_cxx_source_compiles([[
#ifdef _FORTIFY_SOURCE
#error _FORTIFY_SOURCE already defined
#endif
int main() { return 0; }
]] SOGEN_FORTIFY_SOURCE_UNSET)

    if(SOGEN_FORTIFY_SOURCE_UNSET)
      add_compile_definitions(
        _FORTIFY_SOURCE=2
      )
    endif()
  endif()

  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -pie")

  # Embed an rpath so binaries find their colocated shared libraries without
  # requiring LD_LIBRARY_PATH / DYLD_LIBRARY_PATH. All sogen artifacts (the
  # executables and the shared libs they depend on) share one output directory,
  # so an $ORIGIN/@loader_path rpath lets every binary locate its libraries
  # relative to itself. CMAKE_BUILD_RPATH adds it on top of the auto-computed
  # build-tree rpath, which also covers transitive dependencies the linker only
  # records as (non-transitive) DT_RUNPATH. It keeps working after the output
  # directory is copied elsewhere.
  set(CMAKE_BUILD_WITH_INSTALL_RPATH OFF)
  set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)
  set(CMAKE_INSTALL_RPATH_USE_LINK_PATH ON)

  if(APPLE)
    set(CMAKE_BUILD_RPATH "@loader_path")
    set(CMAKE_INSTALL_RPATH "@loader_path")
  else()
    set(CMAKE_BUILD_RPATH "$ORIGIN")
    set(CMAKE_INSTALL_RPATH "$ORIGIN")
  endif()
endif()

##########################################

if(CMAKE_SYSTEM_NAME MATCHES "Emscripten")
  sogen_add_c_and_cxx_compile_options(
    -fexceptions
    -ftrivial-auto-var-init=zero
    -Wno-dollar-in-identifier-extension
  )

  add_link_options(
    -fexceptions
    -sALLOW_MEMORY_GROWTH=1
    -sGROWABLE_ARRAYBUFFERS=0
    $<$<CONFIG:Debug>:-sASSERTIONS>
    -sWASM_BIGINT
    #-sUSE_OFFSET_CONVERTER
    #-sEXCEPTION_CATCHING_ALLOWED=[..]
    -sEXIT_RUNTIME
    -sASYNCIFY
  )

  if(SOGEN_EMSCRIPTEN_MEMORY64)
    sogen_add_c_and_cxx_compile_options(
      -sMEMORY64
    )

    add_link_options(
      -sMAXIMUM_MEMORY=8gb
      -sMEMORY64
    )
  else()
    add_link_options(
      -sMAXIMUM_MEMORY=4gb
    )
  endif()

  if(SOGEN_EMSCRIPTEN_SUPPORT_NODEJS)
    add_compile_definitions(
      SOGEN_EMSCRIPTEN_SUPPORT_NODEJS=1
    )

    add_link_options(
      -lnodefs.js -sNODERAWFS=1
      -sENVIRONMENT=node
      --pre-js ${CMAKE_CURRENT_LIST_DIR}/misc/node-pre-script.js
    )
  else() 
    add_link_options(
      -lidbfs.js
      -sENVIRONMENT=worker
      -sINVOKE_RUN=0
      -sEXPORTED_RUNTIME_METHODS=['callMain']
    )
  endif()
endif()

##########################################

if(MSVC)
  string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
  string(REPLACE "/EHs" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")

  sogen_add_c_and_cxx_compile_options(
    /sdl
    /GS
    /Gy
    /EHa
    #/guard:cf
  )

  sogen_add_compile_options(CXX
    /Zc:__cplusplus
  )

  sogen_add_c_and_cxx_release_compile_options(
    /Gw
  )

  sogen_add_release_link_options(
    /OPT:REF
    /OPT:ICF
  )

  add_link_options(
    /INCREMENTAL:NO
  )

  add_compile_definitions(
    _CRT_SECURE_NO_WARNINGS
    _CRT_NONSTDC_NO_WARNINGS
  )
endif()

##########################################

if(SOGEN_ENABLE_AVX2 AND NOT (CMAKE_SYSTEM_NAME STREQUAL "Android"))
  set(CMAKE_REQUIRED_FLAGS -Werror)
  check_cxx_compiler_flag(-mavx2 COMPILER_SUPPORTS_MAVX2)
  set(CMAKE_REQUIRED_FLAGS "")

  check_cxx_compiler_flag(/arch:AVX2 COMPILER_SUPPORTS_ARCH_AVX2)

  if(COMPILER_SUPPORTS_MAVX2)
    sogen_add_c_and_cxx_compile_options(-mavx2)
  endif()

  if (COMPILER_SUPPORTS_ARCH_AVX2)
    sogen_add_c_and_cxx_compile_options(/arch:AVX2)
  endif()
endif()

##########################################

if(SOGEN_ENABLE_SANITIZER)
  sogen_add_c_and_cxx_compile_options(-fsanitize=address)
  add_link_options(-fsanitize=address)
endif()

# Instrument all code with libFuzzer edge coverage so the fuzzer sees the emulator's own handlers,
# not just the harness. The fuzz executable adds -fsanitize=fuzzer at link time; pair with
# SOGEN_ENABLE_SANITIZER=On for ASAN. libFuzzer is clang-only; on other toolchains only the
# uninstrumented standalone replay driver is built (still useful for smoke/regression).
if(SOGEN_ENABLE_FUZZING AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  sogen_add_c_and_cxx_compile_options(-fsanitize=fuzzer-no-link)
endif()

##########################################
# MSVC Runtime Library Selection
#
# Default is dynamic runtime (/MD or /MDd) to enforce shared allocators
# between emulator and implementation.
#
# Use SOGEN_STATIC_CRT=ON for static runtime (/MT or /MTd) when embedding
# in projects that require it (e.g., IDA plugins).
#
# WARNING: Static CRT may cause heap corruption if memory is allocated
# in one module and freed in another. Ensure allocation ownership is clear.

option(SOGEN_STATIC_CRT "Use static CRT (/MT) instead of dynamic (/MD)" OFF)

if(SOGEN_STATIC_CRT AND NOT SOGEN_BUILD_STATIC)
  message(FATAL_ERROR
    "SOGEN_STATIC_CRT=ON requires SOGEN_BUILD_STATIC=ON.\n"
    "Static CRT with shared libraries causes heap corruption - "
    "each DLL gets its own allocator, but sogen passes ownership across boundaries.")
endif()

if(SOGEN_STATIC_CRT)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
elseif(DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
  # Respect parent project's setting
else()
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

##########################################

if(MSVC)
  add_link_options(
    $<$<NOT:$<STREQUAL:${CMAKE_MSVC_RUNTIME_LIBRARY},MultiThreaded>>:/NODEFAULTLIB:libcmt.lib>
    $<$<NOT:$<STREQUAL:${CMAKE_MSVC_RUNTIME_LIBRARY},MultiThreadedDLL>>:/NODEFAULTLIB:msvcrt.lib>
    $<$<NOT:$<STREQUAL:${CMAKE_MSVC_RUNTIME_LIBRARY},MultiThreadedDebug>>:/NODEFAULTLIB:libcmtd.lib>
    $<$<NOT:$<STREQUAL:${CMAKE_MSVC_RUNTIME_LIBRARY},MultiThreadedDebugDLL>>:/NODEFAULTLIB:msvcrtd.lib>
  )
endif()

##########################################

if(CMAKE_GENERATOR MATCHES "Visual Studio")
  sogen_add_c_and_cxx_compile_options(/MP)
endif()
