file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
)

list(SORT SRC_FILES)

add_executable(arm64-backend-test ${SRC_FILES})

sogen_assign_source_group(${SRC_FILES})

target_link_libraries(arm64-backend-test PRIVATE
  gtest
  gtest_main
  unicorn
  emulator
  unicorn-emulator
  backend-selection
)

add_test(NAME arm64-backend-test
         COMMAND "${PYTHON3_EXE}" "${PROJECT_SOURCE_DIR}/deps/gtest-parallel/gtest_parallel.py" ./arm64-backend-test
         WORKING_DIRECTORY "$<TARGET_FILE_DIR:arm64-backend-test>")

sogen_targets_set_folder("tests" arm64-backend-test)

sogen_strip_target(arm64-backend-test)

if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
  target_link_libraries(arm64-backend-test PRIVATE "-framework Hypervisor")
endif()

sogen_enable_hypervisor_entitlement(arm64-backend-test)

if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
  target_link_libraries(arm64-backend-test PRIVATE "-framework Hypervisor" hvf-emulator)
endif()
