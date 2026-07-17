add_subdirectory(unicorn-emulator)

if (SOGEN_ENABLE_RUST)
  add_subdirectory(icicle-emulator)
endif()

if (NOT MINGW AND MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)
  add_subdirectory(whp-emulator)
endif()

if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT ANDROID AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
  add_subdirectory(kvm-emulator)
endif()
