add_subdirectory(unicorn-emulator)

if (MOMO_ENABLE_RUST)
  add_subdirectory(icicle-emulator)
endif()

if (NOT MINGW AND MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)
  add_subdirectory(whp-emulator)
endif()
