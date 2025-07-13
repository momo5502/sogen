# cross compile
set(CMAKE_SYSTEM_NAME Windows)

set(MINGW_C_COMPILER_NAME "x86_64-w64-mingw32-gcc")
set(MINGW_CXX_COMPILER_NAME "x86_64-w64-mingw32-g++")
set(MINGW_WINDRES_COMPILER_NAME "x86_64-w64-mingw32-windres")

find_file(MINGW_C_COMPILER ${MINGW_C_COMPILER_NAME})
find_file(MINGW_CXX_COMPILER ${MINGW_CXX_COMPILER_NAME})
find_file(MINGW_WINDRES_COMPILER ${MINGW_WINDRES_COMPILER_NAME})

if (${MINGW_C_COMPILER} STREQUAL "MINGW_C_COMPILER-NOTFOUND")
    message(FATAL_ERROR "mingw-w64 compiler not found: ${MINGW_C_COMPILER_NAME}")
endif()
if (${MINGW_CXX_COMPILER} STREQUAL "MINGW_CXX_COMPILER-NOTFOUND")
    message(FATAL_ERROR "mingw-w64 compiler not found: ${MINGW_CXX_COMPILER_NAME}")
endif()
if (${MINGW_WINDRES_COMPILER} STREQUAL "MINGW_WINDRES_COMPILER-NOTFOUND")
    message(FATAL_ERROR "mingw-w64 compiler not found: ${MINGW_WINDRES_COMPILER_NAME}")
endif()

# this macro is needed when compile `libwindows-emulator.a`
add_compile_definitions(NTDDI_VERSION=NTDDI_WIN10_MN)

# set the compiler
set(CMAKE_C_COMPILER  ${MINGW_C_COMPILER})
set(CMAKE_CXX_COMPILER ${MINGW_CXX_COMPILER})
set(CMAKE_RC_COMPILER ${MINGW_WINDRES_COMPILER})

# set the compiler search path
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)

# adjust the default behaviour of the FIND_XXX() commands:
# search headers and libraries in the target environment, search 
# programs in the host environment
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
