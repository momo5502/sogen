#pragma once

// Helpers for marshalling VkPhysicalDevice*Features pNext chains across the GPU bridge. Unlike
// gpu_bridge_protocol.hpp (which is dependency-free), this header needs the Vulkan struct layouts, so
// it is included only by the two ends that speak Vulkan: the host wrapper (vulkan_host.cpp) and the
// guest shim (vulkan_shim.cpp) -- never by the emulator-side dispatcher, which only shuttles bytes.
//
// Every VkPhysicalDevice*Features struct is a { VkStructureType sType; void* pNext; } header followed
// by a run of VkBool32 toggles. The header size differs by ABI (8 on x86, 16 on x64) but the bool run
// is identical, so only the bool run crosses the wire. The guest (always pad-free on x86) computes the
// body size and the host echoes it, keeping a 32-bit guest and 64-bit host in agreement.

#include <cstddef>
#include <vulkan/vulkan_core.h>

namespace sogen::gpu_bridge
{
    // { VkStructureType; void* } occupies two pointer slots on both ABIs (x86: 4+4, x64: 4+pad+8).
    inline constexpr size_t feature_chain_header_size = 2 * sizeof(void*);

    // sizeof the named feature struct on THIS architecture, or 0 if the bridge does not know it.
    // Extend this switch when a new required feature struct shows up (the only place to edit).
    inline size_t feature_struct_size(const VkStructureType type)
    {
        switch (type)
        {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2:
            return sizeof(VkPhysicalDeviceFeatures2);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES:
            return sizeof(VkPhysicalDeviceVulkan11Features);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:
            return sizeof(VkPhysicalDeviceVulkan12Features);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES:
            return sizeof(VkPhysicalDeviceVulkan13Features);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT:
            return sizeof(VkPhysicalDeviceRobustness2FeaturesEXT);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR:
            return sizeof(VkPhysicalDeviceMaintenance5FeaturesKHR);
        default:
            return 0;
        }
    }

    // Bytes of VkBool32 body (no header, no trailing padding) for a known feature struct, else 0.
    // Computed on x86 in the guest (pad-free); the host reuses the guest-supplied value.
    inline size_t feature_body_size(const VkStructureType type)
    {
        const size_t total = feature_struct_size(type);
        return total > feature_chain_header_size ? total - feature_chain_header_size : 0;
    }

    // sizeof the named VkPhysicalDevice*Properties struct on THIS architecture, or 0 if unknown. These
    // chain onto VkPhysicalDeviceProperties2; unlike the base properties (which contains a size_t and is
    // not ABI-identical), the bodies of these structs are runs of uint32/uint64/arrays that lay out the
    // same on x86 and x64, so the body bytes cross the wire directly. Extend when a new one is needed.
    inline size_t property_struct_size(const VkStructureType type)
    {
        switch (type)
        {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES:
            return sizeof(VkPhysicalDeviceVulkan11Properties);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES:
            return sizeof(VkPhysicalDeviceVulkan12Properties);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES:
            return sizeof(VkPhysicalDeviceVulkan13Properties);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_PROPERTIES_EXT:
            return sizeof(VkPhysicalDeviceRobustness2PropertiesEXT);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES:
            return sizeof(VkPhysicalDeviceDescriptorIndexingProperties);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES:
            return sizeof(VkPhysicalDeviceMaintenance3Properties);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_PROPERTIES:
            return sizeof(VkPhysicalDeviceMaintenance4Properties);
        default:
            return 0;
        }
    }

    inline size_t property_body_size(const VkStructureType type)
    {
        const size_t total = property_struct_size(type);
        return total > feature_chain_header_size ? total - feature_chain_header_size : 0;
    }
}
