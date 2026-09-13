# Host-side fuzzing of the property-list reader, which parses bytes taken straight from the sample
# under analysis.
#
# Scope, stated precisely, because an earlier version of this comment overclaimed. In an ordinary
# release build these drivers detect a CRASH or an escaping exception -- which covers the
# unbounded-resize/std::length_error class -- but NOT a silent out-of-bounds read. Detecting that needs
# the library itself hardened: -DSOGEN_ENABLE_LIBCPP_HARDENING=ON turns an over-read into an abort.
# (ASAN would also serve but hangs this emulator on macOS.)
#
# What has NOT been demonstrated: the committed seeds catching an out-of-bounds read. Deleting the
# long-form length guard in plist.cpp and replaying the corpus under hardening still exits 0 -- the
# seeds never drive the parser deep enough. The one reachable over-read found so far needed a
# hand-built 24380-byte input. Treat the corpus as a crash-regression net and a reproducer, and grow it
# before treating a clean run as evidence of memory safety.

# The replay driver is deliberately NOT gated on SOGEN_ENABLE_FUZZING. Reproducing a saved crash is
# something you want in an ordinary build, and under SOGEN_ENABLE_FUZZING every translation unit gets
# -fsanitize=fuzzer-no-link, which leaves a driver that links neither libFuzzer nor a sanitizer with
# undefined __sanitizer_cov_* symbols. Linking ASAN to supply them is not an option here: ASAN makes
# even a hello-world hang on this platform, so the reproducer would be unusable.
add_executable(macos-emulator-fuzz-replay standalone_main.cpp plist_fuzz.cpp)
target_link_libraries(macos-emulator-fuzz-replay PRIVATE macos-emulator)

sogen_targets_set_folder("tests" macos-emulator-fuzz-replay)
sogen_strip_target(macos-emulator-fuzz-replay)

# libFuzzer driver — only when the compiler provides libFuzzer, and only under SOGEN_ENABLE_FUZZING,
# which is what instruments macos-emulator for coverage.
if(SOGEN_ENABLE_FUZZING AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_executable(macos-emulator-fuzz libfuzzer_main.cpp plist_fuzz.cpp)
  target_link_libraries(macos-emulator-fuzz PRIVATE macos-emulator)
  target_compile_options(macos-emulator-fuzz PRIVATE -fsanitize=fuzzer)
  target_link_options(macos-emulator-fuzz PRIVATE -fsanitize=fuzzer)

  sogen_targets_set_folder("tests" macos-emulator-fuzz)
endif()

# The corpus is only evidence if something runs it. Registered with CTest so a regression that crashes
# the parser fails the ordinary test run, and so the seeds cannot silently rot.
file(GLOB MACOS_PLIST_CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus/*.bin")
add_test(NAME macos-plist-corpus COMMAND macos-emulator-fuzz-replay ${MACOS_PLIST_CORPUS})
