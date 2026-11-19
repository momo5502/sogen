file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
  *.rc
)

set(SYMBOL_FILE_SOURCES
  "${CMAKE_CURRENT_SOURCE_DIR}/symbols/pdb_file.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/symbols/cab_file.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/symbols/symbol_download.cpp"
)
list(REMOVE_ITEM SRC_FILES ${SYMBOL_FILE_SOURCES})
list(SORT SRC_FILES)

add_library(symbol-files STATIC ${SYMBOL_FILE_SOURCES})
target_include_directories(symbol-files PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(symbol-files PRIVATE emulator-common raw_pdb cab-decoder)
if(NOT EMSCRIPTEN)
  target_link_libraries(symbol-files PRIVATE CURL::libcurl_static)
endif()

add_library(windows-analyzer STATIC ${SRC_FILES})

sogen_assign_source_group(${SRC_FILES} ${SYMBOL_FILE_SOURCES})

if(NOT SOGEN_ENABLE_CLANG_TIDY)
  target_precompile_headers(windows-analyzer PRIVATE std_include.hpp)
endif()

target_link_libraries(windows-analyzer PRIVATE
  debugger
  disassembler
  capstone
  windows-emulator
  process-manager
  windows-gdb-stub
  backend-selection
  cli11
)

if(WIN)
  # CLI11's ensure_utf8() resolves the UTF-8 command line through CommandLineToArgvW (shell32).
  target_link_libraries(windows-analyzer PRIVATE shell32)
endif()

# main.cpp is a large translation unit (CLI11 plus the analysis code) that exceeds the COFF section
# limit in Debug builds without the large-object format.
if(MSVC)
  target_compile_options(windows-analyzer PRIVATE /bigobj)
elseif(MINGW)
  target_compile_options(windows-analyzer PRIVATE -Wa,-mbig-obj)
endif()

if(SOGEN_ENABLE_REFLECTION)
  target_link_libraries(windows-analyzer PRIVATE
    reflect
  )
endif()

target_link_libraries(windows-analyzer PRIVATE symbol-files)
