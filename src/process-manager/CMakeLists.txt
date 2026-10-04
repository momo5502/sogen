if(EMSCRIPTEN)
  add_library(process-manager INTERFACE)
  target_link_libraries(process-manager INTERFACE emulator)
  target_include_directories(process-manager INTERFACE "${CMAKE_CURRENT_LIST_DIR}")
else()
  file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
    *.cpp
    *.hpp
    *.rc
  )

  list(SORT SRC_FILES)

  add_library(process-manager ${SRC_FILES})

  sogen_assign_source_group(${SRC_FILES})

  target_link_libraries(process-manager PUBLIC emulator)
  target_link_libraries(process-manager PRIVATE emulator-common)
  target_link_libraries(process-manager PRIVATE flatbuffers reproc++)
  target_include_directories(process-manager INTERFACE "${CMAKE_CURRENT_LIST_DIR}")
  sogen_strip_target(process-manager)
endif()

add_custom_target(generate-process-manager-flatbuffer
  DEPENDS flatc
  COMMAND "$<TARGET_FILE:flatc>" --gen-object-api --filename-ext hxx --cpp -o "${CMAKE_CURRENT_LIST_DIR}" "${CMAKE_CURRENT_LIST_DIR}/process_protocol.fbs"
)
