file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
)

list(SORT SRC_FILES)

add_library(linux-gdb-stub ${SRC_FILES})

momo_assign_source_group(${SRC_FILES})

target_link_libraries(linux-gdb-stub PUBLIC
  gdb-stub
  linux-emulator
)

target_include_directories(linux-gdb-stub INTERFACE "${CMAKE_CURRENT_LIST_DIR}")

momo_strip_target(linux-gdb-stub)
