file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
  *.rc
)

list(SORT SRC_FILES)

add_executable(bad-sample ${SRC_FILES})
target_link_libraries(bad-sample PRIVATE emulator-common)

momo_assign_source_group(${SRC_FILES})
