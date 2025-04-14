if (NOT CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
  set(UNICORN_ARCH "x86" CACHE STRING "")
  add_subdirectory(unicorn)
endif()

##########################################

add_library(reflect INTERFACE)
target_include_directories(reflect INTERFACE
    "${CMAKE_CURRENT_LIST_DIR}/reflect"
)

##########################################

include(googletest.cmake)
include(zlib.cmake)
