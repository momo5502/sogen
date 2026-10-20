# Sogen for Android

A Flutter launcher that plays Windows games on Android with Sogen. It downloads the emulation root from
[sogen.dev/root.zip](https://sogen.dev/root.zip), keeps a library of game folders, and runs a game in a separate
`:sogen` service process so a crashing guest never takes the launcher down. Guest windows are rendered into Flutter
textures, and touch, mouse and keyboard input is forwarded to the guest.

## Native libraries

The emulator itself is `libsogen.so` (`src/android-runtime`), built by the regular CMake build for Android, together
with the shared libraries it links (the CPU backends and, on arm64, FEXCore). All `.so` files from the build's
artifacts directory go into `android/app/src/main/jniLibs/<abi>/`, which is not checked in. CI does this in the `build-android-app` job from the `Android arm64` and `Android x86_64` build artifacts.

Building them locally needs a Linux host, since parts of the dependency tree configure through shell scripts:

```sh
export ANDROID_NDK_ROOT=/path/to/ndk ANDROID_ABI=arm64-v8a
cmake --preset=release -DSOGEN_ENABLE_RUST_CODE=Off -DSOGEN_ENABLE_FEX=On \
  -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/toolchain/android-ndk.cmake
cmake --build --preset=release --target android-runtime

mkdir -p app/android/app/src/main/jniLibs/arm64-v8a
cp build/release/artifacts/*.so app/android/app/src/main/jniLibs/arm64-v8a/
```

## Turnip

Adreno's stock Vulkan drivers often lack features DXVK requires. On Adreno GPUs the runtime therefore loads Mesa's
Turnip driver through libadrenotools when the APK carries one at `android/app/src/main/assets/turnip/vulkan.ad07xx.so`.
CI takes it from a pinned [AdrenoToolsDrivers](https://github.com/K11MCH1/AdrenoToolsDrivers) release; see the
`build-android-app` job.

## Building the app

```sh
cd app
flutter build apk
```

arm64 devices run games through FEX, x86_64 devices and emulators through Unicorn.
