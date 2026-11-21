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

## Guest shims

The APK carries the Steam and Vulkan guest DLLs built with its native runtime. `build-android-app` downloads the
same workflow's `Temp Root Build Tools` artifact and stages `steamclient.dll`, `steamclient64.dll`,
`vulkan-shim-x86.dll` and `vulkan-shim-x64.dll` in `android/app/src/main/assets/guest-shims/`. A `version` file
contains the SHA-256 of their `sha256sum` listing, in that order.

Every game service, including child processes, extracts these assets to private storage and mounts them read-only
over `C:\steam\steamclient*.dll` and both `vulkan-1.dll` and `winevulkan.dll` in `system32`/`syswow64`. Downloading
a newer root or retaining an older game's writable layer cannot replace these APK-matched DLLs. Missing bundled
assets stop startup instead of falling back to the downloaded root's shims.

For local APK builds, stage those four DLLs from a root-tools build of the same commit as the native libraries,
then generate `version` with the `Stage Matching Guest Shims` command in `.github/workflows/ci-reusable.yml`.
Updating the APK refreshes the private copies when the bundled DLLs change. Saved processes record this version;
if it changes, start the game again rather than restoring its old DLL images.

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
