file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
)

list(SORT SRC_FILES)

add_library(macos-analyzer STATIC ${SRC_FILES})

sogen_assign_source_group(${SRC_FILES})

target_link_libraries(macos-analyzer PRIVATE macos-emulator backend-selection cli11)
