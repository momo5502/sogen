file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
)

list(SORT SRC_FILES)

add_library(linux-analyzer STATIC ${SRC_FILES})

sogen_assign_source_group(${SRC_FILES})

target_link_libraries(linux-analyzer PRIVATE linux-emulator linux-gdb-stub linux-debugger backend-selection cli11)
