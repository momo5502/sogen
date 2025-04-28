file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
  *.rc
)

list(SORT SRC_FILES)

add_executable(debugger ${SRC_FILES})

momo_assign_source_group(${SRC_FILES})

if(NOT MOMO_ENABLE_CLANG_TIDY)
  target_precompile_headers(debugger PRIVATE std_include.hpp)
endif()

target_link_libraries(debugger PRIVATE
  windows-emulator
  flatbuffers
  base64
)

set_property(GLOBAL PROPERTY VS_STARTUP_PROJECT debugger)

momo_strip_target(debugger)

add_custom_target(gen_fbs
  DEPENDS flatc
  COMMAND "$<TARGET_FILE:flatc>" --gen-mutable --gen-object-api --filename-ext hxx --cpp -o "${CMAKE_CURRENT_LIST_DIR}" "${CMAKE_CURRENT_LIST_DIR}/events.fbs"
  COMMAND "$<TARGET_FILE:flatc>" --gen-mutable --gen-object-api --ts -o "${PROJECT_SOURCE_DIR}/page/src/fb" "${CMAKE_CURRENT_LIST_DIR}/events.fbs"
)
