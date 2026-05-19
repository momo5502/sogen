file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
)

add_library(disassembler ${SRC_FILES})

sogen_assign_source_group(${SRC_FILES})

target_include_directories(disassembler PUBLIC "${CMAKE_CURRENT_LIST_DIR}" "${CMAKE_SOURCE_DIR}/src/emulator" "${CMAKE_SOURCE_DIR}/src/common")

target_link_libraries(disassembler PUBLIC emulator-common capstone)
