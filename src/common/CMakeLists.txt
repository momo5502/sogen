file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
)

add_library(emulator-common ${SRC_FILES})

momo_assign_source_group(${SRC_FILES})

target_include_directories(emulator-common INTERFACE "${CMAKE_CURRENT_LIST_DIR}")

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

if(CMAKE_C_COMPILER MATCHES "/.*/x86_64-w64-mingw32-gcc$")
  target_link_libraries(emulator-common PUBLIC
    Threads::Threads
    zlibstatic
    ws2_32
  )
else()
  target_link_libraries(emulator-common PUBLIC
    Threads::Threads
    zlibstatic
  )
endif()
