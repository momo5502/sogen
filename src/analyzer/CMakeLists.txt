add_executable(analyzer main.cpp)

if(WIN)
  target_sources(analyzer PRIVATE resource.rc)
endif()

sogen_assign_source_group(main.cpp os_selection.hpp)

target_link_libraries(analyzer PRIVATE windows-analyzer linux-analyzer macos-analyzer)

set_property(GLOBAL PROPERTY VS_STARTUP_PROJECT analyzer)

sogen_strip_target(analyzer)

sogen_enable_hypervisor_entitlement(analyzer)

add_test(NAME analyzer-test
         COMMAND "${PYTHON3_EXE}" "${CMAKE_CURRENT_LIST_DIR}/../windows-analyzer/test.py"
         WORKING_DIRECTORY "$<TARGET_FILE_DIR:analyzer>")

add_test(NAME macos-trace-test
         COMMAND "${PYTHON3_EXE}" "${CMAKE_CURRENT_LIST_DIR}/../macos-analyzer/test.py"
         WORKING_DIRECTORY "$<TARGET_FILE_DIR:analyzer>")

set_tests_properties(macos-trace-test PROPERTIES
                     ENVIRONMENT "MACOS_TRACE_FIXTURE=${CMAKE_CURRENT_LIST_DIR}/../macos-emulator-test/fixtures/macho_trace_arm64")
