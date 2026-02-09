file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
)

list(SORT SRC_FILES)

add_library(linux-debugger ${SRC_FILES})

momo_assign_source_group(${SRC_FILES})

target_link_libraries(linux-debugger PRIVATE
  linux-emulator
  debugger
  flatbuffers
  base64
)

target_include_directories(linux-debugger INTERFACE "${CMAKE_CURRENT_LIST_DIR}")
