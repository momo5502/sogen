file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
  *.rc
)

list(SORT SRC_FILES)

add_library(windows-emulator ${SRC_FILES})

sogen_assign_source_group(${SRC_FILES})

if(NOT SOGEN_ENABLE_CLANG_TIDY)
  target_precompile_headers(windows-emulator PRIVATE std_include.hpp)
endif()

target_link_libraries(windows-emulator PUBLIC emulator minidump)

target_include_directories(windows-emulator INTERFACE "${CMAKE_CURRENT_LIST_DIR}")

sogen_strip_target(windows-emulator)
