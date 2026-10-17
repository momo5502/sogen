sogen_get_all_targets(EXISTING_TARGETS)

##########################################

add_subdirectory(bad-sample)
add_subdirectory(hook-sample)
add_subdirectory(test-sample)
add_subdirectory(vulkan-shim)
add_subdirectory(vulkan-shim-test)
add_subdirectory(steam-shim)
if(MSVC AND CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "ARM64")
  add_subdirectory(arm64-hello-world)
endif()

##########################################

sogen_get_all_targets(ALL_TARGETS)
sogen_list_difference("${ALL_TARGETS}" "${EXISTING_TARGETS}" SAMPLE_TARGETS)
sogen_targets_remove_compile_options("${SAMPLE_TARGETS}" /arch:AVX2 -mavx2)
