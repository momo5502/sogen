#!/bin/sh
# Rebuilds the committed Mach-O fixtures. macOS + Xcode command line tools only.
# The output basename is load-bearing: the linker's ad-hoc code signature hashes it,
# so renaming a fixture changes its bytes.
set -eu

cd "$(dirname "$0")"

clang -target arm64-apple-macos -static -nostdlib -Wl,-no_uuid -e __start -o macho_static_arm64 static_entry.c
clang -target arm64e-apple-macos -static -nostdlib -Wl,-no_uuid -e __start -o macho_static_arm64e static_entry.c
lipo -create macho_static_arm64 macho_static_arm64e -output macho_fat_arm64_arm64e
clang -target arm64-apple-macos -Wl,-no_uuid -o macho_dylink_arm64 hello.c
clang -target arm64-apple-macos -static -nostdlib -Wl,-no_uuid -e __start -o macho_trace_arm64 trace_entry.c

# The GUI demo the browser front-end offers. Dynamically linked against SkyLight, so it needs a macOS
# root at run time -- which is what the page's "Load macOS root" provides. Keeps its LC_UUID, unlike the
# fixtures above: dyld refuses a dynamic image without one, and the reproducibility -no_uuid buys is only
# worth having where the bytes are compared.
clang -target arm64-apple-macos -o macho_calcdemo_arm64 calcdemo.c \
  -F/System/Library/PrivateFrameworks -framework SkyLight -framework CoreGraphics

shasum -a 256 macho_static_arm64 macho_static_arm64e macho_fat_arm64_arm64e macho_dylink_arm64 macho_trace_arm64 macho_calcdemo_arm64
