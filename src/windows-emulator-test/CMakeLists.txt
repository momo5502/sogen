file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
  *.rc
)

list(SORT SRC_FILES)

add_executable(windows-emulator-test ${SRC_FILES})

momo_assign_source_group(${SRC_FILES})

target_link_libraries(windows-emulator-test PRIVATE
  gtest
  common
  windows-emulator
)

add_dependencies(windows-emulator-test test-sample)