set(UNICORN_ARCH "x86" CACHE STRING "")

add_subdirectory(unicorn)

##########################################

add_library(reflect INTERFACE)
target_include_directories(reflect INTERFACE
    "${CMAKE_CURRENT_LIST_DIR}/reflect"
)

##########################################

include(googletest.cmake)
include(zlib.cmake)
