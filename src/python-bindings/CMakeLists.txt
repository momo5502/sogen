find_package(Python 3.9 REQUIRED COMPONENTS Interpreter Development.Module OPTIONAL_COMPONENTS Development.SABIModule)

if(NOT TARGET Python::Module)
  add_library(Python::Module INTERFACE IMPORTED)
  if(TARGET Python::SABIModule)
    set_property(TARGET Python::Module PROPERTY INTERFACE_LINK_LIBRARIES Python::SABIModule)
  elseif(TARGET Python::Python)
    set_property(TARGET Python::Module PROPERTY INTERFACE_LINK_LIBRARIES Python::Python)
  endif()
endif()

file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

nanobind_add_module(sogen NB_SUPPRESS_WARNINGS ${SRC_FILES})

if(SOGEN_DISABLE_NANOBIND_LEAK_WARNINGS)
  target_compile_definitions(sogen PRIVATE SOGEN_DISABLE_NANOBIND_LEAK_WARNINGS=1)
endif()

target_link_libraries(sogen PRIVATE windows-emulator linux-emulator disassembler backend-selection)

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
  set(SOGEN_PYTHON_INSTALL_DESTINATION "${SKBUILD_PLATLIB_DIR}")
else()
  set(SOGEN_PYTHON_INSTALL_DESTINATION ".")
endif()

install(TARGETS sogen LIBRARY DESTINATION "${SOGEN_PYTHON_INSTALL_DESTINATION}" RUNTIME DESTINATION "${SOGEN_PYTHON_INSTALL_DESTINATION}")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/sogen.pyi" "${CMAKE_CURRENT_BINARY_DIR}/py.typed" DESTINATION "${SOGEN_PYTHON_INSTALL_DESTINATION}")

# fexcore (see deps/CMakeLists.txt) is built by an ExternalProject and never becomes a normal
# install()-able CMake target of its own; sogen only ever links it in as fex-emulator's transitive
# private dependency. The wheel/package only ships whatever gets install()ed here, so without this,
# `pip install`ing the wheel produces a package missing libFEXCore entirely - it's present in the
# build tree (copied alongside sogen for the in-tree/test case) but never travels into the package.
if(SOGEN_ENABLE_FEX)
  get_target_property(_fexcore_lib_path fexcore IMPORTED_LOCATION)
  get_filename_component(_fexcore_lib_name "${_fexcore_lib_path}" NAME)
  install(FILES "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/${_fexcore_lib_name}" DESTINATION "${SOGEN_PYTHON_INSTALL_DESTINATION}")
  unset(_fexcore_lib_path)
  unset(_fexcore_lib_name)
endif()

sogen_targets_set_folder("bindings" sogen sogen_stub)
