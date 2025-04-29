file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
  *.rc
)

list(SORT SRC_FILES)

add_library(debugger ${SRC_FILES})

momo_assign_source_group(${SRC_FILES})

target_link_libraries(debugger PRIVATE
  windows-emulator
  flatbuffers
  base64
)

target_include_directories(debugger INTERFACE "${CMAKE_CURRENT_LIST_DIR}")

add_custom_target(generate-flatbuffer
  DEPENDS flatc
  COMMAND "$<TARGET_FILE:flatc>" --gen-mutable --gen-object-api --filename-ext hxx --cpp -o "${CMAKE_CURRENT_LIST_DIR}" "${CMAKE_CURRENT_LIST_DIR}/events.fbs"
  COMMAND "$<TARGET_FILE:flatc>" --gen-mutable --gen-object-api --ts -o "${PROJECT_SOURCE_DIR}/page/src/fb" "${CMAKE_CURRENT_LIST_DIR}/events.fbs"
)
