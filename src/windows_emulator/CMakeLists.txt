file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
  *.rc
)

list(SORT SRC_FILES)

add_executable(windows_emulator ${SRC_FILES})

momo_assign_source_group(${SRC_FILES})

target_precompile_headers(windows_emulator PRIVATE std_include.hpp)

target_link_libraries(windows_emulator PRIVATE
  common
  phnt::phnt
  reflect
  unicorn_emulator
  mini-gdbstub
)

set_property(GLOBAL PROPERTY VS_STARTUP_PROJECT windows_emulator)

momo_strip_target(windows_emulator)
