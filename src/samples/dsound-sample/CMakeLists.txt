file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
  *.rc
)

list(SORT SRC_FILES)

add_executable(dsound-sample ${SRC_FILES})

target_link_libraries(dsound-sample PRIVATE ole32 dsound dxguid)

sogen_assign_source_group(${SRC_FILES})
