file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
  *.rc
)

list(SORT SRC_FILES)

add_library(unicorn_emulator SHARED ${SRC_FILES})

target_include_directories(unicorn_emulator INTERFACE
    "${CMAKE_CURRENT_LIST_DIR}"
)

target_link_libraries(unicorn_emulator PUBLIC emulator)
target_link_libraries(unicorn_emulator PRIVATE unicorn)
