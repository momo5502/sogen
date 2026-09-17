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

# FEXCore is built standalone via ExternalProject (see deps/CMakeLists.txt) and consumed as the
# imported FEXCore shared library. The dependency forces the ExternalProject to build before this target.
target_link_libraries(fex-emulator PRIVATE fexcore)
add_dependencies(fex-emulator fex_external)

sogen_strip_target(fex-emulator)
