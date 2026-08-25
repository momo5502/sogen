# seven-emulator SOGEN backend.
# Builds binsnake/seven-jit's libraries (iced_x86, softfloat, seven_core, seven_jit) from an
# out-of-tree source checkout and the glue that implements SOGEN's x86_64_emulator on top of
# seven_jit::JitExecutor. seven_core is a from-scratch C++23 x86 semantic emulator sharing the
# iced-x86 decoder with Mergen; seven_jit adds an asmjit-backed native-codegen fast path on top
# (JIT-compiled hot blocks, interpreter fallback for anything uncovered), which is what makes this
# the reference oracle for VM devirt divergence analysis fast enough to use as the default trace
# backend instead of unicorn.

set(SEVEN_ROOT "C:/Users/vanilla/Desktop/alpha/binsnake/seven-jit" CACHE PATH "seven-jit source root")
set(SEVEN_VENDOR "${SEVEN_ROOT}/vendor/seven-emulator")

if(NOT EXISTS "${SEVEN_VENDOR}/seven_core/include")
  message(FATAL_ERROR "SEVEN_ROOT does not point at a seven-jit checkout: ${SEVEN_ROOT}")
endif()

# SIMD profile — match seven's AVX512 default (emulated in software via wide-integer).
set(_SEVEN_PROFILE "AVX512")
set(_SEVEN_VBITS 512)
set(_SEVEN_VBYTES 64)

# ---- softfloat (C) ----
# seven-jit no longer vendors this locally (it FetchContents the same upstream repo at build time,
# see its top-level CMakeLists.txt) -- match that here instead of expecting a local third_party/ copy.
if(NOT TARGET softfloat)
  include(FetchContent)
  FetchContent_Declare(
    softfloat
    GIT_REPOSITORY https://github.com/ucb-bar/berkeley-softfloat-3.git
    GIT_TAG        master
  )
  FetchContent_GetProperties(softfloat)
  if(NOT softfloat_POPULATED)
    FetchContent_Populate(softfloat)
  endif()
  file(GLOB _SF_SRCS "${softfloat_SOURCE_DIR}/source/*.c" "${softfloat_SOURCE_DIR}/source/8086/*.c")
  list(FILTER _SF_SRCS EXCLUDE REGEX "M\\.c$")
  list(FILTER _SF_SRCS EXCLUDE REGEX "M_")
  set(_SF_PLATDIR "${CMAKE_BINARY_DIR}/softfloat_platform")
  file(MAKE_DIRECTORY "${_SF_PLATDIR}")
  if(MSVC)
    file(WRITE "${_SF_PLATDIR}/platform.h" "#define LITTLEENDIAN 1\n#define INLINE __forceinline\n")
  else()
    file(WRITE "${_SF_PLATDIR}/platform.h" "#define LITTLEENDIAN 1\n#define INLINE static inline\n#define SOFTFLOAT_BUILTIN_CLZ 1\n")
  endif()
  add_library(softfloat STATIC ${_SF_SRCS})
  target_include_directories(softfloat PUBLIC "${softfloat_SOURCE_DIR}/source/include" "${softfloat_SOURCE_DIR}/source/8086")
  target_include_directories(softfloat PRIVATE "${_SF_PLATDIR}")
  target_compile_definitions(softfloat PUBLIC SOFTFLOAT_FAST_INT64)
  sogen_target_disable_warnings(softfloat)
  # SOGEN adds /sdl globally, which promotes C4146/C4996/etc. to hard errors that /W0
  # and /wd cannot suppress. Third-party C/C++ (softfloat, iced) trip these; disable /sdl.
  if(MSVC)
    target_compile_options(softfloat PRIVATE /sdl- /WX-)
  endif()
endif()

# ---- iced_x86 (C++) ----
if(NOT TARGET iced_x86)
  set(_ICED "${SEVEN_VENDOR}/iced_x86/src/iced_x86/src")
  add_library(iced_x86 STATIC
    ${_ICED}/block_encoder.cpp ${_ICED}/decoder.cpp ${_ICED}/encoder.cpp
    ${_ICED}/encoder_handlers.cpp ${_ICED}/encoder_methods.cpp ${_ICED}/encoder_ops.cpp
    ${_ICED}/fastfmt.cpp ${_ICED}/gas_formatter.cpp ${_ICED}/handlers.cpp
    ${_ICED}/instruction.cpp ${_ICED}/instruction_create.cpp ${_ICED}/instruction_info.cpp
    ${_ICED}/intel_formatter.cpp ${_ICED}/masm_formatter.cpp ${_ICED}/memory_size_info.cpp
    ${_ICED}/mvex_info_data.cpp ${_ICED}/nasm_formatter.cpp ${_ICED}/op_code_info.cpp
    ${_ICED}/register_info.cpp ${_ICED}/table_deserializer.cpp ${_ICED}/tables.cpp)
  target_include_directories(iced_x86 PUBLIC "${SEVEN_VENDOR}/iced_x86/include")
  target_compile_definitions(iced_x86 PUBLIC _CRT_SECURE_NO_WARNINGS ICED_X86_CONSTEXPR_HANDLERS=0)
  target_compile_features(iced_x86 PUBLIC cxx_std_23)   # iced uses std::expected (C++23)
  sogen_target_disable_warnings(iced_x86)
  if(MSVC)
    target_compile_options(iced_x86 PUBLIC /Zc:__cplusplus)
    target_compile_options(iced_x86 PRIVATE /sdl- /WX- /permissive-)
  endif()
endif()

# ---- seven_core (C++23) ----
if(NOT TARGET seven_core)
  file(GLOB_RECURSE _SEVEN_SRCS CONFIGURE_DEPENDS "${SEVEN_VENDOR}/seven_core/src/*.cpp")
  add_library(seven_core STATIC ${_SEVEN_SRCS})
  target_include_directories(seven_core PUBLIC "${SEVEN_VENDOR}/seven_core/include" "${SEVEN_VENDOR}")
  target_link_libraries(seven_core PUBLIC iced_x86 softfloat)
  target_compile_features(seven_core PUBLIC cxx_std_23)
  target_compile_definitions(seven_core PUBLIC
    SEVEN_SIMD_PROFILE_${_SEVEN_PROFILE}=1
    SEVEN_ENABLE_AVX=1 SEVEN_ENABLE_AVX2=1 SEVEN_ENABLE_AVX512=1
    SEVEN_VECTOR_BITS=${_SEVEN_VBITS} SEVEN_MAX_VECTOR_BYTES=${_SEVEN_VBYTES}
    SOFTFLOAT_FAST_INT64)
  if(MSVC)
    target_compile_options(seven_core PUBLIC /Zc:__cplusplus /permissive- /EHsc)
    target_compile_options(seven_core PRIVATE /sdl- /WX-)
  endif()
  sogen_target_disable_warnings(seven_core)
endif()

# ---- asmjit (C++, Phase 2 native codegen backend for seven_jit) ----
if(NOT TARGET asmjit::asmjit)
  set(ASMJIT_STATIC ON CACHE BOOL "" FORCE)
  set(ASMJIT_TEST OFF CACHE BOOL "" FORCE)
  # Embedded inside SOGEN's bigger CMake tree (which has its own install()/export() rules), asmjit's
  # own install() rules trip a CMake generate-time check ("INTERFACE_INCLUDE_DIRECTORIES ... which is
  # prefixed in the build directory") over its FetchContent-fetched include path -- a problem
  # seven-jit's own standalone build never hits since it has no install()/export() of its own. Not
  # needed here anyway -- nothing in this build ever installs asmjit as a package.
  set(ASMJIT_NO_INSTALL ON CACHE BOOL "" FORCE)
  FetchContent_Declare(
    asmjit
    GIT_REPOSITORY https://github.com/asmjit/asmjit.git
    GIT_TAG        master
  )
  FetchContent_MakeAvailable(asmjit)
  if(MSVC)
    target_compile_options(asmjit PRIVATE /sdl- /WX-)
  endif()
  sogen_target_disable_warnings(asmjit)
endif()

# ---- seven_jit (C++23): asmjit-backed native codegen on top of seven_core ----
if(NOT TARGET seven_jit)
  add_library(seven_jit STATIC
    "${SEVEN_ROOT}/seven_jit/src/block_compiler.cpp"
    "${SEVEN_ROOT}/seven_jit/src/jit_executor.cpp")
  target_include_directories(seven_jit PUBLIC "${SEVEN_ROOT}/seven_jit/include")
  target_link_libraries(seven_jit PUBLIC seven_core asmjit::asmjit)
  target_compile_features(seven_jit PUBLIC cxx_std_23)
  if(MSVC)
    target_compile_options(seven_jit PUBLIC /Zc:__cplusplus /permissive- /EHsc)
    target_compile_options(seven_jit PRIVATE /sdl- /WX-)
  endif()
  sogen_target_disable_warnings(seven_jit)
endif()

# ---- seven-emulator glue (implements SOGEN x86_64_emulator) ----
file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS *.cpp *.hpp *.rc)
list(SORT SRC_FILES)

if(SOGEN_BUILD_STATIC)
  add_library(seven-emulator STATIC ${SRC_FILES})
else()
  add_library(seven-emulator SHARED ${SRC_FILES})
endif()

target_include_directories(seven-emulator INTERFACE "${CMAKE_CURRENT_LIST_DIR}")
target_compile_features(seven-emulator PRIVATE cxx_std_23)
if(MSVC)
  target_compile_options(seven-emulator PRIVATE /Zc:__cplusplus /permissive- /EHsc /sdl- /WX-)
endif()

target_link_libraries(seven-emulator PUBLIC emulator)
target_link_libraries(seven-emulator PRIVATE emulator-common seven_core seven_jit)

# seven headers are third-party; keep SOGEN's /WX off this TU.
sogen_target_disable_warnings(seven-emulator)

sogen_strip_target(seven-emulator)
