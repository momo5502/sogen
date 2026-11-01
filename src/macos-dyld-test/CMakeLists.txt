file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
)

list(SORT SRC_FILES)

add_executable(macos-dyld-test ${SRC_FILES})

sogen_assign_source_group(${SRC_FILES})

target_link_libraries(macos-dyld-test PRIVATE
  gtest
  gtest_main
  macos-emulator
  backend-selection
)

add_test(NAME macos-dyld-test
         COMMAND "${PYTHON3_EXE}" "${PROJECT_SOURCE_DIR}/deps/gtest-parallel/gtest_parallel.py" ./macos-dyld-test
         WORKING_DIRECTORY "$<TARGET_FILE_DIR:macos-dyld-test>")

sogen_targets_set_folder("tests" macos-dyld-test)

sogen_strip_target(macos-dyld-test)
