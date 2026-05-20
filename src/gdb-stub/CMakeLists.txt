file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
  *.rc
)

list(SORT SRC_FILES)

add_library(gdb-stub ${SRC_FILES})

sogen_assign_source_group(${SRC_FILES})

target_link_libraries(gdb-stub PUBLIC
  emulator-common
  emulator-platform
)

target_include_directories(gdb-stub INTERFACE "${CMAKE_CURRENT_LIST_DIR}")

sogen_strip_target(gdb-stub)
