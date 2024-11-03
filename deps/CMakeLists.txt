set(UNICORN_ARCH "x86" CACHE STRING "")

add_subdirectory(unicorn)
add_subdirectory(phnt)

##########################################

add_library(reflect INTERFACE)
target_include_directories(reflect INTERFACE
    "${CMAKE_CURRENT_LIST_DIR}/reflect"
)

##########################################

include(mini-gdbstub.cmake)
include(googletest.cmake)
