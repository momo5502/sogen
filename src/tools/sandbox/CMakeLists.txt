if(NOT WIN OR MINGW OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  return()
endif()

file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
  *.rc
)

list(SORT SRC_FILES)

add_executable(sandbox ${SRC_FILES})

sogen_assign_source_group(${SRC_FILES})

target_link_libraries(sandbox PRIVATE
  windows-emulator
  whp-emulator
  emulator-common
)

sogen_strip_target(sandbox)
