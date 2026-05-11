pybind11_add_module(sogen MODULE sogen_module.cpp)

target_link_libraries(sogen PRIVATE
  windows-emulator
  backend-selection
)

target_compile_features(sogen PRIVATE cxx_std_20)

add_test(NAME sogen-python-smoke
         COMMAND "${PYTHON3_EXE}" "${CMAKE_CURRENT_LIST_DIR}/test.py"
         WORKING_DIRECTORY "$<TARGET_FILE_DIR:sogen>")

momo_targets_set_folder("bindings" sogen)

momo_strip_target(sogen)
