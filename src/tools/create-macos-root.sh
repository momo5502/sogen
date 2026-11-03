#!/usr/bin/env bash

# Builds a macOS x86-64 emulation root from this Mac's own system files. The root must stay on this
# machine: the macOS license does not allow redistributing it or running it on non-Apple hardware.

set -euo pipefail

if [[ "$(uname -s)" != "Darwin" ]]; then
	echo "Error: the macOS emulation root can only be built on macOS." >&2
	exit 1
fi

ROOT="${1:-macos-root}"

# macOS 13 and later ship the shared cache inside the OS cryptex.
CACHE_DIR=/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld
if [[ ! -d "${CACHE_DIR}" ]]; then
	CACHE_DIR=/System/Library/dyld
fi

if [[ ! -f "${CACHE_DIR}/dyld_shared_cache_x86_64" ]]; then
	echo "Error: no x86-64 dyld shared cache in ${CACHE_DIR}." >&2
	exit 1
fi

for binary in /usr/lib/dyld /bin/bash /bin/echo /bin/ls /bin/pwd; do
	if ! lipo -archs "${binary}" | grep -qw x86_64; then
		echo "Error: ${binary} has no x86-64 slice." >&2
		exit 1
	fi
done

mkdir -p "${ROOT}/bin" "${ROOT}/usr/lib" "${ROOT}/System/Library/dyld"

# System files carry the SIP "restricted" flag, which only the system may set; ditto tries to copy it and
# fails, while cp leaves file flags behind.
cp -X /usr/lib/dyld "${ROOT}/usr/lib/dyld"
cp -X /bin/bash "${ROOT}/bin/bash"
cp -X /bin/echo "${ROOT}/bin/echo"
cp -X /bin/ls "${ROOT}/bin/ls"
cp -X /bin/pwd "${ROOT}/bin/pwd"

for cache in "${CACHE_DIR}"/dyld_shared_cache_x86_64 "${CACHE_DIR}"/dyld_shared_cache_x86_64.[0-9][0-9]; do
	cp -X "${cache}" "${ROOT}/System/Library/dyld/$(basename "${cache}")"
done
