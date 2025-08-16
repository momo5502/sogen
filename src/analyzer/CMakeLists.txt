file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
  *.rc
)

list(SORT SRC_FILES)

add_executable(analyzer ${SRC_FILES})

momo_assign_source_group(${SRC_FILES})

if(NOT MOMO_ENABLE_CLANG_TIDY)
  target_precompile_headers(analyzer PRIVATE std_include.hpp)
endif()

target_link_libraries(analyzer PRIVATE
  reflect
  debugger
  capstone
  windows-emulator
  windows-gdb-stub
  backend-selection
)

set_property(GLOBAL PROPERTY VS_STARTUP_PROJECT analyzer)

momo_strip_target(analyzer)

set(ENV_PREFIX "$")
set(ENV_SUFFIX "")

if(WIN)
set(ENV_PREFIX "%")
set(ENV_SUFFIX "%")
endif()

add_test(NAME analyzer-test
         COMMAND "${PYTHON3_EXE}" "${CMAKE_CURRENT_LIST_DIR}/test.py"
         WORKING_DIRECTORY "$<TARGET_FILE_DIR:analyzer>")
