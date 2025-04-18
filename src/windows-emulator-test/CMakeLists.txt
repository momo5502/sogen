file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
  *.rc
)

list(SORT SRC_FILES)

add_executable(windows-emulator-test ${SRC_FILES})

momo_assign_source_group(${SRC_FILES})

target_link_libraries(windows-emulator-test PRIVATE
  gtest
  gtest_main
  windows-emulator
)

if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
  add_dependencies(windows-emulator-test test-sample)
endif()

set(TEST_COMMAND "./windows-emulator-test")

if(CMAKE_SYSTEM_NAME MATCHES "Emscripten")
  set(TEST_COMMAND node "./windows-emulator-test.js")
endif()

add_test(NAME windows-emulator-test
         COMMAND "${PYTHON3_EXE}" "${PROJECT_SOURCE_DIR}/deps/gtest-parallel/gtest_parallel.py" ${TEST_COMMAND}
         WORKING_DIRECTORY "$<TARGET_FILE_DIR:windows-emulator-test>")

momo_targets_set_folder("tests" windows-emulator-test)

momo_strip_target(windows-emulator-test)
