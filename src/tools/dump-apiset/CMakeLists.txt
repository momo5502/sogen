file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
  *.rc
)

list(SORT SRC_FILES)

add_executable(dump-apiset ${SRC_FILES})

momo_assign_source_group(${SRC_FILES})

if(CMAKE_C_COMPILER MATCHES "/.*/x86_64-w64-mingw32-gcc$")
  target_link_options(dump-apiset PRIVATE -static-libstdc++ -static -lwinpthread)
endif()

target_link_libraries(dump-apiset PRIVATE
  emulator-common
)

momo_strip_target(dump-apiset)
