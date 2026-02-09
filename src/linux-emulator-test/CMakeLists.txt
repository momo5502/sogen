file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
  *.rc
)

list(SORT SRC_FILES)

add_executable(linux-emulator-test ${SRC_FILES})

momo_assign_source_group(${SRC_FILES})

target_link_libraries(linux-emulator-test PRIVATE
  gtest
  gtest_main
  linux-emulator
  backend-selection
)

add_test(NAME linux-emulator-test
         COMMAND ${CMAKE_COMMAND} -E env "LD_LIBRARY_PATH=." -- "${PYTHON3_EXE}" "${PROJECT_SOURCE_DIR}/deps/gtest-parallel/gtest_parallel.py" ./linux-emulator-test
         WORKING_DIRECTORY "$<TARGET_FILE_DIR:linux-emulator-test>")

momo_targets_set_folder("tests" linux-emulator-test)

momo_strip_target(linux-emulator-test)
