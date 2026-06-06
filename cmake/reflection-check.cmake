include_guard()
include(CheckCXXSourceCompiles)

function(sogen_check_reflection_support)
  if(NOT SOGEN_ENABLE_REFLECTION)
    return()
  endif()

  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    return()
  endif()

  set(CMAKE_REQUIRED_INCLUDES
    "${CMAKE_SOURCE_DIR}/deps/reflect"
    "${CMAKE_SOURCE_DIR}/src/windows-analyzer"
  )

  set(CMAKE_REQUIRED_QUIET OFF)
  set(_sogen_old_try_compile_target_type "${CMAKE_TRY_COMPILE_TARGET_TYPE}")
  set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

  check_cxx_source_compiles([[#include <type_traits>
#include <utility>
#include "reflect_extension.hpp"
#include <reflect>

struct sogen_reflection_probe {
  int a;
  long long b;
};

static_assert(reflect::size<sogen_reflection_probe>() == 2);
static_assert(reflect::offset_of<0, sogen_reflection_probe>() == 0);
static_assert(reflect::offset_of<1, sogen_reflection_probe>() >= sizeof(int));
static_assert(reflect::type_name<sogen_reflection_probe>().size() > 0);

void sogen_reflection_probe_use() {
  reflect::for_each<sogen_reflection_probe>([](auto) {});
}

int main() {
  sogen_reflection_probe_use();
  return 0;
}
]] SOGEN_REFLECTION_SUPPORTED)

  set(CMAKE_TRY_COMPILE_TARGET_TYPE "${_sogen_old_try_compile_target_type}")
  unset(_sogen_old_try_compile_target_type)
  unset(CMAKE_REQUIRED_INCLUDES)

  if(NOT SOGEN_REFLECTION_SUPPORTED)
    message(WARNING "SOGEN_ENABLE_REFLECTION was requested, but the compiler failed to compile the reflect header. Reflection will be disabled automatically.")
    set(SOGEN_ENABLE_REFLECTION OFF CACHE BOOL "Enable reflection using the reflect library" FORCE)
  endif()
endfunction()
