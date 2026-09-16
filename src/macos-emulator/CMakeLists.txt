file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
)

list(SORT SRC_FILES)

add_library(macos-emulator ${SRC_FILES})

sogen_assign_source_group(${SRC_FILES})

if(NOT SOGEN_ENABLE_CLANG_TIDY)
  target_precompile_headers(macos-emulator PRIVATE std_include.hpp)
endif()

target_link_libraries(macos-emulator PUBLIC emulator ui-backends)

target_include_directories(macos-emulator INTERFACE "${CMAKE_CURRENT_LIST_DIR}")

sogen_strip_target(macos-emulator)
