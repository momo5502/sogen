add_executable(symbol-file-test symbol_file_test.cpp)
target_link_libraries(symbol-file-test PRIVATE symbol-files emulator-common gtest_main)
add_test(NAME symbol-file-test COMMAND symbol-file-test)
sogen_targets_set_folder("tests" symbol-file-test)
