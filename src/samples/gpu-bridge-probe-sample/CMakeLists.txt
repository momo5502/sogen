file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
  *.rc
)

list(SORT SRC_FILES)

add_executable(gpu-bridge-probe-sample ${SRC_FILES})

sogen_assign_source_group(${SRC_FILES})

target_link_libraries(gpu-bridge-probe-sample PRIVATE gpu-bridge-protocol)
