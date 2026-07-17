sogen_get_all_targets(EXISTING_TARGETS)

##########################################

add_subdirectory(bad-sample)
add_subdirectory(hook-sample)
add_subdirectory(test-sample)
add_subdirectory(vulkan-shim)
add_subdirectory(vulkan-shim-test)
add_subdirectory(steam-shim)
add_subdirectory(audio-sample)
add_subdirectory(dsound-sample)

##########################################

sogen_get_all_targets(ALL_TARGETS)
sogen_list_difference("${ALL_TARGETS}" "${EXISTING_TARGETS}" SAMPLE_TARGETS)
sogen_targets_remove_compile_options("${SAMPLE_TARGETS}" /arch:AVX2 -mavx2)
