find_package(Python 3.9 REQUIRED COMPONENTS Interpreter Development.Module OPTIONAL_COMPONENTS Development.SABIModule)

if(NOT TARGET Python::Module)
  add_library(Python::Module INTERFACE IMPORTED)
  if(TARGET Python::SABIModule)
    set_property(TARGET Python::Module PROPERTY INTERFACE_LINK_LIBRARIES Python::SABIModule)
  elseif(TARGET Python::Python)
    set_property(TARGET Python::Module PROPERTY INTERFACE_LINK_LIBRARIES Python::Python)
  endif()
endif()

nanobind_add_module(sogen NB_SUPPRESS_WARNINGS sogen_module.cpp sogen_bindings.cpp)

target_link_libraries(sogen PRIVATE windows-emulator backend-selection)

target_compile_features(sogen PRIVATE cxx_std_20)
if(MSVC)
  target_compile_options(sogen PRIVATE /wd4714)
  if(TARGET nanobind-static)
    target_compile_options(nanobind-static PRIVATE /wd4714)
  endif()
elseif(UNIX AND NOT APPLE)
  # Override the global -z defs / --no-undefined for the Python extension.
  target_link_options(sogen PRIVATE -Wl,-z,undefs)
endif()

nanobind_add_stub(
  sogen_stub
  MODULE sogen
  OUTPUT sogen.pyi
  MARKER_FILE py.typed
  PYTHON_PATH $<TARGET_FILE_DIR:sogen>
  DEPENDS sogen
)

if(SKBUILD)
  install(TARGETS sogen LIBRARY DESTINATION "${SKBUILD_PLATLIB_DIR}" RUNTIME DESTINATION "${SKBUILD_PLATLIB_DIR}")
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/sogen.pyi" "${CMAKE_CURRENT_BINARY_DIR}/py.typed" DESTINATION "${SKBUILD_PLATLIB_DIR}")
else()
  install(TARGETS sogen LIBRARY DESTINATION . RUNTIME DESTINATION .)
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/sogen.pyi" "${CMAKE_CURRENT_BINARY_DIR}/py.typed" DESTINATION .)
endif()

momo_targets_set_folder("bindings" sogen sogen_stub)
