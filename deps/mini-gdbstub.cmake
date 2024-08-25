file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  mini-gdbstub/lib/*.c
)

list(SORT SRC_FILES)

add_library(mini-gdbstub ${SRC_FILES})

target_include_directories(mini-gdbstub PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/mini-gdbstub/include"
)
