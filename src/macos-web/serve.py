#!/usr/bin/env python3
"""Static file server for the macOS wasm front-end and the capability probe.

Python's SimpleHTTPRequestHandler ignores Range and answers 200 with the whole file. That is not good
enough here for two reasons: the capability probe asks whether a browser's synchronous XHR can issue a
Range request, and an origin that never answers 206 makes that probe report the server's limitation as
the browser's; and the shared-cache range bridge reads slices of a multi-gigabyte file over exactly this
mechanism. It also lets a browser resume the 38 MB wasm module instead of restarting it.
"""

import argparse
import http.server
import os
import urllib.parse
import re
import socketserver
import stat

RANGE = re.compile(r"^bytes=(\d*)-(\d*)$")

# A week. Long enough that a visit costs nothing, short enough that a root rebuilt without its mtime
# changing still self-corrects.
MACOS_ROOT_CACHE_CONTROL = "public, max-age=604800"


class RangeHandler(http.server.SimpleHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    # Set from --macos-root. None means the prefix is not served at all.
    macos_root = None

    _etag = None
    _file_id = None
    _cache_control = "no-cache"

    def translate_path(self, path):
        prefix = "/macos-root/"
        if not self.macos_root or not path.startswith(prefix):
            return super().translate_path(path)

        relative = path[len(prefix):].split("?", 1)[0].split("#", 1)[0]

        # Percent-decoding happens per path component, after the split rather than before it: decoding
        # first would let an encoded separator inject one, so "%2F.." could climb out of the root.
        # Plenty of macOS lives behind a space -- Input Methods, Application Support, Desktop Pictures --
        # and without this none of it is reachable at all.
        #
        # The request is sanitised, not the result. A macOS root is built out of symlinks pointing at
        # system files -- that is the point of it -- so refusing anything that resolves outside the root
        # would refuse the entire root. What must not be possible is a *request* climbing out, so every
        # ".." and "." is dropped after decoding, and so is any component decoding to a separator.
        parts = []
        for raw in relative.split("/"):
            part = urllib.parse.unquote(raw)
            if part in ("", ".", "..") or "/" in part or "\\" in part or "\0" in part:
                continue
            parts.append(part)

        return os.path.join(self.macos_root, *parts) if parts else self.macos_root

    # A range response carried no validator at all, which is the difference between a root that is
    # fetched once and one that is fetched on every visit: with nothing to revalidate against and
    # no-cache on top, a browser has to go to the network for every one of the ~14,000 range reads a
    # launch makes. Size and mtime are enough to name a version of a file that is only ever read.
    def _validators_for(self, path):
        try:
            info = os.stat(path)
        except OSError:
            return None, None, None

        if stat.S_ISDIR(info.st_mode):
            return None, None, None

        # The device and inode are what make this name the file's rather than this URL's. An emulation
        # root is a farm of symlinks onto a sealed system volume, and a sealed volume stamps every file
        # with one mtime, so size and mtime together degenerate to the size: on a macOS 26 host 2,325 of
        # 2,510 framework Info.plists then carry a validator that some *other* framework also carries.
        # A client that treats the validator as the file's identity -- macos-root-sw.js does, to share
        # one block cache between the two paths a cryptex root reaches the shared cache at -- reads the
        # wrong framework's bytes at exactly the right length.
        identity = f"{info.st_dev:x}-{info.st_ino:x}"

        return (
            f'"{identity}-{info.st_size:x}-{info.st_mtime_ns:x}"',
            self.date_time_string(int(info.st_mtime)),
            identity,
        )

    def _is_macos_root(self):
        return bool(self.macos_root) and self.path.startswith("/macos-root/")

    def end_headers(self):
        self.send_header("Accept-Ranges", "bytes")

        if self._etag:
            self.send_header("ETag", self._etag)

        # Named separately from the ETag because they answer different questions: the ETag says whether
        # this URL's bytes changed, this says which file is behind the URL. Only a client that is told
        # the answer may share cached bytes between two URLs.
        if self._file_id:
            self.send_header("X-Sogen-File-Id", self._file_id)

        # An emulation root is a snapshot of a read-only system, so it is worth caching for a week; the
        # ETag is what catches it changing anyway. Everything else is the application itself, which is
        # rebuilt constantly and must revalidate -- that costs a 304 and not a download.
        self.send_header("Cache-Control", self._cache_control)

        # The probe and the front-end are same-origin, but a cross-origin isolated context is what any
        # future SharedArrayBuffer work needs, and these headers cost nothing to send now.
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        super().end_headers()

    def list_directory(self, path):
        # Only advertise what can actually be served. A macOS framework directory is full of symlinks
        # whose target is not on disk at all -- ExtensionFoundation.framework/ExtensionFoundation points
        # at Versions/A/ExtensionFoundation, and that dylib exists only inside the shared cache. Listing
        # one makes the page create a lazy file for it, and the 404 on first read aborts the whole run
        # instead of reaching the guest as the ENOENT it gets natively.
        real_listdir = os.listdir

        def servable(target):
            return [name for name in real_listdir(target) if os.path.exists(os.path.join(target, name))]

        os.listdir = servable
        try:
            return super().list_directory(path)
        finally:
            os.listdir = real_listdir

    def send_head(self):
        path = self.translate_path(self.path)
        self._etag, last_modified, self._file_id = self._validators_for(path)
        self._cache_control = MACOS_ROOT_CACHE_CONTROL if self._is_macos_root() else "no-cache"

        header = self.headers.get("Range")

        # A conditional full-file request is answered from the validator rather than the body. The base
        # class only knows If-Modified-Since, and a browser that was given an ETag sends If-None-Match.
        if not header and self._etag and self.headers.get("If-None-Match") == self._etag:
            self.send_response(304)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return None

        if not header:
            return super().send_head()

        match = RANGE.match(header.strip())
        if not match:
            self.send_error(400, "malformed Range")
            return None

        if os.path.isdir(path):
            return super().send_head()

        # If-Range: a browser holding part of a file asks for the rest only if what it holds is still
        # current. Answering 206 regardless would splice bytes from two different versions of the file
        # together inside the client, which is silent corruption rather than a failed read.
        conditional = self.headers.get("If-Range")
        if conditional and self._etag and conditional.strip() != self._etag:
            return super().send_head()

        try:
            stream = open(path, "rb")
        except OSError:
            self.send_error(404, "File not found")
            return None

        size = os.fstat(stream.fileno()).st_size
        first, last = match.group(1), match.group(2)

        if first == "":
            # A suffix range: the last N bytes.
            if last == "":
                stream.close()
                self.send_error(400, "malformed Range")
                return None
            length = min(int(last), size)
            start = size - length
            end = size - 1
        else:
            start = int(first)
            end = int(last) if last else size - 1
            end = min(end, size - 1)

        if start > end or start >= size:
            stream.close()
            self.send_response(416)
            self.send_header("Content-Range", f"bytes */{size}")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return None

        stream.seek(start)
        self.send_response(206)
        self.send_header("Content-Type", self.guess_type(path))
        self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.send_header("Content-Length", str(end - start + 1))
        if last_modified:
            self.send_header("Last-Modified", last_modified)
        self.end_headers()
        return _Slice(stream, end - start + 1)


class _Slice:
    """Hands copyfile() exactly the requested window and no more."""

    def __init__(self, stream, remaining):
        self.stream = stream
        self.remaining = remaining

    def read(self, amount=-1):
        if self.remaining <= 0:
            return b""
        if amount is None or amount < 0:
            amount = self.remaining
        data = self.stream.read(min(amount, self.remaining))
        self.remaining -= len(data)
        return data

    def close(self):
        self.stream.close()


class Server(socketserver.ThreadingTCPServer):
    daemon_threads = True
    allow_reuse_address = True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8099)
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--directory", default=".")
    parser.add_argument(
        "--macos-root",
        default=None,
        help="Serve this directory tree under /macos-root/, with symlinks resolved. The page fetches the "
        "dylinker and the shared cache from here by range instead of asking the user to pick them, which a "
        "folder picker cannot do anyway: browsers do not follow symlinks, so a root built out of them looks "
        "empty. Bound to localhost and read-only.",
    )
    arguments = parser.parse_args()

    os.chdir(arguments.directory)

    handler = RangeHandler
    if arguments.macos_root:
        RangeHandler.macos_root = os.path.realpath(arguments.macos_root)
        print(f"serving macOS root {RangeHandler.macos_root} at /macos-root/", flush=True)

    with Server((arguments.bind, arguments.port), handler) as server:
        print(f"serving {os.getcwd()} on http://{arguments.bind}:{arguments.port}/", flush=True)
        server.serve_forever()


if __name__ == "__main__":
    main()
