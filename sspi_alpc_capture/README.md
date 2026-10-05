# SSPI ALPC capture utility

This is a standalone Windows x64 diagnostic executable. It patches the
`rpcrt4.dll` import-address-table entries used for ALPC and installs
process-local hooks on the three native system-call stubs used to open, control,
and close `\Device\KsecDD` and `\Device\CNG`. Installing the native hooks before
loading the cryptography providers captures their initialization traffic. It
does not inject, patch LSASS, or modify Sogen.

The utility calls the public SSPI entry points using the exact package name
`Microsoft Unified Security Protocol Provider`, captures the ALPC traffic on
the resulting `\\RPC Control\\lsasspirpc` handle, and completes a live Schannel
client lifecycle against `api.github.com:443`: handshake, stream-size query,
encrypted HTTP request, and the first nonempty decrypted response plaintext.
The host must have working DNS resolution and outbound TCP access to port 443.

## Build

Use an x64 Visual Studio Developer Command Prompt with a recent Windows SDK:

```bat
cmake -S . -B build -A x64
cmake --build build --config Release
```

The executable is normally at:

```text
build\Release\sspi_alpc_capture.exe
```

The CMake project deliberately rejects Win32 builds. It uses C++20, links
`secur32`, `version`, and `ws2_32`, and uses the static MSVC runtime (`/MT`).
It has no third-party or custom-DLL dependencies.

## Run

Run from a writable working directory:

```bat
build\Release\sspi_alpc_capture.exe
```

Administrator privileges are not required. Run it as the same ordinary user
whose SSPI behavior you want to observe. Endpoint security or process-mitigation
policy can still prevent modification of an image's IAT; that failure is
reported explicitly.

The program uses `capture` beneath the current working directory. For safety,
it refuses to run if that directory already contains anything. Move or remove
an earlier `capture` directory before each run; the program never silently
mixes or deletes previous results.

## Output

The ALPC hook's authoritative byte dumps remain:

```text
capture\0001_send.bin
capture\0001_send_payload.bin
capture\0001_recv.bin
capture\0001_recv_payload.bin
...
```

Raw files contain the exact observed bytes from offset zero through the
validated `PORT_MESSAGE.TotalLength`. Payload files contain exactly
`DataLength` bytes starting after the native x64 `PORT_MESSAGE`. When a valid
ALPC view attribute is present, the view bytes are saved separately as
`NNNN_send_view.bin` or `NNNN_recv_view.bin`, and `capture.jsonl` records the
attribute masks, view flags, section handle, base, size, and file name. Each
message or view is limited to 1 MiB, receive lengths are checked against the
pre-call capacity, and memory copies are guarded with structured exception
handling.

KsecDD and CNG requests are recorded separately:

```text
capture\device_io.jsonl
capture\device_0001_ksecdd_input.bin
capture\device_0001_ksecdd_output.bin
capture\device_0002_cng_input.bin
capture\device_0002_cng_output.bin
```

Each metadata row records the device, handle, IOCTL, native return status,
`IO_STATUS_BLOCK` completion status and byte count, input/output capacities,
and the names of captured buffers. Inputs are copied before the call and
completed outputs after it. An operation that returns `STATUS_PENDING` is
recorded, but its output is marked unavailable because completion occurs after
the native hook returns.

The deterministic lifecycle fixture is organized separately:

```text
capture\lifecycle.jsonl
capture\network\send_XXXX.bin
capture\network\recv_XXXX.bin
capture\sspi\isc_XXXX_input.bin
capture\sspi\isc_XXXX_output_token.bin
capture\sspi\http_request_plaintext.bin
capture\sspi\encrypt_0001_ciphertext.bin
capture\sspi\decrypt_XXXX_input.bin
capture\sspi\decrypt_XXXX_plaintext.bin
```

`lifecycle.jsonl` records each credential, ISC, network, stream-size,
encryption, decryption, and cleanup event in execution order. SSPI events
include buffer descriptors before and after the call. The binary files retain
the exact bytes needed for later deterministic replay work.

The live peer address, certificate chain, handshake messages, and HTTP response
can change between runs. File naming, event ordering, buffer metadata, and the
relationship between SSPI and network byte streams are deterministic within
each captured run.

Other files:

- `capture.jsonl`: one metadata object per target-handle send/wait/receive call.
- `connect_before.bin` / `connect_after.bin`: the target connect message where
  safely available.
- `connect.json`: connect status, handle, lengths, and capture diagnostics.
- `environment.json`: Windows build, process architecture/PID, and module
  paths, bases, and file versions.
- `device_io.jsonl`: one metadata object per observed KsecDD or CNG device
  control call.

The ALPC hook assigns sequence numbers at entry. In a multithreaded run,
completion and console lines can occur in a different order, but
`capture.jsonl` and the ALPC filenames are sorted by entry sequence.

## Failure behavior

The two required ALPC imports must be found in `rpcrt4.dll`'s normal or
delay-import tables. Device capture requires the standard x64
`NtOpenFile`, `NtDeviceIoControlFile`, and `NtClose` syscall-stub prologues and
an executable relay allocation within relative-jump range. The utility reports
setup failures and exits rather than silently producing an incomplete capture.
The ALPC hooks intentionally have no inline-hook fallback. Any DNS, TCP, SSPI,
encryption, or decryption failure is reported with its exact status and
produces a nonzero exit code after available artifacts and cleanup events are
written.

No RPC/NDR decoding is attempted. The dumps preserve the outer ALPC messages
and payloads for later analysis.
