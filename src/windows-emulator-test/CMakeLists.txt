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
  windows-emulator
)

if(WIN32)
  add_dependencies(windows-emulator-test test-sample)
endif()

add_test(NAME windows-emulator-test
         COMMAND windows-emulator-test
         WORKING_DIRECTORY "$<TARGET_FILE_DIR:windows-emulator-test>")

momo_targets_set_folder("tests" windows-emulator-test)