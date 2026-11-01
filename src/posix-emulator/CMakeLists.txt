file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
)

list(SORT SRC_FILES)

add_library(posix-emulator ${SRC_FILES})

sogen_assign_source_group(${SRC_FILES})

target_link_libraries(posix-emulator PUBLIC emulator)

target_include_directories(posix-emulator INTERFACE "${CMAKE_CURRENT_LIST_DIR}")

sogen_strip_target(posix-emulator)
