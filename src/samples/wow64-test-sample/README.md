# WoW64 execution regression sample

Build the `wow64-test-sample` target in a Windows x86 configuration. The sample
checks arguments, 32-bit memory accesses, SSE arithmetic, thread-local storage,
thread completion and continued execution after software and hardware
exceptions. It returns either zero or 37, selected by `--exit-37`.

On a Linux host with the x86 cross compiler, the same source can be built with:

```sh
i686-w64-mingw32-gcc -std=c11 -O1 -msse2 -Wall -Wextra -Werror \
  src/samples/wow64-test-sample/main.c -o /path/to/wow64-test-sample.exe -lkernel32
```

The host-side `wow64-emulation-test` CTest target runs both exit cases. Set
`EMULATOR_ROOT` to an x64 root containing its matching WoW64 files and
`EMULATOR_WOW64_SAMPLE` to the built sample. The test mounts the sample and
root read-only, and checks that a 32-bit process completes every sample check.
Without both variables, it skips. The sample does not need to be copied into
the root.

```sh
EMULATOR_ROOT=/path/to/root \
EMULATOR_WOW64_SAMPLE=/path/to/wow64-test-sample.exe \
EMULATOR_KVM=1 ctest --test-dir /path/to/build -R '^wow64-emulation-test$' --output-on-failure
```

Omit `EMULATOR_KVM` to use the default software backend. The separate
`X8664BackendTest.XsaveAndXrstorRestoreSseRegisters` test checks the extended
state instructions directly without a root or executable fixture.
