// Probe for the Sogen GPU paravirtualization bridge. It opens the virtual driver device
// (\\.\SogenGpu) and drives the bridge purely through DeviceIoControl: a protocol handshake,
// then a minimal Vulkan flow (create instance -> enumerate physical devices -> query properties
// -> destroy instance) that is remoted to the host's real Vulkan driver. This is the guest-side
// counterpart to the host gpu_bridge io_device and uses only documented Win32 APIs (no custom
// syscall / asm), so it runs unchanged in both host-DLL and root modes.

#include <windows.h>

#include <cstdio>
#include <vector>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

#include <gpu_bridge_protocol.hpp>

namespace
{
    bool ioctl(HANDLE device, uint32_t code, const void* in, DWORD in_len, void* out, DWORD out_len, DWORD& returned)
    {
        returned = 0;
        return DeviceIoControl(device, code, const_cast<void*>(in), in_len, out, out_len, &returned, nullptr) != FALSE;
    }

    bool handshake(HANDLE device)
    {
        sogen::gpu_bridge::version_response response{};
        DWORD returned = 0;
        if (!ioctl(device, sogen::gpu_bridge::ioctl_get_version, nullptr, 0, &response, sizeof(response), returned))
        {
            std::printf("[gpu-bridge] handshake IOCTL failed: %lu\n", GetLastError());
            return false;
        }

        std::printf("[gpu-bridge] handshake: magic=0x%08X version=%u\n", response.magic, response.version);
        return response.magic == sogen::gpu_bridge::protocol_magic && response.version == sogen::gpu_bridge::protocol_version;
    }
}

int main()
{
    const HANDLE device = CreateFileA(R"(\\.\SogenGpu)", GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (device == INVALID_HANDLE_VALUE)
    {
        std::printf("[gpu-bridge] CreateFile failed: %lu\n", GetLastError());
        return 1;
    }

    if (!handshake(device))
    {
        std::printf("[gpu-bridge] handshake mismatch\n");
        CloseHandle(device);
        return 2;
    }

    DWORD returned = 0;

    sogen::gpu_bridge::create_instance_response instance{};
    if (!ioctl(device, sogen::gpu_bridge::ioctl_create_instance, nullptr, 0, &instance, sizeof(instance), returned) ||
        instance.vk_result != VK_SUCCESS || instance.instance == sogen::gpu_bridge::null_object)
    {
        std::printf("[gpu-bridge] create_instance failed (VkResult=%d)\n", instance.vk_result);
        CloseHandle(device);
        return 3;
    }
    std::printf("[gpu-bridge] vkCreateInstance -> instance id=%llu\n", static_cast<unsigned long long>(instance.instance));

    // Two-call enumeration: first query the count, then fetch the device ids.
    sogen::gpu_bridge::enumerate_physical_devices_request enum_request{};
    enum_request.instance = instance.instance;
    enum_request.max_count = 0;

    sogen::gpu_bridge::enumerate_physical_devices_response enum_header{};
    ioctl(device, sogen::gpu_bridge::ioctl_enumerate_physical_devices, &enum_request, sizeof(enum_request), &enum_header,
          sizeof(enum_header), returned);
    std::printf("[gpu-bridge] vkEnumeratePhysicalDevices -> count=%u\n", enum_header.count);

    if (enum_header.count > 0)
    {
        enum_request.max_count = enum_header.count;

        std::vector<std::byte> buffer(sizeof(sogen::gpu_bridge::enumerate_physical_devices_response) +
                                      enum_header.count * sizeof(sogen::gpu_bridge::object_id));
        ioctl(device, sogen::gpu_bridge::ioctl_enumerate_physical_devices, &enum_request, sizeof(enum_request), buffer.data(),
              static_cast<DWORD>(buffer.size()), returned);

        const auto* header = reinterpret_cast<sogen::gpu_bridge::enumerate_physical_devices_response*>(buffer.data());
        const auto* devices =
            reinterpret_cast<sogen::gpu_bridge::object_id*>(buffer.data() + sizeof(sogen::gpu_bridge::enumerate_physical_devices_response));

        for (uint32_t i = 0; i < header->count; ++i)
        {
            sogen::gpu_bridge::get_physical_device_properties_request props_request{};
            props_request.physical_device = devices[i];

            VkPhysicalDeviceProperties props{};
            if (!ioctl(device, sogen::gpu_bridge::ioctl_get_physical_device_properties, &props_request, sizeof(props_request), &props,
                       sizeof(props), returned))
            {
                std::printf("[gpu-bridge] get_physical_device_properties[%u] failed: %lu\n", i, GetLastError());
                continue;
            }

            std::printf("[gpu-bridge] device[%u]: '%s' type=%u api=%u.%u.%u\n", i, props.deviceName, props.deviceType,
                        VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
                        VK_API_VERSION_PATCH(props.apiVersion));
        }
    }

    sogen::gpu_bridge::destroy_instance_request destroy_request{};
    destroy_request.instance = instance.instance;
    ioctl(device, sogen::gpu_bridge::ioctl_destroy_instance, &destroy_request, sizeof(destroy_request), nullptr, 0, returned);

    CloseHandle(device);
    std::printf("[gpu-bridge] ok\n");
    return 0;
}
