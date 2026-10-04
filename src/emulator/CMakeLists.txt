file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
  *.rc
)

list(SORT SRC_FILES)

if(EMSCRIPTEN)
  list(FILTER SRC_FILES EXCLUDE REGEX "subprocess_process_manager\\.(cpp|hpp)$")
endif()

add_library(emulator ${SRC_FILES})

target_link_libraries(emulator PUBLIC emulator-common emulator-platform)
if(NOT EMSCRIPTEN)
  target_link_libraries(emulator PRIVATE reproc++)
endif()
target_include_directories(emulator INTERFACE "${CMAKE_CURRENT_LIST_DIR}")
