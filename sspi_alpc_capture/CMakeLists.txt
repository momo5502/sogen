cmake_minimum_required(VERSION 3.20)
project(sspi_alpc_capture LANGUAGES CXX)

if(NOT MSVC)
  message(FATAL_ERROR "This project supports Microsoft Visual C++ only.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "This utility must be configured for x64 (use: cmake -A x64).")
endif()

add_executable(sspi_alpc_capture
  main.cpp
  ksecdd_provider_probe.cpp
  iat_hook.cpp
  capture.cpp
)

target_compile_features(sspi_alpc_capture PRIVATE cxx_std_20)
target_compile_definitions(sspi_alpc_capture PRIVATE
  SECURITY_WIN32
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  UNICODE
  _UNICODE
)
target_compile_options(sspi_alpc_capture PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(sspi_alpc_capture PRIVATE secur32 version ws2_32)

# Avoid a dependency on the redistributable MSVC runtime DLLs.
set_property(TARGET sspi_alpc_capture PROPERTY
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
