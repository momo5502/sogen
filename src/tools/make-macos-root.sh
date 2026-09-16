#!/bin/sh
# Builds an emulation root for the macOS emulator out of symlinks into the running system.
#
# The minimal root sogen bring-up used carries only dyld and the shared cache, which is enough for a
# command-line binary: everything it links against is inside the cache. A GUI app is not -- AppKit's
# +[NSAppearance _initializeCoreUI] reads .car asset catalogues out of /System/Library/CoreServices,
# CoreText reads fonts, and an app bundle reads its own resources. None of that is in the cache, and
# an app that cannot find it throws NSInternalInconsistencyException before its first window.
#
# Nothing is copied. Every entry is a symlink into the running system, so a host open() through the root
# lands on the analyst's real file -- clamping the guest path into the root cannot stop that. Reads through
# these links are the point; the write direction is contained by guest_file_system, which refuses any
# mutation whose real path resolves outside the root. Only the directories created below are writable.
#
#   src/tools/make-macos-root.sh [root]     (default /tmp/sogen-macos-root)

set -e

ROOT="${1:-/tmp/sogen-macos-root}"

# The shared cache does not live under /System/Library on a cryptex-based system, and dyld looks for it
# at exactly this guest path.
CACHE_DIR=/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld

if [ ! -d "$CACHE_DIR" ]; then
    echo "no shared cache at $CACHE_DIR" >&2
    exit 1
fi

rm -rf "$ROOT"
mkdir -p "$ROOT/System/Library" "$ROOT/usr" "$ROOT/private"

for entry in /System/Library/*; do
    ln -s "$entry" "$ROOT/System/Library/$(basename "$entry")"
done

ln -s "$CACHE_DIR" "$ROOT/System/Library/dyld"

for entry in /System/Applications /System/Cryptexes /System/iOSSupport; do
    [ -e "$entry" ] && ln -s "$entry" "$ROOT/System/$(basename "$entry")"
done

for entry in /usr/lib /usr/share /usr/bin /usr/libexec; do
    [ -e "$entry" ] && ln -s "$entry" "$ROOT/usr/$(basename "$entry")"
done

for entry in /Library /Applications; do
    [ -e "$entry" ] && ln -s "$entry" "$ROOT/$(basename "$entry")"
done

# A writable scratch the guest can actually use, rather than a link to the analyst's own /tmp.
mkdir -p "$ROOT/private/tmp" "$ROOT/private/var/root" "$ROOT/private/var/folders"

# The guest's home. A real system ships these; an empty /var/root does not, and the guest only creates
# the leaf it wants. Calculator's first arithmetic opens a CoreData store at
# ~/Library/Application Support/default.store, whose mkdir fails with ENOENT on the missing Library and
# takes the whole operation down -- "During recovery, parent directory path reported as missing".
for library in "Application Support" Caches Preferences Containers Saved\ Application\ State; do
    mkdir -p "$ROOT/private/var/root/Library/$library"
done

ln -s private/tmp "$ROOT/tmp"
ln -s private/var "$ROOT/var"

echo "$ROOT"
