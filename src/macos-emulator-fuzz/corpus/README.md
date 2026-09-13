# plist fuzz corpus

Seed and regression inputs for `macos-emulator-fuzz-replay` and `macos-emulator-fuzz`.

The **first byte selects the key** the harness looks up (see `plist_fuzz.cpp`); the rest is the
property-list payload. A generator producing only random bytes almost never reaches the value
readers — the binary format needs a plausible trailer and the XML format needs a matching key — so
the selector byte keeps both paths reachable while leaving the payload fully attacker-shaped.

Every file here must parse **fail-soft**: a clean `nullopt` or a value, never a crash, a hang, or an
out-of-bounds read.

Run them with:

    ./build/release/artifacts/macos-emulator-fuzz-replay src/macos-emulator-fuzz/corpus/*.bin
