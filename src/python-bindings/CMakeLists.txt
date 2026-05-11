find_package(Python 3.9 REQUIRED COMPONENTS Interpreter Development.Module OPTIONAL_COMPONENTS Development.SABIModule)

if(NOT TARGET Python::Module)
  add_library(Python::Module INTERFACE IMPORTED)
  if(TARGET Python::SABIModule)
    set_property(TARGET Python::Module PROPERTY INTERFACE_LINK_LIBRARIES Python::SABIModule)
  elseif(TARGET Python::Python)
    set_property(TARGET Python::Module PROPERTY INTERFACE_LINK_LIBRARIES Python::Python)
  endif()
endif()

nanobind_add_module(sogen NB_SUPPRESS_WARNINGS sogen_module.cpp)

target_link_libraries(sogen PRIVATE windows-emulator backend-selection)

target_compile_features(sogen PRIVATE cxx_std_20)
if(MSVC)
  target_compile_options(sogen PRIVATE /wd4714)
  if(TARGET nanobind-static)
    target_compile_options(nanobind-static PRIVATE /wd4714)
  endif()
endif()

nanobind_add_stub(
  sogen_stub
  MODULE sogen
  OUTPUT sogen.pyi
  MARKER_FILE py.typed
  PYTHON_PATH $<TARGET_FILE_DIR:sogen>
  DEPENDS sogen
)

install(TARGETS sogen LIBRARY DESTINATION . RUNTIME DESTINATION .)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/sogen.pyi" "${CMAKE_CURRENT_BINARY_DIR}/py.typed" DESTINATION .)

add_test(NAME sogen-python-smoke COMMAND "${Python_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test.py")
set_tests_properties(sogen-python-smoke PROPERTIES WORKING_DIRECTORY "$<TARGET_FILE_DIR:sogen>")

momo_targets_set_folder("bindings" sogen sogen_stub)
