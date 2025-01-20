file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
  *.rc
)

list(SORT SRC_FILES)

add_executable(analyzer ${SRC_FILES})

momo_assign_source_group(${SRC_FILES})

target_precompile_headers(analyzer PRIVATE std_include.hpp)

target_link_libraries(analyzer PRIVATE
  reflect
  windows-emulator
  windows-gdb-stub
)

set_property(GLOBAL PROPERTY VS_STARTUP_PROJECT analyzer)

momo_strip_target(analyzer)
