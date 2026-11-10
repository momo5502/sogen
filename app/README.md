# Sogen for Android

A Flutter launcher that plays Windows games on Android with Sogen. It downloads the emulation root from
[sogen.dev/root.zip](https://sogen.dev/root.zip), keeps a library of game folders, and runs a game in a separate
`:sogen` service process so a crashing guest never takes the launcher down. Guest windows are rendered into Flutter
textures, and touch, mouse and keyboard input is forwarded to the guest.

## Native libraries

The emulator itself is `libsogen.so` (`src/android-runtime`), built by the regular CMake build for Android, together
with `libsandbox.so`, which runs a game's child processes, and the shared libraries they link (the CPU backends and,
on arm64, FEXCore). All `.so` files from the build's artifacts directory go into `android/app/src/main/jniLibs/<abi>/`,
which is not checked in. CI builds them this way in the `build-android-app` job.

Building them locally needs a Linux host, since parts of the dependency tree configure through shell scripts, NDK r29
or later, since FEXCore uses `std::atomic_ref`, and the `libclang` and `packaging` Python packages:

```sh
export ANDROID_NDK_ROOT=/path/to/ndk
cmake --workflow --preset=android-app

mkdir -p app/android/app/src/main/jniLibs/arm64-v8a
cp build/android-arm64/artifacts/*.so app/android/app/src/main/jniLibs/arm64-v8a/
```

## Steam

Android has no Steam client, so the `android-app` preset builds the runtime with a Steam provider
(`SOGEN_ENABLE_STEAM_LOCAL_PROVIDER`) that answers a game's Steam calls inside the emulator. It plays games the signed-in
Steam account owns:

- Steam only lets the account download games it owns, and Sogen distributes no game content.
- A game runs as the account it was downloaded with. Its DRM is neither removed nor bypassed: it checks the app
  ownership ticket Steam's servers signed for that account and game. Without such a ticket, the provider withholds
  the ticket interface, and a protected game fails its own check. DRM-free games stay DRM-free.
- Calls the provider does not implement are reported as unsupported instead of being answered.

Sogen is not affiliated with or endorsed by Valve. Steam is a trademark of Valve Corporation.

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

The app supports arm64 devices only, since games run through FEX.
