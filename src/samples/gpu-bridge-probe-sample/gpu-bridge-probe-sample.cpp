// Minimal probe for the Sogen GPU paravirtualization bridge. It opens the virtual driver
// device (\\.\SogenGpu) and performs the protocol handshake via DeviceIoControl, validating
// that the host endpoint is present and speaks the expected protocol revision. This is the
// guest-side counterpart to the host gpu_bridge io_device and uses only documented Win32 APIs
// (no custom syscall / asm), so it runs unchanged in both host-DLL and root modes.

#include <windows.h>

#include <cstdio>

#include <gpu_bridge_protocol.hpp>

int main()
{
    const HANDLE device = CreateFileA(R"(\\.\SogenGpu)", GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (device == INVALID_HANDLE_VALUE)
    {
        std::printf("[gpu-bridge] CreateFile failed: %lu\n", GetLastError());
        return 1;
    }

    sogen::gpu_bridge::version_response response{};
    DWORD bytes_returned = 0;
    const BOOL ok = DeviceIoControl(device, sogen::gpu_bridge::ioctl_get_version, nullptr, 0, &response, sizeof(response),
                                    &bytes_returned, nullptr);

    CloseHandle(device);

    if (!ok)
    {
        std::printf("[gpu-bridge] DeviceIoControl failed: %lu\n", GetLastError());
        return 2;
    }

    std::printf("[gpu-bridge] handshake: magic=0x%08X version=%u (%lu bytes)\n", response.magic, response.version, bytes_returned);

    if (response.magic != sogen::gpu_bridge::protocol_magic)
    {
        std::printf("[gpu-bridge] bad magic (expected 0x%08X)\n", sogen::gpu_bridge::protocol_magic);
        return 3;
    }

    if (response.version != sogen::gpu_bridge::protocol_version)
    {
        std::printf("[gpu-bridge] protocol mismatch (host=%u, guest=%u)\n", response.version, sogen::gpu_bridge::protocol_version);
        return 4;
    }

    std::printf("[gpu-bridge] ok\n");
    return 0;
}
