file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
  *.rc
)

list(SORT SRC_FILES)

add_executable(vulkan-window-sample ${SRC_FILES})

sogen_assign_source_group(${SRC_FILES})

target_link_libraries(vulkan-window-sample PRIVATE vulkan-headers)
