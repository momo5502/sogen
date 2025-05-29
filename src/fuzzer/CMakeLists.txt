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

if(CMAKE_C_COMPILER MATCHES "/.*/x86_64-w64-mingw32-gcc$")
  target_link_options(fuzzer PRIVATE -static-libstdc++ -static -lwinpthread)
endif()

target_link_libraries(fuzzer PRIVATE
  fuzzing-engine
  windows-emulator
)

momo_strip_target(fuzzer)
