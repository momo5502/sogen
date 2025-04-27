file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
  *.rc
)

list(SORT SRC_FILES)

add_executable(debugger ${SRC_FILES})

momo_assign_source_group(${SRC_FILES})

if(NOT MOMO_ENABLE_CLANG_TIDY)
  target_precompile_headers(debugger PRIVATE std_include.hpp)
endif()

target_link_libraries(debugger PRIVATE
  windows-emulator
  flatbuffers
)

set_property(GLOBAL PROPERTY VS_STARTUP_PROJECT debugger)

momo_strip_target(debugger)
