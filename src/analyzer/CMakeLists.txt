add_executable(analyzer main.cpp)

sogen_assign_source_group(main.cpp)

target_link_libraries(analyzer PRIVATE windows-analyzer linux-analyzer)

set_property(GLOBAL PROPERTY VS_STARTUP_PROJECT analyzer)

sogen_strip_target(analyzer)

add_test(NAME analyzer-test
         COMMAND "${PYTHON3_EXE}" "${CMAKE_CURRENT_LIST_DIR}/../windows-analyzer/test.py"
         WORKING_DIRECTORY "$<TARGET_FILE_DIR:analyzer>")
