file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
  *.rc
)

list(SORT SRC_FILES)

add_executable(vulkan-snapshot-test ${SRC_FILES})

sogen_assign_source_group(${SRC_FILES})

target_link_libraries(vulkan-snapshot-test PRIVATE vulkan-headers)
sogen_target_skip_clang_tidy(vulkan-snapshot-test)
