# Windows x64 uses the WHP backend, Linux x64 uses the KVM backend. Both are x86-64 only. Android uses the app's
# default backend: FEX on arm64, Unicorn elsewhere.
if(NOT ANDROID AND NOT (OSX AND SOGEN_ENABLE_FEX)
   AND ((NOT WIN AND NOT (LINUX AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")) OR MINGW OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8
        OR (MSVC AND CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "ARM64")))
  return()
endif()

file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
)

if(WIN)
  file(GLOB_RECURSE RC_FILES CONFIGURE_DEPENDS *.rc)
  list(APPEND SRC_FILES ${RC_FILES})
endif()

list(SORT SRC_FILES)

add_executable(sandbox ${SRC_FILES})

sogen_assign_source_group(${SRC_FILES})

target_link_libraries(sandbox PRIVATE
  windows-emulator
  process-manager
  emulator-common
  cli11
)

if(WIN)
  target_link_libraries(sandbox PRIVATE whp-emulator shell32)
elseif(ANDROID)
  target_link_libraries(sandbox PRIVATE backend-selection)

  # An app can only execute files from its native library directory, and only lib*.so files from the APK land there.
  set_target_properties(sandbox PROPERTIES OUTPUT_NAME libsandbox SUFFIX .so)
elseif(OSX)
  target_link_libraries(sandbox PRIVATE backend-selection)
else()
  target_link_libraries(sandbox PRIVATE kvm-emulator)
endif()

sogen_strip_target(sandbox)
