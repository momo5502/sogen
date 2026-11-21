add_executable(windows-symbols-test symbol_file_test.cpp)
target_link_libraries(windows-symbols-test PRIVATE windows-symbols emulator-common gtest_main)
add_test(NAME windows-symbols-test COMMAND windows-symbols-test)
sogen_targets_set_folder("tests" windows-symbols-test)
