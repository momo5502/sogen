file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
  *.rc
)

list(SORT SRC_FILES)

add_library(emulator ${SRC_FILES})

target_link_libraries(emulator PUBLIC emulator-common)
target_include_directories(emulator INTERFACE "${CMAKE_CURRENT_LIST_DIR}")
