file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
  *.rc
)

list(SORT SRC_FILES)

add_executable(emulator ${SRC_FILES})

momo_assign_source_group(${SRC_FILES})

target_precompile_headers(emulator PRIVATE std_include.hpp)

target_link_libraries(emulator PRIVATE
  common
  unicorn
  phnt::phnt
  reflect
)

set_property(GLOBAL PROPERTY VS_STARTUP_PROJECT emulator)

momo_strip_target(emulator)
