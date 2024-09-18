file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
  *.rc
)

list(SORT SRC_FILES)

add_library(windows_emulator ${SRC_FILES})

momo_assign_source_group(${SRC_FILES})

target_precompile_headers(windows_emulator PRIVATE std_include.hpp)

target_link_libraries(windows_emulator PRIVATE
  common
  unicorn_emulator
  mini-gdbstub
)

target_link_libraries(windows_emulator PUBLIC
  emulator
  phnt::phnt
)

target_include_directories(windows_emulator INTERFACE
    "${CMAKE_CURRENT_LIST_DIR}"
)

momo_strip_target(windows_emulator)
