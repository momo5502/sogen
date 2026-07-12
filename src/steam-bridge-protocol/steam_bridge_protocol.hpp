#pragma once

// Wire protocol for the Sogen Steam paravirtualization bridge.
//
// The guest game loads a drop-in steam_api64.dll shim which, instead of talking to a local Steam
// client over Steam's (undocumented, shared-memory) IPC, forwards Steamworks interface calls to the
// host through a virtual driver device (\\.\SogenSteam, i.e. \Device\SogenSteam) using
// NtDeviceIoControlFile. The host endpoint relays them to a real steamclient64.dll (or a mock/Goldberg
// backend for offline bring-up).
//
// The cut is at the C++ interface boundary -- the same seam Proton's lsteamclient uses. A method call
// is identified by (interface_handle, method_index); its flat in-arguments travel in the input buffer
// and its return value + out-buffers come back in the output buffer. Because guest and host share the
// x86-64 Windows ABI, argument frames are layout-compatible; the only translation the host performs is
// copying pointer/buffer payloads across the guest<->host memory boundary, driven by a per-method
// descriptor table generated from steam_api.json (see tools/steam-bridge-generator).
//
// This header is the single source of truth for that boundary and is deliberately dependency-free so it
// can be included unchanged by both the emulator device and the guest-side shim.

#include <array>
#include <cstdint>

namespace sogen::steam_bridge
{
    // Lets the guest confirm it is talking to a matching bridge before issuing any interface calls.
    inline constexpr uint32_t protocol_magic = 0x4d545353; // 'SSTM'
    inline constexpr uint32_t protocol_version = 1;

    // A host-side interface pointer id. Opaque to the guest; 0 means "no such interface".
    using interface_handle = uint64_t;
    inline constexpr interface_handle null_interface = 0;

    // Windows IOCTL encoding: CTL_CODE(DeviceType, Function, Method, Access). Mirrors the GPU bridge:
    // FILE_DEVICE_UNKNOWN (0x22), METHOD_BUFFERED (0), FILE_ANY_ACCESS (0), vendor function range.
    inline constexpr uint32_t device_type = 0x22; // FILE_DEVICE_UNKNOWN
    inline constexpr uint32_t method_buffered = 0;
    inline constexpr uint32_t file_any_access = 0;

    constexpr uint32_t make_ioctl(const uint32_t function)
    {
        return (device_type << 16) | (file_any_access << 14) | (function << 2) | method_buffered;
    }

    enum class command : uint32_t
    {
        get_version = 0x800,         // handshake: no input; output = version_response
        create_interface = 0x801,    // input = create_interface_request; output = create_interface_response
        release_interface = 0x802,   // input = release_interface_request; no output
        invoke_method = 0x803,       // input = invoke_header + arg blob; output = invoke_response + out blob
        run_callbacks = 0x804,       // input = run_callbacks_request; output = run_callbacks_response + callback blob
        get_api_call_result = 0x805, // input = api_call_result_request; output = api_call_result_response + data
    };

    inline constexpr uint32_t ioctl_get_version = make_ioctl(static_cast<uint32_t>(command::get_version));
    inline constexpr uint32_t ioctl_create_interface = make_ioctl(static_cast<uint32_t>(command::create_interface));
    inline constexpr uint32_t ioctl_release_interface = make_ioctl(static_cast<uint32_t>(command::release_interface));
    inline constexpr uint32_t ioctl_invoke_method = make_ioctl(static_cast<uint32_t>(command::invoke_method));
    inline constexpr uint32_t ioctl_run_callbacks = make_ioctl(static_cast<uint32_t>(command::run_callbacks));
    inline constexpr uint32_t ioctl_get_api_call_result = make_ioctl(static_cast<uint32_t>(command::get_api_call_result));

    // Longest Steamworks version string ("SteamNetworkingUtils004" and friends) is well under this.
    inline constexpr uint32_t max_interface_name = 64;

    struct version_response
    {
        uint32_t magic;
        uint32_t version;
    };

    struct create_interface_request
    {
        // NUL-terminated versioned interface name, e.g. "SteamUser023". Fixed size keeps the request POD.
        std::array<char, max_interface_name> version;
    };

    struct create_interface_response
    {
        interface_handle handle; // null_interface if the host could not resolve the interface
    };

    struct release_interface_request
    {
        interface_handle handle;
    };

    // invoke_method input buffer := invoke_header followed by `arg_bytes` of flat in-arguments, packed by
    // the generated guest thunk in declaration order (scalars inline; out-buffers are NOT packed here --
    // they are carved out of the output buffer, see invoke_response).
    struct invoke_header
    {
        interface_handle handle;
        uint32_t method_index; // vtable slot / steam_api.json method ordinal
        uint32_t arg_bytes;    // length of the trailing in-argument blob
    };

    // invoke_method output buffer := invoke_response followed by `out_bytes` of out-buffer payload (the
    // generated thunk scatters it back into the caller's out-pointers). `return_value` holds the method's
    // raw return, zero-extended to 64 bits (covers bool/int/uint64/CSteamID/pointer-as-string-length, etc).
    struct invoke_response
    {
        int32_t status;        // bridge-level status: 0 = ok, negative = bridge error (see invoke_status)
        uint32_t out_bytes;    // length of the trailing out-buffer blob
        uint64_t return_value; // method return, zero-extended
    };

    enum class invoke_status : int32_t
    {
        ok = 0,
        unknown_interface = -1,
        unknown_method = -2,
        backend_error = -3,
        output_too_small = -4,
    };

    struct run_callbacks_request
    {
        // Guest-visible pipe id; reserved for multi-pipe games. 0 for the single-pipe common case.
        uint32_t pipe;
    };

    // run_callbacks output := run_callbacks_response, then `blob_bytes` of normal callback records, then
    // `reverse_bytes` of reverse-callback records. Normal records feed the game's steam_api callback pump;
    // reverse records are calls the host made into a game-implemented response object (server browser etc.).
    struct run_callbacks_response
    {
        uint32_t count;         // number of normal callback records
        uint32_t blob_bytes;    // length of the normal-record blob
        uint32_t reverse_count; // number of reverse-callback records
        uint32_t reverse_bytes; // length of the reverse-record blob (follows the normal blob)
    };

    struct callback_record
    {
        int32_t callback_id; // iCallback (the k_iCallback constant of the callback struct)
        uint32_t data_bytes; // length of the payload that immediately follows this header
    };

    // A call the host made into a game response object, to be replayed on the guest object for `token`.
    struct reverse_record
    {
        uint64_t token;      // response-object token (from put_callback_token)
        int32_t method;      // method index within the response interface
        uint32_t data_bytes; // length of the serialized-args payload that follows
    };

    // Fetch the result of a completed async call (SteamAPICall_t), for CCallResult dispatch.
    struct api_call_result_request
    {
        uint64_t call;       // SteamAPICall_t handle from SteamAPICallCompleted_t
        int32_t callback_id; // expected iCallback of the result struct
        uint32_t data_bytes; // expected result size (m_cubParam)
        int32_t pipe;        // the game's HSteamPipe (callbacks/results are per-pipe)
        int32_t reserved;
    };

    // Followed by `data_bytes` of the result struct when ok != 0.
    struct api_call_result_response
    {
        int32_t ok;         // non-zero if the result was retrieved
        uint8_t io_failure; // non-zero if the call failed (bIOFailure)
        std::array<uint8_t, 3> reserved;
        uint32_t data_bytes;
    };

} // namespace sogen::steam_bridge
