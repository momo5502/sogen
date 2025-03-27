file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
  *.rc
)

list(SORT SRC_FILES)

add_library(windows-emulator ${SRC_FILES})

momo_assign_source_group(${SRC_FILES})

if(NOT MOMO_ENABLE_CLANG_TIDY)
  target_precompile_headers(windows-emulator PRIVATE std_include.hpp)
endif()

target_link_libraries(windows-emulator PRIVATE
  unicorn-emulator
  icicle-emulator
)

target_link_libraries(windows-emulator PUBLIC emulator)

target_include_directories(windows-emulator INTERFACE "${CMAKE_CURRENT_LIST_DIR}")

momo_strip_target(windows-emulator)
