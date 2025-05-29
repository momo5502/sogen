file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
  *.rc
)

list(SORT SRC_FILES)

add_library(backend-selection ${SRC_FILES})

momo_assign_source_group(${SRC_FILES})

target_include_directories(backend-selection INTERFACE "${CMAKE_CURRENT_LIST_DIR}")

target_link_libraries(backend-selection PRIVATE
  unicorn-emulator
)

if (MOMO_ENABLE_RUST_CODE)
  target_link_libraries(backend-selection PRIVATE
    icicle-emulator
  )
endif()