file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
)

list(SORT SRC_FILES)

add_library(macos-analyzer STATIC ${SRC_FILES})

sogen_assign_source_group(${SRC_FILES})

target_include_directories(macos-analyzer INTERFACE "${CMAKE_CURRENT_LIST_DIR}")

target_link_libraries(macos-analyzer PUBLIC emulator-common PRIVATE macos-emulator backend-selection cli11)
