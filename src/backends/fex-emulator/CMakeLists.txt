file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
  *.rc
)

list(SORT SRC_FILES)

if(SOGEN_BUILD_STATIC)
	add_library(fex-emulator STATIC ${SRC_FILES})
else()
    add_library(fex-emulator SHARED ${SRC_FILES})
endif()

target_include_directories(fex-emulator INTERFACE "${CMAKE_CURRENT_LIST_DIR}")

target_link_libraries(fex-emulator PUBLIC emulator)
target_link_libraries(fex-emulator PRIVATE emulator-common)

# FEXCore is not vendored as a submodule (it is a large, ARM64-host-specific dependency). Locate a
# prebuilt/installed FEXCore. Either point SOGEN_FEX_ROOT at a FEX checkout/install prefix, or have
# FEXCore discoverable via CMake's package config / pkg-config.
find_package(FEXCore CONFIG QUIET)

if(FEXCore_FOUND)
  target_link_libraries(fex-emulator PRIVATE FEXCore::FEXCore)
else()
  if(NOT SOGEN_FEX_ROOT)
    message(FATAL_ERROR
      "SOGEN_ENABLE_FEX is ON but FEXCore was not found. Set -DSOGEN_FEX_ROOT=<path to FEX install/checkout> "
      "or make FEXCore discoverable via find_package(FEXCore).")
  endif()

  find_path(FEXCORE_INCLUDE_DIR
    NAMES FEXCore/Core/Context.h
    HINTS "${SOGEN_FEX_ROOT}/include" "${SOGEN_FEX_ROOT}/FEXCore/include")

  find_library(FEXCORE_LIBRARY
    NAMES FEXCore
    HINTS "${SOGEN_FEX_ROOT}/lib" "${SOGEN_FEX_ROOT}/build" "${SOGEN_FEX_ROOT}")

  if(NOT FEXCORE_INCLUDE_DIR OR NOT FEXCORE_LIBRARY)
    message(FATAL_ERROR "Could not locate FEXCore headers/library under SOGEN_FEX_ROOT=${SOGEN_FEX_ROOT}")
  endif()

  target_include_directories(fex-emulator PRIVATE "${FEXCORE_INCLUDE_DIR}")
  target_link_libraries(fex-emulator PRIVATE "${FEXCORE_LIBRARY}")
endif()

sogen_strip_target(fex-emulator)
