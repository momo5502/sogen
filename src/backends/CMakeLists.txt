add_subdirectory(unicorn-emulator)

if (MOMO_ENABLE_RUST_CODE)
  add_subdirectory(icicle-emulator)
endif()
