add_subdirectory(unicorn-emulator)

if (MOMO_ENABLE_RUST)
  add_subdirectory(icicle-emulator)
endif()
