file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
  *.rc
)

list(SORT SRC_FILES)
list(REMOVE_ITEM SRC_FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/windows_architecture_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/arm64_backend_test.cpp"
)

add_executable(arm64-backend-test arm64_backend_test.cpp)
target_link_libraries(arm64-backend-test PRIVATE windows-emulator backend-selection gtest gtest_main)
add_test(NAME arm64-backend-test COMMAND arm64-backend-test)
set_tests_properties(arm64-backend-test PROPERTIES TIMEOUT 30)
sogen_targets_set_folder("tests" arm64-backend-test)
sogen_enable_hypervisor_entitlement(arm64-backend-test)
sogen_target_skip_clang_tidy(arm64-backend-test)

add_executable(windows-architecture-test windows_architecture_test.cpp)
target_link_libraries(windows-architecture-test PRIVATE windows-emulator backend-selection gtest gtest_main)
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND NOT MINGW)
  if(MSVC AND CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "ARM64")
    set(NATIVE_TEST_SAMPLE arm64-hello-world)
  else()
    set(NATIVE_TEST_SAMPLE test-sample)
  endif()
  add_dependencies(windows-architecture-test ${NATIVE_TEST_SAMPLE})
  target_compile_definitions(windows-architecture-test PRIVATE
    SOGEN_NATIVE_SAMPLE_PATH="$<TARGET_FILE:${NATIVE_TEST_SAMPLE}>")
endif()
add_test(NAME windows-architecture-test COMMAND windows-architecture-test)
set_tests_properties(windows-architecture-test PROPERTIES TIMEOUT 30)
sogen_targets_set_folder("tests" windows-architecture-test)
sogen_enable_hypervisor_entitlement(windows-architecture-test)
sogen_target_skip_clang_tidy(windows-architecture-test)

add_executable(windows-emulator-test ${SRC_FILES})

sogen_assign_source_group(${SRC_FILES})

target_link_libraries(windows-emulator-test PRIVATE
  gtest
  gtest_main
  windows-emulator
  backend-selection
  gpu-bridge-protocol
  vulkan-bridge-marshal
  debugger
)

if(EMSCRIPTEN)
  target_link_options(windows-emulator-test PRIVATE -sASYNCIFY_EXPORTS=main)
endif()

if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
  add_dependencies(windows-emulator-test test-sample)
endif()

if(MSVC AND CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "ARM64")
  # ARM64 hosts have no x86-64 backend, so LTCG proves the code after x86-64 emulator creation unreachable.
  target_compile_options(windows-emulator-test PRIVATE /wd4702)
endif()

add_test(NAME windows-emulator-test
         COMMAND "${PYTHON3_EXE}" "${PROJECT_SOURCE_DIR}/deps/gtest-parallel/gtest_parallel.py" ./windows-emulator-test
         WORKING_DIRECTORY "$<TARGET_FILE_DIR:windows-emulator-test>")

sogen_targets_set_folder("tests" windows-emulator-test)

sogen_strip_target(windows-emulator-test)
sogen_enable_hypervisor_entitlement(windows-emulator-test)
sogen_target_skip_clang_tidy(windows-emulator-test)
