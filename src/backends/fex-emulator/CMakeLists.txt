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

# FEXCore is built in-tree from the deps/FEX submodule (see deps/CMakeLists.txt). Link its static
# library target; it carries the FEXCore public include directory and transitive deps PUBLICly.
target_include_directories(fex-emulator PRIVATE "${CMAKE_SOURCE_DIR}/deps/FEX/FEXCore/include")
target_link_libraries(fex-emulator PRIVATE FEXCore_Base)

sogen_strip_target(fex-emulator)
