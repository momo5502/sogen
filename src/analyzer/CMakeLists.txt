file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
  *.rc
)

list(SORT SRC_FILES)

add_library(windows-analyzer STATIC ${SRC_FILES})

sogen_assign_source_group(${SRC_FILES})

if(NOT SOGEN_ENABLE_CLANG_TIDY)
  target_precompile_headers(windows-analyzer PRIVATE std_include.hpp)
endif()

target_link_libraries(windows-analyzer PRIVATE
  debugger
  disassembler
  capstone
  windows-emulator
  windows-gdb-stub
  backend-selection
)

if(SOGEN_ENABLE_REFLECTION)
  target_link_libraries(windows-analyzer PRIVATE
    reflect
  )
endif()
