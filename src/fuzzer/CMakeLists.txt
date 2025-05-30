file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
  *.rc
)

list(SORT SRC_FILES)

add_executable(fuzzer ${SRC_FILES})

momo_assign_source_group(${SRC_FILES})

if(NOT MOMO_ENABLE_CLANG_TIDY)
  target_precompile_headers(fuzzer PRIVATE std_include.hpp)
endif()

target_link_libraries(fuzzer PRIVATE
  fuzzing-engine
  windows-emulator
)

if (MOMO_ENABLE_RUST_CODE)
  target_link_libraries(fuzzer PRIVATE
    icicle-emulator
  )
endif()

momo_strip_target(fuzzer)
