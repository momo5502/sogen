file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
  *.rc
)

list(SORT SRC_FILES)

add_library(fuzzing-engine ${SRC_FILES})

momo_assign_source_group(${SRC_FILES})

target_link_libraries(fuzzing-engine PRIVATE
  emulator-common
)

target_include_directories(fuzzing-engine INTERFACE "${CMAKE_CURRENT_LIST_DIR}")

momo_strip_target(fuzzing-engine)
