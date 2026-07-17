# Host-side fuzzing of the emulator's IO device / syscall handlers with untrusted guest data.
# NOTE: this is NOT the guest-application fuzzer in src/fuzzer — it fuzzes the emulator itself.
#
# Engine-independent core (sogen::fuzz::run, device_fuzz.cpp) + thin per-engine drivers. Gated by
# SOGEN_ENABLE_FUZZING. device_fuzz.cpp is compiled into each driver so each gets its own flags
# (the replay driver stays uninstrumented; the libFuzzer driver adds fuzzer+asan).

# Standalone replay driver — any toolchain (incl. MSVC). Smoke tests / regression / reproducers.
add_executable(windows-emulator-fuzz-replay standalone_main.cpp device_fuzz.cpp)
target_link_libraries(windows-emulator-fuzz-replay PRIVATE
  windows-emulator
  vulkan-bridge-marshal
)

sogen_targets_set_folder("tests" windows-emulator-fuzz-replay)
sogen_strip_target(windows-emulator-fuzz-replay)

# libFuzzer driver — only when the compiler provides libFuzzer (clang / clang-cl). Effective coverage
# requires the whole emulator to be built with -fsanitize=fuzzer-no-link (see SOGEN_ENABLE_FUZZING in
# cmake/compiler-env.cmake); pair with SOGEN_ENABLE_SANITIZER=On for ASAN.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_executable(windows-emulator-fuzz libfuzzer_main.cpp device_fuzz.cpp)
  target_link_libraries(windows-emulator-fuzz PRIVATE
    windows-emulator
    vulkan-bridge-marshal
  )
  target_compile_options(windows-emulator-fuzz PRIVATE -fsanitize=fuzzer,address)
  target_link_options(windows-emulator-fuzz PRIVATE -fsanitize=fuzzer,address)

  sogen_targets_set_folder("tests" windows-emulator-fuzz)
endif()
