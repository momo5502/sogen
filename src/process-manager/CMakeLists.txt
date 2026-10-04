file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
  *.rc
)

if(EMSCRIPTEN)
  list(FILTER SRC_FILES EXCLUDE REGEX "subprocess_process_manager\\.(cpp|hpp)$")
endif()

list(SORT SRC_FILES)

add_library(process-manager ${SRC_FILES})

sogen_assign_source_group(${SRC_FILES})

target_link_libraries(process-manager PUBLIC emulator)
target_link_libraries(process-manager PRIVATE emulator-common)

if(NOT EMSCRIPTEN)
  target_link_libraries(process-manager PRIVATE flatbuffers reproc++)
endif()

target_include_directories(process-manager INTERFACE "${CMAKE_CURRENT_LIST_DIR}")

add_custom_target(generate-process-manager-flatbuffer
  DEPENDS flatc
  COMMAND "$<TARGET_FILE:flatc>" --gen-object-api --filename-ext hxx --cpp -o "${CMAKE_CURRENT_LIST_DIR}" "${CMAKE_CURRENT_LIST_DIR}/process_protocol.fbs"
)

sogen_strip_target(process-manager)
