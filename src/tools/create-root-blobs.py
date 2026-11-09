#!/usr/bin/env python3
"""Publishes an emulation root for incremental downloads.

Writes every file of the root, gzip-compressed, to <out>/blobs/<sha256 of its content>, and a manifest that lists each
file's path, hash and size plus every directory to <out>/manifest.json and to <root>/manifest.json, so that an
installed root knows its own files. A client downloads only the hashes it does not have yet.
"""

import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path

# Written by this script; it describes the root rather than belongs to it.
METADATA = {'manifest.json'}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('root', type=Path)
    parser.add_argument('out', type=Path)
    parser.add_argument('version')
    args = parser.parse_args()

    blobs = args.out / 'blobs'
    blobs.mkdir(parents=True, exist_ok=True)

    files = []
    directories = []
    for directory, subdirectories, names in os.walk(args.root):
        subdirectories.sort()
        relative = Path(directory).relative_to(args.root)
        if relative != Path('.'):
            directories.append(relative.as_posix())
        for name in sorted(names):
            path = (relative / name).as_posix()
            if path in METADATA:
                continue
            data = (Path(directory) / name).read_bytes()
            digest = hashlib.sha256(data).hexdigest()
            blob = blobs / digest
            if not blob.exists():
                blob.write_bytes(gzip.compress(data, compresslevel=9, mtime=0))
            files.append({'path': path, 'sha256': digest, 'size': len(data)})

    manifest = json.dumps({'version': args.version, 'directories': directories, 'files': files}, separators=(',', ':'))
    (args.out / 'manifest.json').write_text(manifest)
    (args.root / 'manifest.json').write_text(manifest)


if __name__ == '__main__':
    main()
