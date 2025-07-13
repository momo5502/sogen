file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
  *.rc
)

list(SORT SRC_FILES)

add_executable(test-sample ${SRC_FILES})

if(WIN)
  target_link_libraries(test-sample PRIVATE ws2_32)
endif()

momo_assign_source_group(${SRC_FILES})
