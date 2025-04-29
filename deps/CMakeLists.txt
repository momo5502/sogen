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
target_compile_options(flatc PRIVATE /MD)
endif()

##########################################

add_library(reflect INTERFACE)
target_include_directories(reflect INTERFACE
    "${CMAKE_CURRENT_LIST_DIR}/reflect"
)

##########################################

include(googletest.cmake)
include(zlib.cmake)
