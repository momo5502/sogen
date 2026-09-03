#!/bin/sh
# Builds appkitwin + libwstrace and records two traces of one run to first frame:
#   wstrace.log         every mach_msg send: msgh_id, size, target port, caller symbol
#   wstrace-exports.log ordered export-call surface (lldb breakpoints, auto-continuing)
set -e
cd "$(dirname "$0")"

: "${WSTRACE_WORKDIR:=/tmp/wstrace}"
mkdir -p "$WSTRACE_WORKDIR"

clang -arch arm64 -O2 -fno-omit-frame-pointer -dynamiclib -o "$WSTRACE_WORKDIR/libwstrace.dylib" wstrace.c
clang -arch arm64 -O2 -fobjc-arc -o "$WSTRACE_WORKDIR/appkitwin" appkitwin.m \
    -framework Foundation -framework AppKit -framework CoreGraphics

WSTRACE_OUT="$WSTRACE_WORKDIR/wstrace.log" \
DYLD_INSERT_LIBRARIES="$WSTRACE_WORKDIR/libwstrace.dylib" \
    "$WSTRACE_WORKDIR/appkitwin"

if command -v lldb >/dev/null; then
    lldb -b -s wstrace.lldb -- "$WSTRACE_WORKDIR/appkitwin" \
        >"$WSTRACE_WORKDIR/wstrace-exports.log" 2>&1 || true
fi

echo "wrote $WSTRACE_WORKDIR/wstrace.log"
echo "wrote $WSTRACE_WORKDIR/wstrace-exports.log"
