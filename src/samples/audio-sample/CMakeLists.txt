file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
  *.rc
)

list(SORT SRC_FILES)

add_executable(audio-sample ${SRC_FILES})

target_link_libraries(audio-sample PRIVATE ole32)

sogen_assign_source_group(${SRC_FILES})
