file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
)

list(SORT SRC_FILES)

add_executable(macos-analyzer-test ${SRC_FILES})

sogen_assign_source_group(${SRC_FILES})

target_link_libraries(macos-analyzer-test PRIVATE
  gtest
  gtest_main
  emulator-common
  emulator-platform
  macos-analyzer
  macos-emulator
  unicorn-emulator
  backend-selection
)

target_include_directories(macos-analyzer-test PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../analyzer")

target_compile_definitions(macos-analyzer-test PRIVATE
  MACOS_FIXTURE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../macos-emulator-test/fixtures"
)

add_test(NAME macos-analyzer-test
         COMMAND "${PYTHON3_EXE}" "${PROJECT_SOURCE_DIR}/deps/gtest-parallel/gtest_parallel.py" ./macos-analyzer-test
         WORKING_DIRECTORY "$<TARGET_FILE_DIR:macos-analyzer-test>")

sogen_targets_set_folder("tests" macos-analyzer-test)

sogen_strip_target(macos-analyzer-test)
