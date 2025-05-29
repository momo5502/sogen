file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
  *.rc
)

list(SORT SRC_FILES)

add_executable(test-sample ${SRC_FILES})

if(CMAKE_C_COMPILER MATCHES "/.*/x86_64-w64-mingw32-gcc$")
  target_link_options(test-sample PRIVATE -static-libstdc++ -static -lwinpthread)
  target_link_libraries(test-sample PRIVATE ws2_32)
endif()

momo_assign_source_group(${SRC_FILES})
