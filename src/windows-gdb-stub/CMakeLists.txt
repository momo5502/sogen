file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
  *.rc
)

list(SORT SRC_FILES)

add_library(windows-gdb-stub ${SRC_FILES})

momo_assign_source_group(${SRC_FILES})

target_link_libraries(windows-gdb-stub PUBLIC
  gdb-stub
  windows-emulator
)

target_include_directories(windows-gdb-stub INTERFACE "${CMAKE_CURRENT_LIST_DIR}")

momo_strip_target(windows-gdb-stub)
