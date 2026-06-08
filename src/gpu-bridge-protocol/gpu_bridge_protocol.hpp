#pragma once

// Wire protocol for the Sogen GPU paravirtualization bridge.
//
// The guest talks to the host through a virtual driver device (\\.\SogenGpu, i.e.
// \Device\SogenGpu) using NtDeviceIoControlFile. The IOCTL code selects a command;
// the input/output buffers carry the (de)serialized payload. This header is the single
// source of truth for that boundary and is intentionally dependency-free (only <cstdint>)
// so it can be included unchanged by both the emulator and guest-side shims.

#include <cstdint>

namespace sogen::gpu_bridge
{
    // Identifies a valid bridge and lets the guest detect a host that speaks a different
    // protocol revision before issuing any further commands.
    inline constexpr uint32_t protocol_magic = 0x55504753;   // 'SGPU'
    inline constexpr uint32_t protocol_version = 1;

    // Windows IOCTL encoding: CTL_CODE(DeviceType, Function, Method, Access).
    //   value = (DeviceType << 16) | (Access << 14) | (Function << 2) | Method
    // We use FILE_DEVICE_UNKNOWN (0x22), METHOD_BUFFERED (0) and FILE_ANY_ACCESS (0), and
    // keep functions in the vendor-defined range (>= 0x800), matching how a real driver
    // would lay out its private control codes.
    inline constexpr uint32_t device_type = 0x22;            // FILE_DEVICE_UNKNOWN
    inline constexpr uint32_t method_buffered = 0;
    inline constexpr uint32_t file_any_access = 0;

    inline constexpr uint32_t make_ioctl(const uint32_t function)
    {
        return (device_type << 16) | (file_any_access << 14) | (function << 2) | method_buffered;
    }

    enum class command : uint32_t
    {
        get_version = 0x800,
    };

    inline constexpr uint32_t ioctl_get_version = make_ioctl(static_cast<uint32_t>(command::get_version));

    // Output payload of ioctl_get_version.
    struct version_response
    {
        uint32_t magic;   // protocol_magic
        uint32_t version; // protocol_version
    };
}
