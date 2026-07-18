file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
  *.rc
)

list(SORT SRC_FILES)

add_executable(speaker-config-sample ${SRC_FILES})

target_link_libraries(speaker-config-sample PRIVATE ole32 dsound dxguid winmm)

sogen_assign_source_group(${SRC_FILES})
