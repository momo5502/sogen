#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace sogen
{
    // Thin host-side wrapper around the real Vulkan driver. Owns the dynamically loaded loader and
    // the mapping from opaque object ids (handed to the guest) to live Vulkan handles, so raw host
    // pointers never cross the bridge.
    //
    // This interface is deliberately free of emulator/guest-Windows types: its implementation
    // includes the host <vulkan/vulkan_core.h> and the platform dynamic loader, which must not be
    // mixed with the emulated Windows definitions used elsewhere in windows-emulator. VkResult is
    // surfaced as int32_t for the same reason.
    class vulkan_host
    {
      public:
        vulkan_host();
        ~vulkan_host();

        vulkan_host(const vulkan_host&) = delete;
        vulkan_host& operator=(const vulkan_host&) = delete;
        vulkan_host(vulkan_host&&) = delete;
        vulkan_host& operator=(vulkan_host&&) = delete;

        // False if no Vulkan driver could be loaded on the host.
        bool available() const;

        // Creates a bare instance (no layers/extensions). out_instance is set to a fresh object id
        // on success, or 0 on failure.
        int32_t create_instance(uint64_t& out_instance);
        void destroy_instance(uint64_t instance);

        // Reports the host physical devices for the instance. out_count always receives the true
        // device count; up to out_devices.size() ids are written. Ids are stable across calls.
        int32_t enumerate_physical_devices(uint64_t instance, std::span<uint64_t> out_devices, uint32_t& out_count);

        // Writes up to out_size bytes of the device's VkPhysicalDeviceProperties into out.
        int32_t get_physical_device_properties(uint64_t physical_device, void* out, size_t out_size);

      private:
        struct impl;
        std::unique_ptr<impl> impl_;
    };
}
