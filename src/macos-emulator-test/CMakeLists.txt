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
  emulator-common
  emulator-platform
  macos-emulator
  unicorn-emulator
  backend-selection
  ui-backends
)

set(SOGEN_MACOS_HOST_ROOT "/" CACHE STRING "Emulation root used by the root-dependent macOS tests")

target_compile_definitions(macos-emulator-test PRIVATE
  MACOS_HOST_ROOT="${SOGEN_MACOS_HOST_ROOT}"
  MACOS_FIXTURE_ROOT="${CMAKE_CURRENT_LIST_DIR}/fixtures"
  MACOS_DYLD_HOST_PATH="/usr/lib/dyld"
  MACOS_DYLD_CACHE_HOST_PATH="/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld/dyld_shared_cache_arm64e"
)

add_test(NAME macos-emulator-test
         COMMAND "${PYTHON3_EXE}" "${PROJECT_SOURCE_DIR}/deps/gtest-parallel/gtest_parallel.py" ./macos-emulator-test
         WORKING_DIRECTORY "$<TARGET_FILE_DIR:macos-emulator-test>")

sogen_targets_set_folder("tests" macos-emulator-test)

sogen_strip_target(macos-emulator-test)
