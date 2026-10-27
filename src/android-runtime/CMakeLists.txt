if(NOT SOGEN_ENABLE_FEX AND NOT SOGEN_ENABLE_RUST)
  message(FATAL_ERROR "The Android runtime needs FEX (ARM64 hosts) or Icicle (Rust) to run x86-64 guests")
endif()

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
  process-manager
  vulkan-headers
  android
  log
)

if(TARGET adrenotools)
  target_link_libraries(android-runtime PRIVATE adrenotools)
  target_compile_definitions(android-runtime PRIVATE SOGEN_HAS_ADRENOTOOLS=1)
  # adrenotools dlopens its hooks at runtime from the app's native library directory, so they have to ship
  # next to libsogen.so even though nothing links them.
  add_dependencies(android-runtime main_hook file_redirect_hook gsl_alloc_hook)
endif()

sogen_strip_target(android-runtime)
