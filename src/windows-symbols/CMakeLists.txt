file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
)
list(SORT SRC_FILES)

add_library(windows-symbols STATIC ${SRC_FILES})
sogen_assign_source_group(${SRC_FILES})
target_include_directories(windows-symbols PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(windows-symbols PRIVATE windows-emulator emulator-common raw_pdb cab-decoder)
if(NOT EMSCRIPTEN)
  target_link_libraries(windows-symbols PRIVATE CURL::libcurl_static)
endif()
