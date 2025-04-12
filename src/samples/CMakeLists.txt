momo_get_all_targets(EXISTING_TARGETS)

##########################################

add_subdirectory(bad-sample)
add_subdirectory(test-sample)

##########################################

momo_get_all_targets(ALL_TARGETS)
momo_list_difference("${ALL_TARGETS}" "${EXISTING_TARGETS}" SAMPLE_TARGETS)
momo_targets_remove_compile_options("${SAMPLE_TARGETS}" /arch:AVX2 -mavx2)