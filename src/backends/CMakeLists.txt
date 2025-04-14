if (NOT CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
  add_subdirectory(unicorn-emulator)
endif()

if (MOMO_ENABLE_RUST_CODE)
  add_subdirectory(icicle-emulator)
endif()
