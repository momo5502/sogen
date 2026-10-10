file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
)

list(SORT SRC_FILES)

add_executable(macos-emulator-test ${SRC_FILES})

sogen_assign_source_group(${SRC_FILES})

target_link_libraries(macos-emulator-test PRIVATE
  gtest
  gtest_main
  macos-emulator
  backend-selection
)

add_test(NAME macos-emulator-test
         COMMAND "${PYTHON3_EXE}" "${PROJECT_SOURCE_DIR}/deps/gtest-parallel/gtest_parallel.py" ./macos-emulator-test
         WORKING_DIRECTORY "$<TARGET_FILE_DIR:macos-emulator-test>")

sogen_targets_set_folder("tests" macos-emulator-test)

sogen_strip_target(macos-emulator-test)
