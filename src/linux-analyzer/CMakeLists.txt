file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
)

list(SORT SRC_FILES)

add_executable(linux-analyzer ${SRC_FILES})

momo_assign_source_group(${SRC_FILES})

target_link_libraries(linux-analyzer PRIVATE linux-emulator linux-gdb-stub backend-selection)

momo_strip_target(linux-analyzer)
