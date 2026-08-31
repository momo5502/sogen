#!/bin/sh
# Refreshes the vendored XNU inputs. Run by hand on a developer machine; never in CI.
# CI regenerates the tables from what this script committed and diffs the result.
set -eu

cd "$(dirname "$0")"
mkdir -p vendor

if [ $# -eq 1 ]; then
    tag="$1"
else
    host_xnu=$(uname -v | sed -n 's/.*root:xnu-\([0-9]*\)\..*/\1/p')
    tag=$(curl -fsSL "https://api.github.com/repos/apple-oss-distributions/xnu/tags?per_page=100" |
        sed -n 's/.*"name": "\(xnu-[0-9.]*\)".*/\1/p' |
        { [ -n "$host_xnu" ] && grep "^xnu-${host_xnu}\." || cat; } |
        head -n 1)
fi

if [ -z "$tag" ]; then
    echo "could not determine an xnu tag; pass one explicitly: $0 xnu-11215.41.3" >&2
    exit 1
fi

base="https://raw.githubusercontent.com/apple-oss-distributions/xnu/${tag}"
curl -fsSL "${base}/bsd/kern/syscalls.master" -o vendor/syscalls.master
curl -fsSL "${base}/osfmk/kern/syscall_sw.c" -o vendor/syscall_sw.c
# syscalls.master carries no per-file licence header of its own -- XNU licenses it through the
# repository's top-level APPLE_LICENSE, so that file has to travel with it to satisfy APSL 2.0.
curl -fsSL "${base}/APPLE_LICENSE" -o vendor/APPLE_LICENSE

master_sha=$(shasum -a 256 vendor/syscalls.master | cut -d' ' -f1)
sw_sha=$(shasum -a 256 vendor/syscall_sw.c | cut -d' ' -f1)
licence_sha=$(shasum -a 256 vendor/APPLE_LICENSE | cut -d' ' -f1)

cat > vendor/provenance.json <<EOF
{
  "repository": "https://github.com/apple-oss-distributions/xnu",
  "xnu_version": "${tag}",
  "retrieved": "$(date -u +%Y-%m-%d)",
  "license": "Apple Public Source License 2.0",
  "files": {
    "syscalls.master": {"path": "bsd/kern/syscalls.master", "sha256": "${master_sha}"},
    "syscall_sw.c": {"path": "osfmk/kern/syscall_sw.c", "sha256": "${sw_sha}"},
    "APPLE_LICENSE": {"path": "APPLE_LICENSE", "sha256": "${licence_sha}"}
  }
}
EOF

echo "vendored ${tag}"
