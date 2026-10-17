file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
)

list(SORT SRC_FILES)

add_library(android-runtime SHARED ${SRC_FILES})

sogen_assign_source_group(${SRC_FILES})

if(NOT SOGEN_ENABLE_CLANG_TIDY)
  target_precompile_headers(android-runtime PRIVATE std_include.hpp)
endif()

set_target_properties(android-runtime PROPERTIES OUTPUT_NAME sogen)

target_link_libraries(android-runtime PRIVATE
  windows-emulator
  backend-selection
  vulkan-headers
  android
  log
)

if(TARGET adrenotools)
  target_link_libraries(android-runtime PRIVATE adrenotools)
  target_compile_definitions(android-runtime PRIVATE SOGEN_HAS_ADRENOTOOLS=1)
endif()

sogen_strip_target(android-runtime)
