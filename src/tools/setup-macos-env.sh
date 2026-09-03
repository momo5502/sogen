#!/bin/sh
# Prepares a machine to run the macOS user-space emulator, natively and in a browser.
#
# Every step is idempotent and re-running is cheap: an already-configured build tree is reused, an
# already-staged root is rebuilt from symlinks in under a second, and npm install is skipped when
# node_modules is newer than the lockfile.
#
#   src/tools/setup-macos-env.sh [--root DIR] [--port N] [--native-only|--browser-only] [--serve]
#
# With --serve it ends by serving the playground; otherwise it prints the command to run.

set -e

ROOT=/tmp/sogen-macos-root-full
PORT=8120
DO_NATIVE=1
DO_BROWSER=1
DO_SERVE=0

while [ $# -gt 0 ]; do
    case "$1" in
        --root) ROOT="$2"; shift 2 ;;
        --port) PORT="$2"; shift 2 ;;
        --native-only) DO_BROWSER=0; shift ;;
        --browser-only) DO_NATIVE=0; shift ;;
        --serve) DO_SERVE=1; shift ;;
        -h|--help) sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

SOURCE_DIR=$(cd "$(dirname "$0")/../.." && pwd)
cd "$SOURCE_DIR"

say() { printf '\n==> %s\n' "$1"; }
die() { printf '\nerror: %s\n' "$1" >&2; exit 1; }

need() {
    command -v "$1" >/dev/null 2>&1 || die "$1 is not installed. $2"
}

# ---------------------------------------------------------------- prerequisites

say "Checking prerequisites"

[ "$(uname -s)" = "Darwin" ] || die "the emulation root is built out of symlinks into a running macOS
       system, so staging one requires a macOS host. The emulator itself builds elsewhere."

[ "$(uname -m)" = "arm64" ] || printf '  warning: host is %s, not arm64. A guest arm64 binary will be\n           interpreted rather than run through Hypervisor.framework, which is much slower.\n' "$(uname -m)"

need cmake     "Install it with: brew install cmake"
need ninja     "Install it with: brew install ninja"
need python3   "Install it with: brew install python"

printf '  cmake   %s\n' "$(cmake --version | head -1 | awk '{print $3}')"
printf '  ninja   %s\n' "$(ninja --version)"
printf '  python3 %s\n' "$(python3 --version | awk '{print $2}')"

if [ "$DO_BROWSER" = 1 ]; then
    need node "Install it with: brew install node"
    need npm  "Install it with: brew install node"
    printf '  node    %s\n' "$(node --version)"
fi

# ---------------------------------------------------------------- emulation root

say "Staging the emulation root at $ROOT"

# make-macos-root.sh fails with its own message when the cryptex cache is missing, which is the one
# prerequisite a package manager cannot supply.
sh src/tools/make-macos-root.sh "$ROOT"

[ -e "$ROOT/System/Library/dyld/dyld_shared_cache_arm64e" ] \
    || die "the staged root has no shared cache at System/Library/dyld. Nothing will load without it."

printf '  shared cache  %s\n' "$(cd "$ROOT" && ls System/Library/dyld/dyld_shared_cache_arm64e >/dev/null && echo present)"
printf '  Calculator    %s\n' "$([ -d "$ROOT/System/Applications/Calculator.app" ] && echo present || echo 'absent (a GUI subject to test with)')"

# ---------------------------------------------------------------- native build

if [ "$DO_NATIVE" = 1 ]; then
    say "Building the native analyzer"

    [ -d build/release ] || cmake --preset=release
    cmake --build --preset=release

    [ -x build/release/artifacts/analyzer ] || die "the release build produced no analyzer binary."
    printf '  built  build/release/artifacts/analyzer\n'
fi

# ---------------------------------------------------------------- browser build

if [ "$DO_BROWSER" = 1 ]; then
    say "Building the wasm module"

    # The preset reads $EMSDK/upstream/emscripten/cmake/... . A Homebrew emscripten has no such layout,
    # and deriving it from `which emcc` lands on a symlink one level short of the toolchain file, so
    # configure fails with "Could not find toolchain file". A two-line shim gives the preset the shape
    # it expects; an emsdk-managed install already has it.
    if [ -z "$EMSDK" ]; then
        if [ -d /opt/homebrew/opt/emscripten/libexec ]; then
            EMSDK=/tmp/sogen-emsdk-shim
            mkdir -p "$EMSDK/upstream"
            ln -sfn /opt/homebrew/opt/emscripten/libexec "$EMSDK/upstream/emscripten"
            export EMSDK
            printf '  using Homebrew emscripten through a shim at %s\n' "$EMSDK"
        else
            die "EMSDK is unset and no Homebrew emscripten was found.
       Install one with:  brew install emscripten
       or source an emsdk checkout's emsdk_env.sh and re-run."
        fi
    else
        printf '  using EMSDK=%s\n' "$EMSDK"
    fi

    [ -f "$EMSDK/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake" ] \
        || die "no toolchain file under \$EMSDK/upstream/emscripten/cmake. EMSDK points at the wrong level."

    # -DUNICORN_ARCH=aarch64 is load-bearing: a multi-architecture wasm module mis-resolves Unicorn's
    # memory helpers and faults inside dyld (src/macos-web/CMakeLists.txt records the reproduction).
    # The warning suppression works around em++ rejecting macos-emulator's precompiled header on
    # Emscripten 4.0.23 with -Werror,-Wunused-command-line-argument on its own -x c++-header flag.
    [ -d build/emscripten64 ] || cmake --preset=emscripten64 \
        -DUNICORN_ARCH=aarch64 \
        -DCMAKE_CXX_FLAGS="-Wno-unused-command-line-argument"

    ninja -C build/emscripten64 macos-web

    say "Building the playground"

    # Reinstall when the lockfile has moved since the last install, not merely when node_modules is
    # absent -- a stale tree after a pull is the failure this avoids, and it does not look like one.
    if [ ! -d page/node_modules ] || [ page/package-lock.json -nt page/node_modules ]; then
        (cd page && npm install)
    else
        printf '  npm     dependencies up to date\n'
    fi

    (cd page && npm run build)

    [ -f page/dist/macos-analyzer.wasm ] \
        || die "page/dist has no macOS wasm. The CMake POST_BUILD publish step did not run."

    printf '  wasm    %s bytes\n' "$(wc -c < page/dist/macos-analyzer.wasm | tr -d ' ')"
fi

# ---------------------------------------------------------------- done

say "Ready"

SERVE="python3 src/macos-web/serve.py --port $PORT --directory page/dist --macos-root $ROOT"

if [ "$DO_NATIVE" = 1 ]; then
    cat <<EOF

Run Calculator natively, to a screenshot:

  build/release/artifacts/analyzer --os=macos -v --gui \\
    --root $ROOT --desktop-size 420x520 \\
    --max-instructions 40000000000 --skip-syscalls \\
    --screenshot /tmp/calc.png /System/Applications/Calculator.app
EOF
fi

if [ "$DO_BROWSER" = 1 ]; then
    cat <<EOF

Serve the playground:

  $SERVE

then open http://127.0.0.1:$PORT in Chrome or Edge -- it needs wasm64, which Safari and Firefox
do not have -- pick "macOS Emulator" in Settings, and attach a subject from the served root.
EOF
fi

if [ "$DO_SERVE" = 1 ]; then
    say "Serving on http://127.0.0.1:$PORT (ctrl-c to stop)"
    exec $SERVE
fi
