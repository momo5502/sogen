include_guard()
include(CheckCXXSourceCompiles)

function(momo_check_reflection_support)
  if(NOT MOMO_ENABLE_REFLECTION)
    return()
  endif()

  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    return()
  endif()

  set(CMAKE_REQUIRED_INCLUDES
    "${CMAKE_SOURCE_DIR}/deps/reflect"
    "${CMAKE_SOURCE_DIR}/src/analyzer"
  )

  set(CMAKE_REQUIRED_QUIET OFF)
  set(_momo_old_try_compile_target_type "${CMAKE_TRY_COMPILE_TARGET_TYPE}")
  set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

  check_cxx_source_compiles([[#include <type_traits>
#include <utility>
#include "reflect_extension.hpp"
#include <reflect>

struct momo_reflection_probe {
  int a;
  long long b;
};

static_assert(reflect::size<momo_reflection_probe>() == 2);
static_assert(reflect::offset_of<0, momo_reflection_probe>() == 0);
static_assert(reflect::offset_of<1, momo_reflection_probe>() >= sizeof(int));
static_assert(reflect::type_name<momo_reflection_probe>().size() > 0);

void momo_reflection_probe_use() {
  reflect::for_each<momo_reflection_probe>([](auto) {});
}

int main() {
  momo_reflection_probe_use();
  return 0;
}
]] MOMO_REFLECTION_SUPPORTED)

  set(CMAKE_TRY_COMPILE_TARGET_TYPE "${_momo_old_try_compile_target_type}")
  unset(_momo_old_try_compile_target_type)
  unset(CMAKE_REQUIRED_INCLUDES)

  if(NOT MOMO_REFLECTION_SUPPORTED)
    message(WARNING "MOMO_ENABLE_REFLECTION was requested, but the compiler failed to compile the reflect header. Reflection will be disabled automatically.")
    set(MOMO_ENABLE_REFLECTION OFF CACHE BOOL "Enable reflection using the reflect library" FORCE)
  endif()
endfunction()
