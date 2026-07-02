# Windows uses the WHP backend, Linux x64 uses the KVM backend. Both are 64-bit only.
if((NOT WIN AND NOT LINUX) OR MINGW OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  return()
endif()

file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
)

if(WIN)
  file(GLOB_RECURSE RC_FILES CONFIGURE_DEPENDS *.rc)
  list(APPEND SRC_FILES ${RC_FILES})
endif()

list(SORT SRC_FILES)

add_executable(sandbox ${SRC_FILES})

sogen_assign_source_group(${SRC_FILES})

target_link_libraries(sandbox PRIVATE
  windows-emulator
  emulator-common
  cli11
)

if(WIN)
  target_link_libraries(sandbox PRIVATE whp-emulator shell32)
else()
  target_link_libraries(sandbox PRIVATE kvm-emulator)
endif()

sogen_strip_target(sandbox)
