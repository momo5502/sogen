file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
  *.rc
)

list(SORT SRC_FILES)

add_executable(dump-apiset ${SRC_FILES})

sogen_assign_source_group(${SRC_FILES})

target_link_libraries(dump-apiset PRIVATE
  emulator-common
  emulator-platform
)

sogen_strip_target(dump-apiset)
