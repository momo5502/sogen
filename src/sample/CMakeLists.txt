file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
  *.rc
)

list(SORT SRC_FILES)

add_executable(sample ${SRC_FILES})

momo_assign_source_group(${SRC_FILES})

target_precompile_headers(sample PRIVATE std_include.hpp)

target_link_libraries(sample PRIVATE
  common
  reflect
  windows-emulator
)

set_property(GLOBAL PROPERTY VS_STARTUP_PROJECT sample)

momo_strip_target(sample)
