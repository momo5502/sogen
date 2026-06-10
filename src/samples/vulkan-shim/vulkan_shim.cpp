// Guest-side Vulkan shim: a drop-in vulkan-1.dll that implements the Vulkan entry points by
// forwarding them across the Sogen GPU bridge (\\.\SogenGpu) to the host's real driver. An app
// loads it exactly like a normal ICD/loader -- LoadLibrary + vkGetInstanceProcAddr -- and never
// sees the bridge. This is the guest counterpart to the host gpu_bridge io_device.
//
// Opaque Vulkan handles (VkInstance, VkPhysicalDevice, ...) are pointer-sized, so the bridge's
// object_id is stored directly in the handle value; no guest-side handle table is required.

#ifndef NOMINMAX
#define NOMINMAX // keep the Windows min/max macros from clobbering std::min/std::max
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <type_traits>
#include <unordered_map>
#include <vector>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan_win32.h>

#include <gpu_bridge_protocol.hpp>
#include <vk_feature_chain.hpp>

namespace gb = sogen::gpu_bridge;

namespace
{
    HANDLE g_bridge = INVALID_HANDLE_VALUE;

    HANDLE bridge()
    {
        if (g_bridge == INVALID_HANDLE_VALUE)
        {
            g_bridge = CreateFileA(R"(\\.\SogenGpu)", GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        }
        return g_bridge;
    }

    bool bridge_call(uint32_t code, const void* in, DWORD in_len, void* out, DWORD out_len)
    {
        const HANDLE handle = bridge();
        if (handle == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        DWORD returned = 0;
        return DeviceIoControl(handle, code, const_cast<void*>(in), in_len, out, out_len, &returned, nullptr) != FALSE;
    }

    // Vulkan handles come in two shapes: dispatchable handles (VkInstance, VkDevice, VkQueue,
    // VkCommandBuffer, VkPhysicalDevice) are pointers -- pointer-sized, so 64-bit on x64 and 32-bit on
    // x86/WOW64 -- while non-dispatchable handles (VkBuffer, VkImage, VkDeviceMemory, ...) are uint64_t on
    // every platform. The bridge's object_id is a uint64 that always crosses the wire in full; store it
    // directly in uint64 handles, and in the pointer value for dispatchable handles. Object ids are small
    // monotonic counters, so they fit a 32-bit pointer without loss -- and a non-dispatchable handle keeps
    // the full 64 bits regardless of guest bitness. This makes a 32-bit (WOW64) shim and a 64-bit host
    // agree on the protocol, and vice versa.
    template <typename Handle>
    gb::object_id to_object_id(Handle handle)
    {
        if constexpr (std::is_pointer_v<Handle>)
        {
            return static_cast<gb::object_id>(reinterpret_cast<uintptr_t>(handle));
        }
        else
        {
            return static_cast<gb::object_id>(handle);
        }
    }

    template <typename Handle>
    Handle to_handle(gb::object_id id)
    {
        if constexpr (std::is_pointer_v<Handle>)
        {
            return reinterpret_cast<Handle>(static_cast<uintptr_t>(id));
        }
        else
        {
            return static_cast<Handle>(id);
        }
    }

    // VkDeviceMemory is host-side; the guest can't see a host pointer. We emulate vkMapMemory by
    // staging a guest-side copy: download the host range on map, hand the app that buffer, and upload
    // it back on unmap so writes persist. allocationSize is tracked here to resolve VK_WHOLE_SIZE.
    std::unordered_map<gb::object_id, uint64_t> g_memory_sizes;

    struct mapped_range
    {
        std::vector<uint8_t> staging;
        uint64_t offset{};
        uint64_t size{};
    };
    std::unordered_map<gb::object_id, mapped_range> g_mapped_ranges;

    // vkDestroyX(device, child) for the non-dispatchable device children that share device_child_request.
    template <typename Handle>
    void destroy_device_child(uint32_t code, VkDevice device, Handle child)
    {
        if (!child)
        {
            return;
        }
        gb::device_child_request request{};
        request.device = to_object_id(device);
        request.object = to_object_id(child);
        bridge_call(code, &request, sizeof(request), nullptr, 0);
    }

    // Command-buffer recording is batched: instead of one IOCTL per vkCmd*, each command is appended to a
    // per-command-buffer byte stream and the whole stream (begin -> cmds -> end) is flushed to the bridge
    // in a single IOCTL at vkEndCommandBuffer. This amortises the boundary crossing, which dominates
    // emulated frame time. The shim only runs inside the single-threaded emulator, so the map needs no
    // lock. Each record is a gb::command_record_header followed by that command's request payload.
    std::unordered_map<gb::object_id, std::vector<uint8_t>> g_command_streams;

    void record_command(gb::object_id command_buffer, gb::command command, const void* payload, size_t size)
    {
        auto& stream = g_command_streams[command_buffer];
        gb::command_record_header header{.command = static_cast<uint32_t>(command), .size = static_cast<uint32_t>(size)};
        const auto* header_bytes = reinterpret_cast<const uint8_t*>(&header);
        stream.insert(stream.end(), header_bytes, header_bytes + sizeof(header));
        const auto* payload_bytes = reinterpret_cast<const uint8_t*>(payload);
        stream.insert(stream.end(), payload_bytes, payload_bytes + size);
    }

    // OutputDebugStringA is unbuffered; printf mirrors it to stdout, which the emulator captures.
    void shim_log(const char* message)
    {
        OutputDebugStringA(message);
        std::fputs(message, stdout);
        std::fflush(stdout);
    }
}

extern "C"
{
    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo*, const VkAllocationCallbacks*,
                                                                          VkInstance* pInstance)
    {
        gb::create_instance_response response{};
        if (!bridge_call(gb::ioctl_create_instance, nullptr, 0, &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        if (response.vk_result != VK_SUCCESS)
        {
            return static_cast<VkResult>(response.vk_result);
        }

        *pInstance = to_handle<VkInstance>(response.instance);
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks*)
    {
        gb::destroy_instance_request request{};
        request.instance = to_object_id(instance);
        bridge_call(gb::ioctl_destroy_instance, &request, sizeof(request), nullptr, 0);
    }

    // The instance-level enumeration commands are resolved (and required) by loaders such as DXVK
    // before vkCreateInstance. The bridge's vkCreateInstance ignores the requested layers/extensions,
    // so these only need to advertise enough for callers to proceed: no layers, the Win32 WSI
    // instance extensions the bridge supports, and a modern API version.
    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceVersion(uint32_t* pApiVersion)
    {
        if (pApiVersion)
        {
            *pApiVersion = VK_API_VERSION_1_3;
        }
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(uint32_t* pPropertyCount, VkLayerProperties*)
    {
        if (!pPropertyCount)
        {
            return VK_INCOMPLETE;
        }

        *pPropertyCount = 0;
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const char* /*pLayerName*/,
                                                                                                uint32_t* pPropertyCount,
                                                                                                VkExtensionProperties* pProperties)
    {
        static const VkExtensionProperties extensions[] = {
            {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_SURFACE_SPEC_VERSION},
            {VK_KHR_WIN32_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_SPEC_VERSION},
        };
        constexpr auto available = static_cast<uint32_t>(sizeof(extensions) / sizeof(extensions[0]));

        if (!pPropertyCount)
        {
            return VK_INCOMPLETE;
        }

        if (!pProperties)
        {
            *pPropertyCount = available;
            return VK_SUCCESS;
        }

        const uint32_t to_copy = std::min(*pPropertyCount, available);
        for (uint32_t i = 0; i < to_copy; ++i)
        {
            pProperties[i] = extensions[i];
        }
        *pPropertyCount = to_copy;
        return to_copy < available ? VK_INCOMPLETE : VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDevices(VkInstance instance, uint32_t* pCount,
                                                                                    VkPhysicalDevice* pDevices)
    {
        gb::enumerate_physical_devices_request request{};
        request.instance = to_object_id(instance);

        if (!pDevices)
        {
            request.max_count = 0;
            gb::enumerate_physical_devices_response response{};
            if (!bridge_call(gb::ioctl_enumerate_physical_devices, &request, sizeof(request), &response, sizeof(response)))
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }

            *pCount = response.count;
            return static_cast<VkResult>(response.vk_result);
        }

        request.max_count = *pCount;

        std::vector<std::byte> buffer(sizeof(gb::enumerate_physical_devices_response) +
                                      static_cast<size_t>(*pCount) * sizeof(gb::object_id));
        if (!bridge_call(gb::ioctl_enumerate_physical_devices, &request, sizeof(request), buffer.data(), static_cast<DWORD>(buffer.size())))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto* response = reinterpret_cast<const gb::enumerate_physical_devices_response*>(buffer.data());
        const auto* ids = reinterpret_cast<const gb::object_id*>(buffer.data() + sizeof(*response));

        const uint32_t written = (response->count < *pCount) ? response->count : *pCount;
        for (uint32_t i = 0; i < written; ++i)
        {
            pDevices[i] = to_handle<VkPhysicalDevice>(ids[i]);
        }

        *pCount = written;
        return (written < response->count) ? VK_INCOMPLETE : VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice,
                                                                                   VkPhysicalDeviceProperties* pProperties)
    {
        gb::get_physical_device_properties_request request{};
        request.physical_device = to_object_id(physicalDevice);
        bridge_call(gb::ioctl_get_physical_device_properties, &request, sizeof(request), pProperties, sizeof(*pProperties));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice physicalDevice,
                                                                                              uint32_t* pCount,
                                                                                              VkQueueFamilyProperties* pProperties)
    {
        gb::get_queue_family_properties_request request{};
        request.physical_device = to_object_id(physicalDevice);
        request.max_count = pProperties ? *pCount : 0;

        std::vector<std::byte> buffer(sizeof(gb::get_queue_family_properties_response) +
                                      static_cast<size_t>(request.max_count) * sizeof(VkQueueFamilyProperties));
        if (!bridge_call(gb::ioctl_get_queue_family_properties, &request, sizeof(request), buffer.data(),
                         static_cast<DWORD>(buffer.size())))
        {
            *pCount = 0;
            return;
        }

        const auto* response = reinterpret_cast<const gb::get_queue_family_properties_response*>(buffer.data());

        if (!pProperties)
        {
            *pCount = response->count;
            return;
        }

        const uint32_t written = (response->count < *pCount) ? response->count : *pCount;
        const auto* families =
            reinterpret_cast<const VkQueueFamilyProperties*>(buffer.data() + sizeof(gb::get_queue_family_properties_response));
        for (uint32_t i = 0; i < written; ++i)
        {
            pProperties[i] = families[i];
        }
        *pCount = written;
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice physicalDevice,
                                                                        const VkDeviceCreateInfo* pCreateInfo, const VkAllocationCallbacks*,
                                                                        VkDevice* pDevice)
    {
        gb::create_device_request request{};
        request.physical_device = to_object_id(physicalDevice);

        if (pCreateInfo && pCreateInfo->queueCreateInfoCount > 0 && pCreateInfo->pQueueCreateInfos)
        {
            request.queue_family_index = pCreateInfo->pQueueCreateInfos[0].queueFamilyIndex;
            request.queue_count = pCreateInfo->pQueueCreateInfos[0].queueCount;
        }
        else
        {
            request.queue_count = 1;
        }

        // Marshal the enabled device-extension names (NUL-terminated, concatenated).
        std::vector<std::byte> extension_blob;
        uint32_t extension_count = 0;
        if (pCreateInfo && pCreateInfo->ppEnabledExtensionNames)
        {
            for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; ++i)
            {
                const char* name = pCreateInfo->ppEnabledExtensionNames[i];
                if (!name)
                {
                    continue;
                }
                const auto* bytes = reinterpret_cast<const std::byte*>(name);
                extension_blob.insert(extension_blob.end(), bytes, bytes + std::strlen(name) + 1);
                ++extension_count;
            }
        }

        // Marshal the features to enable as a feature_chain_record stream: the base VkPhysicalDeviceFeatures
        // (as a FEATURES_2 record) plus each known chained feature struct. Features arrive either through a
        // VkPhysicalDeviceFeatures2 in pNext (DXVK) or via pEnabledFeatures.
        std::vector<std::byte> feature_blob;
        uint32_t feature_struct_count = 0;
        const auto append_record = [&](VkStructureType type, const void* body_src) {
            const auto body_size = static_cast<uint32_t>(gb::feature_body_size(type));
            const gb::feature_chain_record record{.s_type = static_cast<uint32_t>(type), .body_size = body_size};
            const auto* record_bytes = reinterpret_cast<const std::byte*>(&record);
            feature_blob.insert(feature_blob.end(), record_bytes, record_bytes + sizeof(record));
            if (body_size > 0 && body_src)
            {
                const auto* body_bytes = static_cast<const std::byte*>(body_src);
                feature_blob.insert(feature_blob.end(), body_bytes, body_bytes + body_size);
            }
            ++feature_struct_count;
        };

        bool have_features2 = false;
        if (pCreateInfo)
        {
            for (const auto* next = static_cast<const VkBaseInStructure*>(pCreateInfo->pNext); next; next = next->pNext)
            {
                if (gb::feature_struct_size(next->sType) == 0)
                {
                    continue;
                }
                append_record(next->sType, reinterpret_cast<const uint8_t*>(next) + gb::feature_chain_header_size);
                have_features2 = have_features2 || next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            }
            if (!have_features2 && pCreateInfo->pEnabledFeatures)
            {
                append_record(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, pCreateInfo->pEnabledFeatures);
            }
        }

        request.extension_count = extension_count;
        request.extension_blob_size = static_cast<uint32_t>(extension_blob.size());
        request.feature_struct_count = feature_struct_count;
        request.feature_blob_size = static_cast<uint32_t>(feature_blob.size());

        std::vector<std::byte> in(sizeof(request) + extension_blob.size() + feature_blob.size());
        std::memcpy(in.data(), &request, sizeof(request));
        if (!extension_blob.empty())
        {
            std::memcpy(in.data() + sizeof(request), extension_blob.data(), extension_blob.size());
        }
        if (!feature_blob.empty())
        {
            std::memcpy(in.data() + sizeof(request) + extension_blob.size(), feature_blob.data(), feature_blob.size());
        }

        gb::create_device_response response{};
        if (!bridge_call(gb::ioctl_create_device, in.data(), static_cast<DWORD>(in.size()), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        if (response.vk_result != VK_SUCCESS)
        {
            return static_cast<VkResult>(response.vk_result);
        }

        *pDevice = to_handle<VkDevice>(response.device);
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkDestroyDevice(VkDevice device, const VkAllocationCallbacks*)
    {
        gb::destroy_device_request request{};
        request.device = to_object_id(device);
        bridge_call(gb::ioctl_destroy_device, &request, sizeof(request), nullptr, 0);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex,
                                                                      VkQueue* pQueue)
    {
        gb::get_device_queue_request request{};
        request.device = to_object_id(device);
        request.queue_family_index = queueFamilyIndex;
        request.queue_index = queueIndex;

        gb::get_device_queue_response response{};
        if (!bridge_call(gb::ioctl_get_device_queue, &request, sizeof(request), &response, sizeof(response)))
        {
            *pQueue = VK_NULL_HANDLE;
            return;
        }

        *pQueue = to_handle<VkQueue>(response.queue);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkCreateCommandPool(VkDevice device, const VkCommandPoolCreateInfo* pCreateInfo,
                                                                             const VkAllocationCallbacks*, VkCommandPool* pCommandPool)
    {
        gb::create_command_pool_request request{};
        request.device = to_object_id(device);
        request.queue_family_index = pCreateInfo ? pCreateInfo->queueFamilyIndex : 0;
        request.flags = pCreateInfo ? pCreateInfo->flags : 0;

        gb::create_command_pool_response response{};
        if (!bridge_call(gb::ioctl_create_command_pool, &request, sizeof(request), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (response.vk_result != VK_SUCCESS)
        {
            return static_cast<VkResult>(response.vk_result);
        }

        *pCommandPool = to_handle<VkCommandPool>(response.command_pool);
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkDestroyCommandPool(VkDevice device, VkCommandPool commandPool,
                                                                          const VkAllocationCallbacks*)
    {
        gb::destroy_command_pool_request request{};
        request.device = to_object_id(device);
        request.command_pool = to_object_id(commandPool);
        bridge_call(gb::ioctl_destroy_command_pool, &request, sizeof(request), nullptr, 0);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkAllocateCommandBuffers(VkDevice device,
                                                                                  const VkCommandBufferAllocateInfo* pAllocateInfo,
                                                                                  VkCommandBuffer* pCommandBuffers)
    {
        for (uint32_t i = 0; i < pAllocateInfo->commandBufferCount; ++i)
        {
            gb::allocate_command_buffer_request request{};
            request.device = to_object_id(device);
            request.command_pool = to_object_id(pAllocateInfo->commandPool);

            gb::allocate_command_buffer_response response{};
            if (!bridge_call(gb::ioctl_allocate_command_buffer, &request, sizeof(request), &response, sizeof(response)) ||
                response.vk_result != VK_SUCCESS)
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }

            pCommandBuffers[i] = to_handle<VkCommandBuffer>(response.command_buffer);
        }
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkFreeCommandBuffers(VkDevice device, VkCommandPool commandPool,
                                                                          uint32_t commandBufferCount,
                                                                          const VkCommandBuffer* pCommandBuffers)
    {
        for (uint32_t i = 0; i < commandBufferCount; ++i)
        {
            gb::free_command_buffer_request request{};
            request.device = to_object_id(device);
            request.command_pool = to_object_id(commandPool);
            request.command_buffer = to_object_id(pCommandBuffers[i]);
            bridge_call(gb::ioctl_free_command_buffer, &request, sizeof(request), nullptr, 0);
        }
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkBeginCommandBuffer(VkCommandBuffer commandBuffer,
                                                                              const VkCommandBufferBeginInfo* pBeginInfo)
    {
        // Start a fresh recording stream; the begin is its first record. Flushed at vkEndCommandBuffer.
        gb::begin_command_buffer_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        request.flags = pBeginInfo ? pBeginInfo->flags : 0;

        g_command_streams[request.command_buffer].clear();
        record_command(request.command_buffer, gb::command::begin_command_buffer, &request, sizeof(request));
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer commandBuffer)
    {
        const gb::object_id command_buffer = to_object_id(commandBuffer);
        gb::end_command_buffer_request request{};
        request.command_buffer = command_buffer;
        record_command(command_buffer, gb::command::end_command_buffer, &request, sizeof(request));

        const auto it = g_command_streams.find(command_buffer);
        if (it == g_command_streams.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const std::vector<uint8_t> stream = std::move(it->second);
        g_command_streams.erase(it);

        gb::result_response response{};
        if (!bridge_call(gb::ioctl_record_commands, stream.data(), static_cast<DWORD>(stream.size()), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        return static_cast<VkResult>(response.vk_result);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkCreateFence(VkDevice device, const VkFenceCreateInfo* pCreateInfo,
                                                                       const VkAllocationCallbacks*, VkFence* pFence)
    {
        gb::create_fence_request request{};
        request.device = to_object_id(device);
        request.flags = pCreateInfo ? pCreateInfo->flags : 0;

        gb::create_fence_response response{};
        if (!bridge_call(gb::ioctl_create_fence, &request, sizeof(request), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (response.vk_result != VK_SUCCESS)
        {
            return static_cast<VkResult>(response.vk_result);
        }

        *pFence = to_handle<VkFence>(response.fence);
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkDestroyFence(VkDevice device, VkFence fence, const VkAllocationCallbacks*)
    {
        gb::destroy_fence_request request{};
        request.device = to_object_id(device);
        request.fence = to_object_id(fence);
        bridge_call(gb::ioctl_destroy_fence, &request, sizeof(request), nullptr, 0);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkResetFences(VkDevice device, uint32_t fenceCount, const VkFence* pFences)
    {
        for (uint32_t i = 0; i < fenceCount; ++i)
        {
            gb::reset_fence_request request{};
            request.device = to_object_id(device);
            request.fence = to_object_id(pFences[i]);

            gb::result_response response{};
            if (!bridge_call(gb::ioctl_reset_fence, &request, sizeof(request), &response, sizeof(response)) ||
                response.vk_result != VK_SUCCESS)
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
        }
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkGetFenceStatus(VkDevice, VkFence fence)
    {
        gb::get_fence_status_request request{};
        request.fence = to_object_id(fence);

        gb::result_response response{};
        if (!bridge_call(gb::ioctl_get_fence_status, &request, sizeof(request), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        return static_cast<VkResult>(response.vk_result);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(VkQueue queue, uint32_t submitCount, const VkSubmitInfo* pSubmits,
                                                                       VkFence fence)
    {
        // Count the command buffers so the fence can be attached to the final submission only.
        uint32_t total = 0;
        for (uint32_t s = 0; s < submitCount; ++s)
        {
            total += pSubmits[s].commandBufferCount;
        }

        // A zero-batch submission (no command buffers) is still a valid fence signal in Vulkan. Forward
        // a fence-only submit so a later vkWaitForFences doesn't spin forever on an unsignaled fence.
        if (total == 0)
        {
            if (!fence)
            {
                return VK_SUCCESS;
            }

            gb::queue_submit_request request{};
            request.queue = to_object_id(queue);
            request.command_buffer = gb::null_object;
            request.fence = to_object_id(fence);

            gb::result_response response{};
            if (!bridge_call(gb::ioctl_queue_submit, &request, sizeof(request), &response, sizeof(response)) ||
                response.vk_result != VK_SUCCESS)
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            return VK_SUCCESS;
        }

        uint32_t emitted = 0;
        for (uint32_t s = 0; s < submitCount; ++s)
        {
            for (uint32_t i = 0; i < pSubmits[s].commandBufferCount; ++i)
            {
                ++emitted;

                gb::queue_submit_request request{};
                request.queue = to_object_id(queue);
                request.command_buffer = to_object_id(pSubmits[s].pCommandBuffers[i]);
                request.fence = (emitted == total) ? to_object_id(fence) : gb::null_object;

                gb::result_response response{};
                if (!bridge_call(gb::ioctl_queue_submit, &request, sizeof(request), &response, sizeof(response)) ||
                    response.vk_result != VK_SUCCESS)
                {
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
            }
        }
        return VK_SUCCESS;
    }

    // Poll-and-yield: the host endpoint only ever does a non-blocking vkGetFenceStatus, so the host
    // thread is never blocked on the GPU. We spin here, yielding to the emulator between polls, until
    // the fence(s) signal or the timeout elapses. The real GPU makes progress independently of the
    // emulator thread, so the fence eventually signals.
    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkWaitForFences(VkDevice device, uint32_t fenceCount, const VkFence* pFences,
                                                                         VkBool32 waitAll, uint64_t timeout)
    {
        const ULONGLONG start = GetTickCount64();
        const ULONGLONG timeout_ms = (timeout == UINT64_MAX) ? UINT64_MAX : (timeout / 1000000ULL);

        for (;;)
        {
            uint32_t signaled = 0;
            for (uint32_t i = 0; i < fenceCount; ++i)
            {
                if (vkGetFenceStatus(device, pFences[i]) == VK_SUCCESS)
                {
                    ++signaled;
                }
            }

            if (waitAll ? (signaled == fenceCount) : (signaled > 0))
            {
                return VK_SUCCESS;
            }

            if (timeout_ms != UINT64_MAX && (GetTickCount64() - start) >= timeout_ms)
            {
                return VK_TIMEOUT;
            }

            SwitchToThread();
        }
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL
    vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice physicalDevice, VkPhysicalDeviceMemoryProperties* pMemoryProperties)
    {
        if (!pMemoryProperties)
        {
            return;
        }
        *pMemoryProperties = {};

        gb::get_physical_device_memory_properties_request request{};
        request.physical_device = to_object_id(physicalDevice);
        bridge_call(gb::ioctl_get_physical_device_memory_properties, &request, sizeof(request), pMemoryProperties,
                    sizeof(*pMemoryProperties));
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(VkDevice device, const VkMemoryAllocateInfo* pAllocateInfo,
                                                                          const VkAllocationCallbacks*, VkDeviceMemory* pMemory)
    {
        gb::allocate_memory_request request{};
        request.device = to_object_id(device);
        request.size = pAllocateInfo->allocationSize;
        request.memory_type_index = pAllocateInfo->memoryTypeIndex;

        gb::allocate_memory_response response{};
        if (!bridge_call(gb::ioctl_allocate_memory, &request, sizeof(request), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (response.vk_result != VK_SUCCESS)
        {
            return static_cast<VkResult>(response.vk_result);
        }

        g_memory_sizes[response.memory] = pAllocateInfo->allocationSize;
        *pMemory = to_handle<VkDeviceMemory>(response.memory);
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkFreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks*)
    {
        if (!memory)
        {
            return;
        }
        const gb::object_id mem_id = to_object_id(memory);

        gb::free_memory_request request{};
        request.device = to_object_id(device);
        request.memory = mem_id;
        bridge_call(gb::ioctl_free_memory, &request, sizeof(request), nullptr, 0);

        g_memory_sizes.erase(mem_id);
        g_mapped_ranges.erase(mem_id);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkMapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset,
                                                                     VkDeviceSize size, VkMemoryMapFlags, void** ppData)
    {
        const gb::object_id mem_id = to_object_id(memory);

        uint64_t actual = size;
        if (size == VK_WHOLE_SIZE)
        {
            const auto it = g_memory_sizes.find(mem_id);
            const uint64_t total = (it != g_memory_sizes.end()) ? it->second : 0;
            actual = (total > offset) ? (total - offset) : 0;
        }

        mapped_range range{};
        range.offset = offset;
        range.size = actual;
        range.staging.resize(static_cast<size_t>(actual));

        if (actual > 0)
        {
            gb::download_memory_request request{};
            request.device = to_object_id(device);
            request.memory = mem_id;
            request.offset = offset;
            request.size = actual;
            if (!bridge_call(gb::ioctl_download_memory, &request, sizeof(request), range.staging.data(),
                             static_cast<DWORD>(range.staging.size())))
            {
                return VK_ERROR_MEMORY_MAP_FAILED;
            }
        }

        auto& stored = (g_mapped_ranges[mem_id] = std::move(range));
        *ppData = stored.staging.data();
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkUnmapMemory(VkDevice device, VkDeviceMemory memory)
    {
        const gb::object_id mem_id = to_object_id(memory);
        const auto it = g_mapped_ranges.find(mem_id);
        if (it == g_mapped_ranges.end())
        {
            return;
        }

        const auto& range = it->second;
        if (range.size > 0)
        {
            std::vector<uint8_t> message(sizeof(gb::upload_memory_request) + range.staging.size());
            gb::upload_memory_request header{};
            header.device = to_object_id(device);
            header.memory = mem_id;
            header.offset = range.offset;
            header.size = range.size;
            std::memcpy(message.data(), &header, sizeof(header));
            std::memcpy(message.data() + sizeof(header), range.staging.data(), range.staging.size());

            gb::result_response response{};
            bridge_call(gb::ioctl_upload_memory, message.data(), static_cast<DWORD>(message.size()), &response, sizeof(response));
        }

        g_mapped_ranges.erase(it);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkCreateBuffer(VkDevice device, const VkBufferCreateInfo* pCreateInfo,
                                                                        const VkAllocationCallbacks*, VkBuffer* pBuffer)
    {
        gb::create_buffer_request request{};
        request.device = to_object_id(device);
        request.size = pCreateInfo->size;
        request.usage = pCreateInfo->usage;

        gb::create_buffer_response response{};
        if (!bridge_call(gb::ioctl_create_buffer, &request, sizeof(request), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (response.vk_result != VK_SUCCESS)
        {
            return static_cast<VkResult>(response.vk_result);
        }

        *pBuffer = to_handle<VkBuffer>(response.buffer);
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkDestroyBuffer(VkDevice device, VkBuffer buffer, const VkAllocationCallbacks*)
    {
        if (!buffer)
        {
            return;
        }
        gb::destroy_buffer_request request{};
        request.device = to_object_id(device);
        request.buffer = to_object_id(buffer);
        bridge_call(gb::ioctl_destroy_buffer, &request, sizeof(request), nullptr, 0);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements(VkDevice device, VkBuffer buffer,
                                                                                   VkMemoryRequirements* pMemoryRequirements)
    {
        if (!pMemoryRequirements)
        {
            return;
        }
        *pMemoryRequirements = {};

        gb::get_buffer_memory_requirements_request request{};
        request.device = to_object_id(device);
        request.buffer = to_object_id(buffer);

        gb::memory_requirements_response response{};
        if (!bridge_call(gb::ioctl_get_buffer_memory_requirements, &request, sizeof(request), &response, sizeof(response)))
        {
            return;
        }

        pMemoryRequirements->size = response.size;
        pMemoryRequirements->alignment = response.alignment;
        pMemoryRequirements->memoryTypeBits = response.memory_type_bits;
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory(VkDevice device, VkBuffer buffer, VkDeviceMemory memory,
                                                                            VkDeviceSize memoryOffset)
    {
        gb::bind_buffer_memory_request request{};
        request.device = to_object_id(device);
        request.buffer = to_object_id(buffer);
        request.memory = to_object_id(memory);
        request.offset = memoryOffset;

        gb::result_response response{};
        if (!bridge_call(gb::ioctl_bind_buffer_memory, &request, sizeof(request), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        return static_cast<VkResult>(response.vk_result);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdFillBuffer(VkCommandBuffer commandBuffer, VkBuffer dstBuffer,
                                                                     VkDeviceSize dstOffset, VkDeviceSize size, uint32_t data)
    {
        gb::cmd_fill_buffer_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        request.buffer = to_object_id(dstBuffer);
        request.offset = dstOffset;
        request.size = size;
        request.data = data;
        record_command(request.command_buffer, gb::command::cmd_fill_buffer, &request, sizeof(request));
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage(VkDevice device, const VkImageCreateInfo* pCreateInfo,
                                                                       const VkAllocationCallbacks*, VkImage* pImage)
    {
        gb::create_image_request request{};
        request.device = to_object_id(device);
        request.format = static_cast<uint32_t>(pCreateInfo->format);
        request.width = pCreateInfo->extent.width;
        request.height = pCreateInfo->extent.height;
        request.usage = pCreateInfo->usage;
        request.tiling = static_cast<uint32_t>(pCreateInfo->tiling);

        gb::create_image_response response{};
        if (!bridge_call(gb::ioctl_create_image, &request, sizeof(request), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (response.vk_result != VK_SUCCESS)
        {
            return static_cast<VkResult>(response.vk_result);
        }

        *pImage = to_handle<VkImage>(response.image);
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkDestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks*)
    {
        if (!image)
        {
            return;
        }
        gb::destroy_image_request request{};
        request.device = to_object_id(device);
        request.image = to_object_id(image);
        bridge_call(gb::ioctl_destroy_image, &request, sizeof(request), nullptr, 0);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkGetImageMemoryRequirements(VkDevice device, VkImage image,
                                                                                  VkMemoryRequirements* pMemoryRequirements)
    {
        if (!pMemoryRequirements)
        {
            return;
        }
        *pMemoryRequirements = {};

        gb::get_image_memory_requirements_request request{};
        request.device = to_object_id(device);
        request.image = to_object_id(image);

        gb::memory_requirements_response response{};
        if (!bridge_call(gb::ioctl_get_image_memory_requirements, &request, sizeof(request), &response, sizeof(response)))
        {
            return;
        }

        pMemoryRequirements->size = response.size;
        pMemoryRequirements->alignment = response.alignment;
        pMemoryRequirements->memoryTypeBits = response.memory_type_bits;
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkBindImageMemory(VkDevice device, VkImage image, VkDeviceMemory memory,
                                                                           VkDeviceSize memoryOffset)
    {
        gb::bind_image_memory_request request{};
        request.device = to_object_id(device);
        request.image = to_object_id(image);
        request.memory = to_object_id(memory);
        request.offset = memoryOffset;

        gb::result_response response{};
        if (!bridge_call(gb::ioctl_bind_image_memory, &request, sizeof(request), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        return static_cast<VkResult>(response.vk_result);
    }

    namespace
    {
        gb::image_subresource_range to_wire_range(const VkImageSubresourceRange& range)
        {
            gb::image_subresource_range out{};
            out.aspect_mask = range.aspectMask;
            out.base_mip_level = range.baseMipLevel;
            out.level_count = range.levelCount;
            out.base_array_layer = range.baseArrayLayer;
            out.layer_count = range.layerCount;
            return out;
        }
    }

    // Only image memory barriers are remoted (one IOCTL each, using the global stage masks); memory and
    // buffer barriers are not modeled yet.
    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier(VkCommandBuffer commandBuffer, VkPipelineStageFlags srcStageMask,
                                                                          VkPipelineStageFlags dstStageMask, VkDependencyFlags, uint32_t,
                                                                          const VkMemoryBarrier*, uint32_t, const VkBufferMemoryBarrier*,
                                                                          uint32_t imageMemoryBarrierCount,
                                                                          const VkImageMemoryBarrier* pImageMemoryBarriers)
    {
        for (uint32_t i = 0; i < imageMemoryBarrierCount; ++i)
        {
            const VkImageMemoryBarrier& b = pImageMemoryBarriers[i];
            gb::cmd_pipeline_barrier_request request{};
            request.command_buffer = to_object_id(commandBuffer);
            request.image = to_object_id(b.image);
            request.subresource = to_wire_range(b.subresourceRange);
            request.src_stage_mask = srcStageMask;
            request.dst_stage_mask = dstStageMask;
            request.src_access_mask = b.srcAccessMask;
            request.dst_access_mask = b.dstAccessMask;
            request.old_layout = static_cast<uint32_t>(b.oldLayout);
            request.new_layout = static_cast<uint32_t>(b.newLayout);
            record_command(request.command_buffer, gb::command::cmd_pipeline_barrier, &request, sizeof(request));
        }
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdClearColorImage(VkCommandBuffer commandBuffer, VkImage image,
                                                                          VkImageLayout imageLayout, const VkClearColorValue* pColor,
                                                                          uint32_t rangeCount, const VkImageSubresourceRange* pRanges)
    {
        for (uint32_t i = 0; i < rangeCount; ++i)
        {
            gb::cmd_clear_color_image_request request{};
            request.command_buffer = to_object_id(commandBuffer);
            request.image = to_object_id(image);
            request.subresource = to_wire_range(pRanges[i]);
            request.image_layout = static_cast<uint32_t>(imageLayout);
            request.color_r = pColor->float32[0];
            request.color_g = pColor->float32[1];
            request.color_b = pColor->float32[2];
            request.color_a = pColor->float32[3];
            record_command(request.command_buffer, gb::command::cmd_clear_color_image, &request, sizeof(request));
        }
    }

    // Copies are remoted assuming tight packing of mip 0 / layer 0 at image offset 0 to buffer offset 0
    // (bufferRowLength/bufferImageHeight/imageOffset are not yet honored).
    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdCopyImageToBuffer(VkCommandBuffer commandBuffer, VkImage srcImage,
                                                                            VkImageLayout srcImageLayout, VkBuffer dstBuffer,
                                                                            uint32_t regionCount, const VkBufferImageCopy* pRegions)
    {
        for (uint32_t i = 0; i < regionCount; ++i)
        {
            const VkBufferImageCopy& r = pRegions[i];
            gb::cmd_copy_image_to_buffer_request request{};
            request.command_buffer = to_object_id(commandBuffer);
            request.image = to_object_id(srcImage);
            request.buffer = to_object_id(dstBuffer);
            request.image_layout = static_cast<uint32_t>(srcImageLayout);
            request.width = r.imageExtent.width;
            request.height = r.imageExtent.height;
            request.aspect_mask = r.imageSubresource.aspectMask;
            record_command(request.command_buffer, gb::command::cmd_copy_image_to_buffer, &request, sizeof(request));
        }
    }

    // Copies are remoted assuming tight packing into mip 0 / layer 0 at image offset 0 from buffer
    // offset 0 (bufferRowLength/bufferImageHeight/imageOffset are not yet honored).
    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdCopyBufferToImage(VkCommandBuffer commandBuffer, VkBuffer srcBuffer,
                                                                            VkImage dstImage, VkImageLayout dstImageLayout,
                                                                            uint32_t regionCount, const VkBufferImageCopy* pRegions)
    {
        for (uint32_t i = 0; i < regionCount; ++i)
        {
            const VkBufferImageCopy& r = pRegions[i];
            gb::cmd_copy_buffer_to_image_request request{};
            request.command_buffer = to_object_id(commandBuffer);
            request.buffer = to_object_id(srcBuffer);
            request.image = to_object_id(dstImage);
            request.image_layout = static_cast<uint32_t>(dstImageLayout);
            request.width = r.imageExtent.width;
            request.height = r.imageExtent.height;
            request.aspect_mask = r.imageSubresource.aspectMask;
            record_command(request.command_buffer, gb::command::cmd_copy_buffer_to_image, &request, sizeof(request));
        }
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkCreateWin32SurfaceKHR(VkInstance, const VkWin32SurfaceCreateInfoKHR* pCreateInfo,
                                                                                 const VkAllocationCallbacks*, VkSurfaceKHR* pSurface)
    {
        gb::create_surface_request request{};
        request.hwnd = reinterpret_cast<uint64_t>(pCreateInfo->hwnd);

        gb::create_surface_response response{};
        if (!bridge_call(gb::ioctl_create_surface, &request, sizeof(request), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (response.vk_result != VK_SUCCESS)
        {
            return static_cast<VkResult>(response.vk_result);
        }

        *pSurface = to_handle<VkSurfaceKHR>(response.surface);
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkDestroySurfaceKHR(VkInstance, VkSurfaceKHR surface, const VkAllocationCallbacks*)
    {
        if (!surface)
        {
            return;
        }
        gb::destroy_surface_request request{};
        request.surface = to_object_id(surface);
        bridge_call(gb::ioctl_destroy_surface, &request, sizeof(request), nullptr, 0);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR* pSurfaceCapabilities)
    {
        if (!pSurfaceCapabilities)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        *pSurfaceCapabilities = {};

        gb::get_surface_capabilities_request request{};
        request.physical_device = to_object_id(physicalDevice);
        request.surface = to_object_id(surface);
        if (!bridge_call(gb::ioctl_get_surface_capabilities, &request, sizeof(request), pSurfaceCapabilities,
                         sizeof(*pSurfaceCapabilities)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        return VK_SUCCESS;
    }

    // --- Core physical-device + surface queries (minimal stubs for D3D->Vulkan layers like DXVK) ---
    // The `2` variants delegate to the already-remoted base queries and drop the pNext chain. Features
    // and format support are reported permissively (everything available) so the layer proceeds; this
    // is optimistic — the bridge may not actually support every capability — but it advances bring-up.
    // Surface queries report a single common BGRA format and FIFO present mode.

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures(VkPhysicalDevice, VkPhysicalDeviceFeatures* pFeatures)
    {
        if (!pFeatures)
        {
            return;
        }
        // VkPhysicalDeviceFeatures is a block of VkBool32 toggles; advertise them all as available.
        auto* flags = reinterpret_cast<VkBool32*>(pFeatures);
        for (size_t i = 0; i < sizeof(*pFeatures) / sizeof(VkBool32); ++i)
        {
            flags[i] = VK_TRUE;
        }
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2(VkPhysicalDevice physicalDevice,
                                                                                  VkPhysicalDeviceFeatures2* pFeatures)
    {
        if (!pFeatures)
        {
            return;
        }

        // Collect the caller's chain: the root VkPhysicalDeviceFeatures2 (carrying the base
        // VkPhysicalDeviceFeatures), then each pNext struct. For each we record its sType + pad-free
        // body size and where to write the real values back. Bodies start after the {sType,pNext}
        // header, which is feature_chain_header_size bytes on this ABI.
        struct dest
        {
            uint8_t* body;
            uint32_t body_size;
        };
        std::vector<dest> dests;
        std::vector<gb::feature_chain_record> records;

        const auto add = [&](VkStructureType type, void* base) {
            const auto body_size = static_cast<uint32_t>(gb::feature_body_size(type));
            dests.push_back({.body = reinterpret_cast<uint8_t*>(base) + gb::feature_chain_header_size, .body_size = body_size});
            records.push_back({.s_type = static_cast<uint32_t>(type), .body_size = body_size});
        };

        add(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, pFeatures);
        for (auto* next = static_cast<VkBaseOutStructure*>(pFeatures->pNext); next; next = next->pNext)
        {
            add(next->sType, next);
        }

        std::vector<std::byte> in(sizeof(gb::get_physical_device_features2_request) + records.size() * sizeof(gb::feature_chain_record));
        auto* request = reinterpret_cast<gb::get_physical_device_features2_request*>(in.data());
        request->physical_device = to_object_id(physicalDevice);
        request->struct_count = static_cast<uint32_t>(records.size());
        request->reserved = 0;
        std::memcpy(in.data() + sizeof(*request), records.data(), records.size() * sizeof(gb::feature_chain_record));

        size_t out_capacity = sizeof(gb::get_physical_device_features2_response);
        for (const auto& d : dests)
        {
            out_capacity += sizeof(gb::feature_chain_record) + d.body_size;
        }
        std::vector<std::byte> out(out_capacity);

        if (!bridge_call(gb::ioctl_get_physical_device_features2, in.data(), static_cast<DWORD>(in.size()), out.data(),
                         static_cast<DWORD>(out.size())))
        {
            return;
        }

        const auto* response = reinterpret_cast<const gb::get_physical_device_features2_response*>(out.data());
        if (response->vk_result != VK_SUCCESS)
        {
            return;
        }

        // Records come back in request order, so record i fills dests[i]. Copy the real bool run in.
        size_t offset = sizeof(gb::get_physical_device_features2_response);
        for (uint32_t i = 0; i < response->struct_count && i < dests.size(); ++i)
        {
            if (offset + sizeof(gb::feature_chain_record) > out.size())
            {
                break;
            }
            const auto* record = reinterpret_cast<const gb::feature_chain_record*>(out.data() + offset);
            offset += sizeof(gb::feature_chain_record);
            if (offset + record->body_size > out.size())
            {
                break;
            }

            const uint32_t copy = record->body_size < dests[i].body_size ? record->body_size : dests[i].body_size;
            if (copy > 0)
            {
                std::memcpy(dests[i].body, out.data() + offset, copy);
            }
            offset += record->body_size;
        }
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2(VkPhysicalDevice physicalDevice,
                                                                                    VkPhysicalDeviceProperties2* pProperties)
    {
        if (pProperties)
        {
            vkGetPhysicalDeviceProperties(physicalDevice, &pProperties->properties);
        }
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL
    vkGetPhysicalDeviceMemoryProperties2(VkPhysicalDevice physicalDevice, VkPhysicalDeviceMemoryProperties2* pMemoryProperties)
    {
        if (pMemoryProperties)
        {
            vkGetPhysicalDeviceMemoryProperties(physicalDevice, &pMemoryProperties->memoryProperties);
        }
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties2(VkPhysicalDevice physicalDevice,
                                                                                               uint32_t* pCount,
                                                                                               VkQueueFamilyProperties2* pProperties)
    {
        if (!pCount)
        {
            return;
        }

        if (!pProperties)
        {
            vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, pCount, nullptr);
            return;
        }

        std::vector<VkQueueFamilyProperties> families(*pCount);
        uint32_t count = *pCount;
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &count, families.data());
        for (uint32_t i = 0; i < count; ++i)
        {
            pProperties[i].queueFamilyProperties = families[i];
        }
        *pCount = count;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice, VkFormat,
                                                                                         VkFormatProperties* pFormatProperties)
    {
        if (!pFormatProperties)
        {
            return;
        }
        constexpr VkFormatFeatureFlags image_features =
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
            VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT |
            VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
            VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        constexpr VkFormatFeatureFlags buffer_features =
            VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT | VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT;
        pFormatProperties->linearTilingFeatures = image_features;
        pFormatProperties->optimalTilingFeatures = image_features;
        pFormatProperties->bufferFeatures = buffer_features;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties2(VkPhysicalDevice physicalDevice, VkFormat format,
                                                                                          VkFormatProperties2* pFormatProperties)
    {
        if (!pFormatProperties)
        {
            return;
        }

        vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &pFormatProperties->formatProperties);

        // DXVK reads format features through VkFormatProperties3 (VkFormatFeatureFlags2) chained on
        // pNext, not the legacy VkFormatProperties. Mirror the feature bits there as well; the flag bit
        // values are identical between the legacy and the 64-bit flags. Without this DXVK observes no
        // format support and rejects every render-target format.
        for (auto* next = static_cast<VkBaseOutStructure*>(pFormatProperties->pNext); next != nullptr; next = next->pNext)
        {
            if (next->sType == VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3)
            {
                auto* properties3 = reinterpret_cast<VkFormatProperties3*>(next);
                properties3->linearTilingFeatures = pFormatProperties->formatProperties.linearTilingFeatures;
                properties3->optimalTilingFeatures = pFormatProperties->formatProperties.optimalTilingFeatures;
                properties3->bufferFeatures = pFormatProperties->formatProperties.bufferFeatures;
            }
        }
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL
    vkGetPhysicalDeviceImageFormatProperties(VkPhysicalDevice, VkFormat, VkImageType, VkImageTiling, VkImageUsageFlags, VkImageCreateFlags,
                                             VkImageFormatProperties* pImageFormatProperties)
    {
        if (!pImageFormatProperties)
        {
            return VK_ERROR_FORMAT_NOT_SUPPORTED;
        }
        *pImageFormatProperties = {};
        pImageFormatProperties->maxExtent = {.width = 16384, .height = 16384, .depth = 16384};
        pImageFormatProperties->maxMipLevels = 14;
        pImageFormatProperties->maxArrayLayers = 2048;
        pImageFormatProperties->sampleCounts = VK_SAMPLE_COUNT_1_BIT;
        pImageFormatProperties->maxResourceSize = static_cast<VkDeviceSize>(1) << 31;
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL
    vkGetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceImageFormatInfo2* pImageFormatInfo,
                                              VkImageFormatProperties2* pImageFormatProperties)
    {
        if (!pImageFormatInfo || !pImageFormatProperties)
        {
            return VK_ERROR_FORMAT_NOT_SUPPORTED;
        }
        return vkGetPhysicalDeviceImageFormatProperties(physicalDevice, pImageFormatInfo->format, pImageFormatInfo->type,
                                                        pImageFormatInfo->tiling, pImageFormatInfo->usage, pImageFormatInfo->flags,
                                                        &pImageFormatProperties->imageFormatProperties);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice, uint32_t, VkSurfaceKHR,
                                                                                              VkBool32* pSupported)
    {
        if (pSupported)
        {
            *pSupported = VK_TRUE;
        }
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice, VkSurfaceKHR,
                                                                                              uint32_t* pCount,
                                                                                              VkSurfaceFormatKHR* pSurfaceFormats)
    {
        static const VkSurfaceFormatKHR formats[] = {
            {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
            {VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
        };
        constexpr auto available = static_cast<uint32_t>(sizeof(formats) / sizeof(formats[0]));

        if (!pCount)
        {
            return VK_INCOMPLETE;
        }
        if (!pSurfaceFormats)
        {
            *pCount = available;
            return VK_SUCCESS;
        }

        const uint32_t to_copy = std::min(*pCount, available);
        for (uint32_t i = 0; i < to_copy; ++i)
        {
            pSurfaceFormats[i] = formats[i];
        }
        *pCount = to_copy;
        return to_copy < available ? VK_INCOMPLETE : VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice, VkSurfaceKHR,
                                                                                                   uint32_t* pCount,
                                                                                                   VkPresentModeKHR* pPresentModes)
    {
        static const VkPresentModeKHR modes[] = {VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR};
        constexpr auto available = static_cast<uint32_t>(sizeof(modes) / sizeof(modes[0]));

        if (!pCount)
        {
            return VK_INCOMPLETE;
        }
        if (!pPresentModes)
        {
            *pCount = available;
            return VK_SUCCESS;
        }

        const uint32_t to_copy = std::min(*pCount, available);
        for (uint32_t i = 0; i < to_copy; ++i)
        {
            pPresentModes[i] = modes[i];
        }
        *pCount = to_copy;
        return to_copy < available ? VK_INCOMPLETE : VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR VkBool32 VKAPI_CALL vkGetPhysicalDeviceWin32PresentationSupportKHR(VkPhysicalDevice, uint32_t)
    {
        return VK_TRUE;
    }

    // Core 1.1: the bridge does not model external semaphores, so report none supported (callers fall
    // back to internal synchronization). Core, so a layer can call it without enabling an extension --
    // returning null from vkGetInstanceProcAddr for it would crash such a caller.
    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceExternalSemaphoreProperties(
        VkPhysicalDevice, const VkPhysicalDeviceExternalSemaphoreInfo*, VkExternalSemaphoreProperties* pProperties)
    {
        if (pProperties)
        {
            pProperties->exportFromImportedHandleTypes = 0;
            pProperties->compatibleHandleTypes = 0;
            pProperties->externalSemaphoreFeatures = 0;
        }
    }

    // Core: no sparse residency support.
    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceSparseImageFormatProperties(VkPhysicalDevice, VkFormat, VkImageType,
                                                                                                    VkSampleCountFlagBits,
                                                                                                    VkImageUsageFlags, VkImageTiling,
                                                                                                    uint32_t* pPropertyCount,
                                                                                                    VkSparseImageFormatProperties*)
    {
        if (pPropertyCount)
        {
            *pPropertyCount = 0;
        }
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceSparseImageFormatProperties2(
        VkPhysicalDevice, const VkPhysicalDeviceSparseImageFormatInfo2*, uint32_t* pPropertyCount, VkSparseImageFormatProperties2*)
    {
        if (pPropertyCount)
        {
            *pPropertyCount = 0;
        }
    }

    // VK_KHR_get_surface_capabilities2 / VK_EXT_surface_maintenance1: delegate to the KHR queries.
    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL
    vkGetPhysicalDeviceSurfaceCapabilities2KHR(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceSurfaceInfo2KHR* pSurfaceInfo,
                                               VkSurfaceCapabilities2KHR* pSurfaceCapabilities)
    {
        if (!pSurfaceInfo || !pSurfaceCapabilities)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        return vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, pSurfaceInfo->surface, &pSurfaceCapabilities->surfaceCapabilities);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL
    vkGetPhysicalDeviceSurfaceFormats2KHR(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceSurfaceInfo2KHR* pSurfaceInfo,
                                          uint32_t* pSurfaceFormatCount, VkSurfaceFormat2KHR* pSurfaceFormats)
    {
        if (!pSurfaceInfo || !pSurfaceFormatCount)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (!pSurfaceFormats)
        {
            return vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, pSurfaceInfo->surface, pSurfaceFormatCount, nullptr);
        }

        std::vector<VkSurfaceFormatKHR> formats(*pSurfaceFormatCount);
        uint32_t count = *pSurfaceFormatCount;
        const VkResult result = vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, pSurfaceInfo->surface, &count, formats.data());
        for (uint32_t i = 0; i < count; ++i)
        {
            pSurfaceFormats[i].surfaceFormat = formats[i];
        }
        *pSurfaceFormatCount = count;
        return result;
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL
    vkGetPhysicalDeviceSurfacePresentModes2EXT(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceSurfaceInfo2KHR* pSurfaceInfo,
                                               uint32_t* pPresentModeCount, VkPresentModeKHR* pPresentModes)
    {
        if (!pSurfaceInfo || !pPresentModeCount)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        return vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, pSurfaceInfo->surface, pPresentModeCount, pPresentModes);
    }

    // VK_EXT_debug_utils: accepted but not modeled (labels/messengers are no-ops).
    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdBeginDebugUtilsLabelEXT(VkCommandBuffer, const VkDebugUtilsLabelEXT*)
    {
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdEndDebugUtilsLabelEXT(VkCommandBuffer)
    {
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdInsertDebugUtilsLabelEXT(VkCommandBuffer, const VkDebugUtilsLabelEXT*)
    {
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkCreateDebugUtilsMessengerEXT(VkInstance,
                                                                                        const VkDebugUtilsMessengerCreateInfoEXT*,
                                                                                        const VkAllocationCallbacks*,
                                                                                        VkDebugUtilsMessengerEXT* pMessenger)
    {
        if (pMessenger)
        {
            *pMessenger = to_handle<VkDebugUtilsMessengerEXT>(1);
        }
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkDestroyDebugUtilsMessengerEXT(VkInstance, VkDebugUtilsMessengerEXT,
                                                                                     const VkAllocationCallbacks*)
    {
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkSubmitDebugUtilsMessageEXT(VkInstance, VkDebugUtilsMessageSeverityFlagBitsEXT,
                                                                                  VkDebugUtilsMessageTypeFlagsEXT,
                                                                                  const VkDebugUtilsMessengerCallbackDataEXT*)
    {
    }

    // VK_EXT_swapchain_maintenance1: the bridge's readback present has nothing to release.
    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkReleaseSwapchainImagesEXT(VkDevice, const VkReleaseSwapchainImagesInfoEXT*)
    {
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice,
                                                                                              const char* /*pLayerName*/,
                                                                                              uint32_t* pPropertyCount,
                                                                                              VkExtensionProperties* pProperties)
    {
        if (!pPropertyCount)
        {
            return VK_INCOMPLETE;
        }

        gb::enumerate_device_extension_properties_request request{};
        request.physical_device = to_object_id(physicalDevice);
        request.max_count = pProperties ? *pPropertyCount : 0;

        std::vector<std::byte> buffer(sizeof(gb::enumerate_device_extension_properties_response) +
                                      static_cast<size_t>(request.max_count) * sizeof(VkExtensionProperties));
        if (!bridge_call(gb::ioctl_enumerate_device_extension_properties, &request, sizeof(request), buffer.data(),
                         static_cast<DWORD>(buffer.size())))
        {
            *pPropertyCount = 0;
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto* response = reinterpret_cast<const gb::enumerate_device_extension_properties_response*>(buffer.data());
        if (response->vk_result != VK_SUCCESS)
        {
            *pPropertyCount = 0;
            return static_cast<VkResult>(response->vk_result);
        }

        if (!pProperties)
        {
            *pPropertyCount = response->count;
            return VK_SUCCESS;
        }

        const uint32_t written = (response->count < *pPropertyCount) ? response->count : *pPropertyCount;
        const auto* extensions =
            reinterpret_cast<const VkExtensionProperties*>(buffer.data() + sizeof(gb::enumerate_device_extension_properties_response));
        for (uint32_t i = 0; i < written; ++i)
        {
            pProperties[i] = extensions[i];
        }
        *pPropertyCount = written;
        return written < response->count ? VK_INCOMPLETE : VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo,
                                                                              const VkAllocationCallbacks*, VkSwapchainKHR* pSwapchain)
    {
        gb::create_swapchain_request request{};
        request.device = to_object_id(device);
        request.surface = to_object_id(pCreateInfo->surface);
        request.format = static_cast<uint32_t>(pCreateInfo->imageFormat);
        request.width = pCreateInfo->imageExtent.width;
        request.height = pCreateInfo->imageExtent.height;
        request.min_image_count = pCreateInfo->minImageCount;
        request.image_usage = pCreateInfo->imageUsage;
        request.present_mode = static_cast<uint32_t>(pCreateInfo->presentMode);

        gb::create_swapchain_response response{};
        if (!bridge_call(gb::ioctl_create_swapchain, &request, sizeof(request), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (response.vk_result != VK_SUCCESS)
        {
            return static_cast<VkResult>(response.vk_result);
        }

        *pSwapchain = to_handle<VkSwapchainKHR>(response.swapchain);
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                                           const VkAllocationCallbacks*)
    {
        if (!swapchain)
        {
            return;
        }
        gb::destroy_swapchain_request request{};
        request.device = to_object_id(device);
        request.swapchain = to_object_id(swapchain);
        bridge_call(gb::ioctl_destroy_swapchain, &request, sizeof(request), nullptr, 0);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkGetSwapchainImagesKHR(VkDevice, VkSwapchainKHR swapchain,
                                                                                 uint32_t* pSwapchainImageCount, VkImage* pSwapchainImages)
    {
        const uint32_t capacity = pSwapchainImages ? *pSwapchainImageCount : 0;

        gb::get_swapchain_images_request request{};
        request.swapchain = to_object_id(swapchain);
        request.max_count = capacity;

        std::vector<uint8_t> out(sizeof(gb::get_swapchain_images_response) + capacity * sizeof(gb::object_id));
        if (!bridge_call(gb::ioctl_get_swapchain_images, &request, sizeof(request), out.data(), static_cast<DWORD>(out.size())))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        gb::get_swapchain_images_response header{};
        std::memcpy(&header, out.data(), sizeof(header));
        if (header.vk_result != VK_SUCCESS)
        {
            return static_cast<VkResult>(header.vk_result);
        }

        if (!pSwapchainImages)
        {
            *pSwapchainImageCount = header.count;
            return VK_SUCCESS;
        }

        const uint32_t to_write = (header.count < capacity) ? header.count : capacity;
        const auto* ids = reinterpret_cast<const gb::object_id*>(out.data() + sizeof(header));
        for (uint32_t i = 0; i < to_write; ++i)
        {
            pSwapchainImages[i] = to_handle<VkImage>(ids[i]);
        }
        *pSwapchainImageCount = to_write;
        return (to_write < header.count) ? VK_INCOMPLETE : VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImageKHR(VkDevice, VkSwapchainKHR swapchain, uint64_t, VkSemaphore,
                                                                               VkFence, uint32_t* pImageIndex)
    {
        // semaphore/fence are ignored: the bridge's images are always immediately available.
        gb::acquire_next_image_request request{};
        request.swapchain = to_object_id(swapchain);

        gb::acquire_next_image_response response{};
        if (!bridge_call(gb::ioctl_acquire_next_image, &request, sizeof(request), &response, sizeof(response)))
        {
            return VK_ERROR_OUT_OF_DATE_KHR;
        }
        if (response.vk_result != VK_SUCCESS)
        {
            return static_cast<VkResult>(response.vk_result);
        }

        *pImageIndex = response.image_index;
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo)
    {
        VkResult overall = VK_SUCCESS;
        for (uint32_t i = 0; i < pPresentInfo->swapchainCount; ++i)
        {
            gb::queue_present_request request{};
            request.queue = to_object_id(queue);
            request.swapchain = to_object_id(pPresentInfo->pSwapchains[i]);
            request.image_index = pPresentInfo->pImageIndices[i];

            gb::result_response response{};
            const bool ok = bridge_call(gb::ioctl_queue_present, &request, sizeof(request), &response, sizeof(response));
            const VkResult result = ok ? static_cast<VkResult>(response.vk_result) : VK_ERROR_INITIALIZATION_FAILED;
            if (pPresentInfo->pResults)
            {
                pPresentInfo->pResults[i] = result;
            }
            if (result != VK_SUCCESS)
            {
                overall = result;
            }
        }
        return overall;
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkCreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo* pCreateInfo,
                                                                              const VkAllocationCallbacks*, VkShaderModule* pShaderModule)
    {
        const auto code_size = static_cast<uint32_t>(pCreateInfo->codeSize);
        std::vector<uint8_t> message(sizeof(gb::create_shader_module_request) + code_size);
        gb::create_shader_module_request header{};
        header.device = to_object_id(device);
        header.code_size = code_size;
        std::memcpy(message.data(), &header, sizeof(header));
        std::memcpy(message.data() + sizeof(header), pCreateInfo->pCode, code_size);

        gb::object_response response{};
        if (!bridge_call(gb::ioctl_create_shader_module, message.data(), static_cast<DWORD>(message.size()), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (response.vk_result != VK_SUCCESS)
        {
            return static_cast<VkResult>(response.vk_result);
        }
        *pShaderModule = to_handle<VkShaderModule>(response.object);
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkDestroyShaderModule(VkDevice device, VkShaderModule shaderModule,
                                                                           const VkAllocationCallbacks*)
    {
        destroy_device_child(gb::ioctl_destroy_shader_module, device, shaderModule);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(VkDevice device, const VkImageViewCreateInfo* pCreateInfo,
                                                                           const VkAllocationCallbacks*, VkImageView* pView)
    {
        gb::create_image_view_request request{};
        request.device = to_object_id(device);
        request.image = to_object_id(pCreateInfo->image);
        request.format = static_cast<uint32_t>(pCreateInfo->format);
        request.aspect_mask = pCreateInfo->subresourceRange.aspectMask;

        gb::object_response response{};
        if (!bridge_call(gb::ioctl_create_image_view, &request, sizeof(request), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (response.vk_result != VK_SUCCESS)
        {
            return static_cast<VkResult>(response.vk_result);
        }
        *pView = to_handle<VkImageView>(response.object);
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkDestroyImageView(VkDevice device, VkImageView imageView,
                                                                        const VkAllocationCallbacks*)
    {
        destroy_device_child(gb::ioctl_destroy_image_view, device, imageView);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkCreateRenderPass(VkDevice device, const VkRenderPassCreateInfo* pCreateInfo,
                                                                            const VkAllocationCallbacks*, VkRenderPass* pRenderPass)
    {
        // The first attachment drives the color attachment; a subpass depth-stencil attachment (if any)
        // contributes its format so the bridge adds a matching depth attachment.
        const VkAttachmentDescription& a = pCreateInfo->pAttachments[0];
        gb::create_render_pass_request request{};
        request.device = to_object_id(device);
        request.format = static_cast<uint32_t>(a.format);
        request.load_op = static_cast<uint32_t>(a.loadOp);
        request.store_op = static_cast<uint32_t>(a.storeOp);
        request.initial_layout = static_cast<uint32_t>(a.initialLayout);
        request.final_layout = static_cast<uint32_t>(a.finalLayout);
        if (pCreateInfo->subpassCount > 0 && pCreateInfo->pSubpasses[0].pDepthStencilAttachment)
        {
            const uint32_t depth_index = pCreateInfo->pSubpasses[0].pDepthStencilAttachment->attachment;
            if (depth_index != VK_ATTACHMENT_UNUSED && depth_index < pCreateInfo->attachmentCount)
            {
                request.depth_format = static_cast<uint32_t>(pCreateInfo->pAttachments[depth_index].format);
            }
        }

        gb::object_response response{};
        if (!bridge_call(gb::ioctl_create_render_pass, &request, sizeof(request), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (response.vk_result != VK_SUCCESS)
        {
            return static_cast<VkResult>(response.vk_result);
        }
        *pRenderPass = to_handle<VkRenderPass>(response.object);
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkDestroyRenderPass(VkDevice device, VkRenderPass renderPass,
                                                                         const VkAllocationCallbacks*)
    {
        destroy_device_child(gb::ioctl_destroy_render_pass, device, renderPass);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkCreateFramebuffer(VkDevice device, const VkFramebufferCreateInfo* pCreateInfo,
                                                                             const VkAllocationCallbacks*, VkFramebuffer* pFramebuffer)
    {
        gb::create_framebuffer_request request{};
        request.device = to_object_id(device);
        request.render_pass = to_object_id(pCreateInfo->renderPass);
        request.image_view = to_object_id(pCreateInfo->pAttachments[0]); // color attachment
        request.depth_view = (pCreateInfo->attachmentCount >= 2) ? to_object_id(pCreateInfo->pAttachments[1]) : gb::null_object;
        request.width = pCreateInfo->width;
        request.height = pCreateInfo->height;

        gb::object_response response{};
        if (!bridge_call(gb::ioctl_create_framebuffer, &request, sizeof(request), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (response.vk_result != VK_SUCCESS)
        {
            return static_cast<VkResult>(response.vk_result);
        }
        *pFramebuffer = to_handle<VkFramebuffer>(response.object);
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkDestroyFramebuffer(VkDevice device, VkFramebuffer framebuffer,
                                                                          const VkAllocationCallbacks*)
    {
        destroy_device_child(gb::ioctl_destroy_framebuffer, device, framebuffer);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkCreatePipelineLayout(VkDevice device,
                                                                                const VkPipelineLayoutCreateInfo* pCreateInfo,
                                                                                const VkAllocationCallbacks*,
                                                                                VkPipelineLayout* pPipelineLayout)
    {
        gb::create_pipeline_layout_request request{};
        request.device = to_object_id(device);
        // One push-constant block from offset 0 is modeled: OR the stages, take the widest extent.
        for (uint32_t i = 0; i < pCreateInfo->pushConstantRangeCount; ++i)
        {
            const VkPushConstantRange& r = pCreateInfo->pPushConstantRanges[i];
            request.push_constant_stages |= r.stageFlags;
            request.push_constant_size = std::max(request.push_constant_size, r.offset + r.size);
        }
        request.set_layout_count = pCreateInfo->setLayoutCount;

        // The descriptor-set-layout ids trail the header.
        std::vector<uint8_t> message(sizeof(request) + static_cast<size_t>(request.set_layout_count) * sizeof(gb::object_id));
        std::memcpy(message.data(), &request, sizeof(request));
        for (uint32_t i = 0; i < request.set_layout_count; ++i)
        {
            const gb::object_id id = to_object_id(pCreateInfo->pSetLayouts[i]);
            std::memcpy(message.data() + sizeof(request) + i * sizeof(id), &id, sizeof(id));
        }

        gb::object_response response{};
        if (!bridge_call(gb::ioctl_create_pipeline_layout, message.data(), static_cast<DWORD>(message.size()), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (response.vk_result != VK_SUCCESS)
        {
            return static_cast<VkResult>(response.vk_result);
        }
        *pPipelineLayout = to_handle<VkPipelineLayout>(response.object);
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkDestroyPipelineLayout(VkDevice device, VkPipelineLayout pipelineLayout,
                                                                             const VkAllocationCallbacks*)
    {
        destroy_device_child(gb::ioctl_destroy_pipeline_layout, device, pipelineLayout);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorSetLayout(VkDevice device,
                                                                                     const VkDescriptorSetLayoutCreateInfo* pCreateInfo,
                                                                                     const VkAllocationCallbacks*,
                                                                                     VkDescriptorSetLayout* pSetLayout)
    {
        gb::create_descriptor_set_layout_request header{};
        header.device = to_object_id(device);
        header.binding_count = pCreateInfo->bindingCount;

        std::vector<uint8_t> message(sizeof(header) +
                                     static_cast<size_t>(header.binding_count) * sizeof(gb::descriptor_set_layout_binding));
        std::memcpy(message.data(), &header, sizeof(header));
        for (uint32_t i = 0; i < header.binding_count; ++i)
        {
            const VkDescriptorSetLayoutBinding& b = pCreateInfo->pBindings[i];
            gb::descriptor_set_layout_binding wire{};
            wire.binding = b.binding;
            wire.descriptor_type = static_cast<uint32_t>(b.descriptorType);
            wire.descriptor_count = b.descriptorCount;
            wire.stage_flags = b.stageFlags;
            std::memcpy(message.data() + sizeof(header) + i * sizeof(wire), &wire, sizeof(wire));
        }

        gb::object_response response{};
        if (!bridge_call(gb::ioctl_create_descriptor_set_layout, message.data(), static_cast<DWORD>(message.size()), &response,
                         sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (response.vk_result != VK_SUCCESS)
        {
            return static_cast<VkResult>(response.vk_result);
        }
        *pSetLayout = to_handle<VkDescriptorSetLayout>(response.object);
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorSetLayout(VkDevice device,
                                                                                  VkDescriptorSetLayout descriptorSetLayout,
                                                                                  const VkAllocationCallbacks*)
    {
        destroy_device_child(gb::ioctl_destroy_descriptor_set_layout, device, descriptorSetLayout);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorPool(VkDevice device,
                                                                                const VkDescriptorPoolCreateInfo* pCreateInfo,
                                                                                const VkAllocationCallbacks*,
                                                                                VkDescriptorPool* pDescriptorPool)
    {
        gb::create_descriptor_pool_request header{};
        header.device = to_object_id(device);
        header.max_sets = pCreateInfo->maxSets;
        header.pool_size_count = pCreateInfo->poolSizeCount;

        std::vector<uint8_t> message(sizeof(header) + static_cast<size_t>(header.pool_size_count) * sizeof(gb::descriptor_pool_size));
        std::memcpy(message.data(), &header, sizeof(header));
        for (uint32_t i = 0; i < header.pool_size_count; ++i)
        {
            gb::descriptor_pool_size wire{};
            wire.descriptor_type = static_cast<uint32_t>(pCreateInfo->pPoolSizes[i].type);
            wire.descriptor_count = pCreateInfo->pPoolSizes[i].descriptorCount;
            std::memcpy(message.data() + sizeof(header) + i * sizeof(wire), &wire, sizeof(wire));
        }

        gb::object_response response{};
        if (!bridge_call(gb::ioctl_create_descriptor_pool, message.data(), static_cast<DWORD>(message.size()), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (response.vk_result != VK_SUCCESS)
        {
            return static_cast<VkResult>(response.vk_result);
        }
        *pDescriptorPool = to_handle<VkDescriptorPool>(response.object);
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorPool(VkDevice device, VkDescriptorPool descriptorPool,
                                                                             const VkAllocationCallbacks*)
    {
        destroy_device_child(gb::ioctl_destroy_descriptor_pool, device, descriptorPool);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkAllocateDescriptorSets(VkDevice device,
                                                                                  const VkDescriptorSetAllocateInfo* pAllocateInfo,
                                                                                  VkDescriptorSet* pDescriptorSets)
    {
        const uint32_t count = pAllocateInfo->descriptorSetCount;
        gb::allocate_descriptor_sets_request header{};
        header.device = to_object_id(device);
        header.descriptor_pool = to_object_id(pAllocateInfo->descriptorPool);
        header.set_count = count;

        std::vector<uint8_t> message(sizeof(header) + static_cast<size_t>(count) * sizeof(gb::object_id));
        std::memcpy(message.data(), &header, sizeof(header));
        for (uint32_t i = 0; i < count; ++i)
        {
            const gb::object_id id = to_object_id(pAllocateInfo->pSetLayouts[i]);
            std::memcpy(message.data() + sizeof(header) + i * sizeof(id), &id, sizeof(id));
        }

        std::vector<uint8_t> out(sizeof(gb::allocate_descriptor_sets_response) + static_cast<size_t>(count) * sizeof(gb::object_id));
        if (!bridge_call(gb::ioctl_allocate_descriptor_sets, message.data(), static_cast<DWORD>(message.size()), out.data(),
                         static_cast<DWORD>(out.size())))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        gb::allocate_descriptor_sets_response resp_header{};
        std::memcpy(&resp_header, out.data(), sizeof(resp_header));
        if (resp_header.vk_result != VK_SUCCESS)
        {
            return static_cast<VkResult>(resp_header.vk_result);
        }

        const auto* ids = reinterpret_cast<const gb::object_id*>(out.data() + sizeof(resp_header));
        const uint32_t written = (resp_header.count < count) ? resp_header.count : count;
        for (uint32_t i = 0; i < written; ++i)
        {
            pDescriptorSets[i] = to_handle<VkDescriptorSet>(ids[i]);
        }
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkUpdateDescriptorSets(VkDevice device, uint32_t descriptorWriteCount,
                                                                            const VkWriteDescriptorSet* pDescriptorWrites, uint32_t,
                                                                            const VkCopyDescriptorSet*)
    {
        // Descriptor copies are not modeled; only writes are forwarded. Each VkWriteDescriptorSet is
        // flattened to `descriptorCount` single-descriptor wire writes.
        std::vector<gb::descriptor_write> writes;
        for (uint32_t w = 0; w < descriptorWriteCount; ++w)
        {
            const VkWriteDescriptorSet& src = pDescriptorWrites[w];
            for (uint32_t e = 0; e < src.descriptorCount; ++e)
            {
                gb::descriptor_write wire{};
                wire.dst_set = to_object_id(src.dstSet);
                wire.dst_binding = src.dstBinding;
                wire.dst_array_element = src.dstArrayElement + e;
                wire.descriptor_type = static_cast<uint32_t>(src.descriptorType);
                if (src.pBufferInfo)
                {
                    wire.buffer = to_object_id(src.pBufferInfo[e].buffer);
                    wire.offset = src.pBufferInfo[e].offset;
                    wire.range = src.pBufferInfo[e].range;
                }
                if (src.pImageInfo)
                {
                    wire.sampler = to_object_id(src.pImageInfo[e].sampler);
                    wire.image_view = to_object_id(src.pImageInfo[e].imageView);
                    wire.image_layout = static_cast<uint32_t>(src.pImageInfo[e].imageLayout);
                }
                writes.push_back(wire);
            }
        }

        gb::update_descriptor_sets_request header{};
        header.device = to_object_id(device);
        header.write_count = static_cast<uint32_t>(writes.size());

        std::vector<uint8_t> message(sizeof(header) + writes.size() * sizeof(gb::descriptor_write));
        std::memcpy(message.data(), &header, sizeof(header));
        if (!writes.empty())
        {
            std::memcpy(message.data() + sizeof(header), writes.data(), writes.size() * sizeof(gb::descriptor_write));
        }

        gb::result_response response{};
        bridge_call(gb::ioctl_update_descriptor_sets, message.data(), static_cast<DWORD>(message.size()), &response, sizeof(response));
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkCreateSampler(VkDevice device, const VkSamplerCreateInfo* pCreateInfo,
                                                                         const VkAllocationCallbacks*, VkSampler* pSampler)
    {
        gb::create_sampler_request request{};
        request.device = to_object_id(device);
        request.mag_filter = static_cast<uint32_t>(pCreateInfo->magFilter);
        request.min_filter = static_cast<uint32_t>(pCreateInfo->minFilter);
        request.address_mode_u = static_cast<uint32_t>(pCreateInfo->addressModeU);
        request.address_mode_v = static_cast<uint32_t>(pCreateInfo->addressModeV);
        request.address_mode_w = static_cast<uint32_t>(pCreateInfo->addressModeW);

        gb::object_response response{};
        if (!bridge_call(gb::ioctl_create_sampler, &request, sizeof(request), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (response.vk_result != VK_SUCCESS)
        {
            return static_cast<VkResult>(response.vk_result);
        }
        *pSampler = to_handle<VkSampler>(response.object);
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkDestroySampler(VkDevice device, VkSampler sampler, const VkAllocationCallbacks*)
    {
        destroy_device_child(gb::ioctl_destroy_sampler, device, sampler);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkCreateGraphicsPipelines(VkDevice device, VkPipelineCache,
                                                                                   uint32_t createInfoCount,
                                                                                   const VkGraphicsPipelineCreateInfo* pCreateInfos,
                                                                                   const VkAllocationCallbacks*, VkPipeline* pPipelines)
    {
        VkResult overall = VK_SUCCESS;
        for (uint32_t i = 0; i < createInfoCount; ++i)
        {
            const VkGraphicsPipelineCreateInfo& ci = pCreateInfos[i];

            gb::object_id vertex_shader = gb::null_object;
            gb::object_id fragment_shader = gb::null_object;
            for (uint32_t s = 0; s < ci.stageCount; ++s)
            {
                if (ci.pStages[s].stage == VK_SHADER_STAGE_VERTEX_BIT)
                {
                    vertex_shader = to_object_id(ci.pStages[s].module);
                }
                else if (ci.pStages[s].stage == VK_SHADER_STAGE_FRAGMENT_BIT)
                {
                    fragment_shader = to_object_id(ci.pStages[s].module);
                }
            }

            uint32_t width = 0;
            uint32_t height = 0;
            if (ci.pViewportState && ci.pViewportState->pViewports && ci.pViewportState->viewportCount > 0)
            {
                width = static_cast<uint32_t>(ci.pViewportState->pViewports[0].width);
                height = static_cast<uint32_t>(ci.pViewportState->pViewports[0].height);
            }

            // Vertex input state (variable-length): flatten the binding/attribute descriptions after the
            // request header. Empty state (vertices baked into the shader) just sends counts of 0.
            uint32_t binding_count = 0;
            uint32_t attribute_count = 0;
            const VkVertexInputBindingDescription* vk_bindings = nullptr;
            const VkVertexInputAttributeDescription* vk_attributes = nullptr;
            if (ci.pVertexInputState)
            {
                binding_count = ci.pVertexInputState->vertexBindingDescriptionCount;
                attribute_count = ci.pVertexInputState->vertexAttributeDescriptionCount;
                vk_bindings = ci.pVertexInputState->pVertexBindingDescriptions;
                vk_attributes = ci.pVertexInputState->pVertexAttributeDescriptions;
            }

            gb::create_graphics_pipeline_request request{};
            request.device = to_object_id(device);
            request.render_pass = to_object_id(ci.renderPass);
            request.pipeline_layout = to_object_id(ci.layout);
            request.vertex_shader = vertex_shader;
            request.fragment_shader = fragment_shader;
            request.width = width;
            request.height = height;
            if (ci.pDepthStencilState && ci.pDepthStencilState->depthTestEnable)
            {
                request.depth_test_enable = 1;
                request.depth_write_enable = ci.pDepthStencilState->depthWriteEnable ? 1 : 0;
                request.depth_compare_op = static_cast<uint32_t>(ci.pDepthStencilState->depthCompareOp);
            }
            request.binding_count = binding_count;
            request.attribute_count = attribute_count;

            std::vector<uint8_t> message(sizeof(request) + static_cast<size_t>(binding_count) * sizeof(gb::vertex_input_binding) +
                                         static_cast<size_t>(attribute_count) * sizeof(gb::vertex_input_attribute));
            std::memcpy(message.data(), &request, sizeof(request));
            size_t cursor = sizeof(request);
            for (uint32_t b = 0; b < binding_count; ++b)
            {
                gb::vertex_input_binding wire{};
                wire.binding = vk_bindings[b].binding;
                wire.stride = vk_bindings[b].stride;
                wire.input_rate = static_cast<uint32_t>(vk_bindings[b].inputRate);
                std::memcpy(message.data() + cursor, &wire, sizeof(wire));
                cursor += sizeof(wire);
            }
            for (uint32_t a = 0; a < attribute_count; ++a)
            {
                gb::vertex_input_attribute wire{};
                wire.location = vk_attributes[a].location;
                wire.binding = vk_attributes[a].binding;
                wire.format = static_cast<uint32_t>(vk_attributes[a].format);
                wire.offset = vk_attributes[a].offset;
                std::memcpy(message.data() + cursor, &wire, sizeof(wire));
                cursor += sizeof(wire);
            }

            gb::object_response response{};
            const bool ok = bridge_call(gb::ioctl_create_graphics_pipeline, message.data(), static_cast<DWORD>(message.size()), &response,
                                        sizeof(response));
            if (!ok || response.vk_result != VK_SUCCESS)
            {
                pPipelines[i] = VK_NULL_HANDLE;
                overall = ok ? static_cast<VkResult>(response.vk_result) : VK_ERROR_INITIALIZATION_FAILED;
            }
            else
            {
                pPipelines[i] = to_handle<VkPipeline>(response.object);
            }
        }
        return overall;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkDestroyPipeline(VkDevice device, VkPipeline pipeline, const VkAllocationCallbacks*)
    {
        destroy_device_child(gb::ioctl_destroy_pipeline, device, pipeline);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdBeginRenderPass(VkCommandBuffer commandBuffer,
                                                                          const VkRenderPassBeginInfo* pRenderPassBegin, VkSubpassContents)
    {
        gb::cmd_begin_render_pass_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        request.render_pass = to_object_id(pRenderPassBegin->renderPass);
        request.framebuffer = to_object_id(pRenderPassBegin->framebuffer);
        request.width = pRenderPassBegin->renderArea.extent.width;
        request.height = pRenderPassBegin->renderArea.extent.height;
        request.clear_depth = 1.0f;
        if (pRenderPassBegin->clearValueCount > 0 && pRenderPassBegin->pClearValues)
        {
            request.clear_r = pRenderPassBegin->pClearValues[0].color.float32[0];
            request.clear_g = pRenderPassBegin->pClearValues[0].color.float32[1];
            request.clear_b = pRenderPassBegin->pClearValues[0].color.float32[2];
            request.clear_a = pRenderPassBegin->pClearValues[0].color.float32[3];
            if (pRenderPassBegin->clearValueCount >= 2)
            {
                request.clear_depth = pRenderPassBegin->pClearValues[1].depthStencil.depth;
            }
        }
        record_command(request.command_buffer, gb::command::cmd_begin_render_pass, &request, sizeof(request));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdBindPipeline(VkCommandBuffer commandBuffer, VkPipelineBindPoint,
                                                                       VkPipeline pipeline)
    {
        gb::cmd_bind_pipeline_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        request.pipeline = to_object_id(pipeline);
        record_command(request.command_buffer, gb::command::cmd_bind_pipeline, &request, sizeof(request));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdDraw(VkCommandBuffer commandBuffer, uint32_t vertexCount, uint32_t instanceCount,
                                                               uint32_t firstVertex, uint32_t firstInstance)
    {
        gb::cmd_draw_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        request.vertex_count = vertexCount;
        request.instance_count = instanceCount;
        request.first_vertex = firstVertex;
        request.first_instance = firstInstance;
        record_command(request.command_buffer, gb::command::cmd_draw, &request, sizeof(request));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdBindVertexBuffers(VkCommandBuffer commandBuffer, uint32_t firstBinding,
                                                                            uint32_t bindingCount, const VkBuffer* pBuffers,
                                                                            const VkDeviceSize* pOffsets)
    {
        const gb::object_id command_buffer = to_object_id(commandBuffer);
        std::vector<uint8_t> message(sizeof(gb::cmd_bind_vertex_buffers_request) +
                                     static_cast<size_t>(bindingCount) * sizeof(gb::vertex_buffer_binding));
        gb::cmd_bind_vertex_buffers_request header{};
        header.command_buffer = command_buffer;
        header.first_binding = firstBinding;
        header.binding_count = bindingCount;
        std::memcpy(message.data(), &header, sizeof(header));
        for (uint32_t i = 0; i < bindingCount; ++i)
        {
            gb::vertex_buffer_binding vb{};
            vb.buffer = to_object_id(pBuffers[i]);
            vb.offset = pOffsets ? pOffsets[i] : 0;
            std::memcpy(message.data() + sizeof(header) + i * sizeof(vb), &vb, sizeof(vb));
        }
        record_command(command_buffer, gb::command::cmd_bind_vertex_buffers, message.data(), message.size());
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdBindIndexBuffer(VkCommandBuffer commandBuffer, VkBuffer buffer,
                                                                          VkDeviceSize offset, VkIndexType indexType)
    {
        gb::cmd_bind_index_buffer_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        request.buffer = to_object_id(buffer);
        request.offset = offset;
        request.index_type = static_cast<uint32_t>(indexType);
        record_command(request.command_buffer, gb::command::cmd_bind_index_buffer, &request, sizeof(request));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexed(VkCommandBuffer commandBuffer, uint32_t indexCount,
                                                                      uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset,
                                                                      uint32_t firstInstance)
    {
        gb::cmd_draw_indexed_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        request.index_count = indexCount;
        request.instance_count = instanceCount;
        request.first_index = firstIndex;
        request.vertex_offset = vertexOffset;
        request.first_instance = firstInstance;
        record_command(request.command_buffer, gb::command::cmd_draw_indexed, &request, sizeof(request));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets(VkCommandBuffer commandBuffer, VkPipelineBindPoint,
                                                                             VkPipelineLayout layout, uint32_t firstSet,
                                                                             uint32_t descriptorSetCount,
                                                                             const VkDescriptorSet* pDescriptorSets, uint32_t,
                                                                             const uint32_t*)
    {
        // Dynamic offsets are not modeled yet (the samples use none).
        const gb::object_id command_buffer = to_object_id(commandBuffer);
        std::vector<uint8_t> message(sizeof(gb::cmd_bind_descriptor_sets_request) +
                                     static_cast<size_t>(descriptorSetCount) * sizeof(gb::object_id));
        gb::cmd_bind_descriptor_sets_request header{};
        header.command_buffer = command_buffer;
        header.pipeline_layout = to_object_id(layout);
        header.first_set = firstSet;
        header.set_count = descriptorSetCount;
        std::memcpy(message.data(), &header, sizeof(header));
        for (uint32_t i = 0; i < descriptorSetCount; ++i)
        {
            const gb::object_id id = to_object_id(pDescriptorSets[i]);
            std::memcpy(message.data() + sizeof(header) + i * sizeof(id), &id, sizeof(id));
        }
        record_command(command_buffer, gb::command::cmd_bind_descriptor_sets, message.data(), message.size());
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdEndRenderPass(VkCommandBuffer commandBuffer)
    {
        gb::cmd_end_render_pass_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        record_command(request.command_buffer, gb::command::cmd_end_render_pass, &request, sizeof(request));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdPushConstants(VkCommandBuffer commandBuffer, VkPipelineLayout layout,
                                                                        VkShaderStageFlags stageFlags, uint32_t offset, uint32_t size,
                                                                        const void* pValues)
    {
        std::vector<uint8_t> message(sizeof(gb::cmd_push_constants_request) + size);
        gb::cmd_push_constants_request header{};
        header.command_buffer = to_object_id(commandBuffer);
        header.pipeline_layout = to_object_id(layout);
        header.stage_flags = stageFlags;
        header.offset = offset;
        header.size = size;
        std::memcpy(message.data(), &header, sizeof(header));
        if (size > 0 && pValues)
        {
            std::memcpy(message.data() + sizeof(header), pValues, size);
        }
        record_command(header.command_buffer, gb::command::cmd_push_constants, message.data(), message.size());
    }

    __declspec(dllexport) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice, const char* pName);

    __declspec(dllexport) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance, const char* pName)
    {
        if (!pName)
        {
            return nullptr;
        }

        struct entry
        {
            const char* name;
            PFN_vkVoidFunction func;
        };

        static const entry table[] = {
            {.name = "vkGetInstanceProcAddr", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetInstanceProcAddr)},
            {.name = "vkGetDeviceProcAddr", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceProcAddr)},
            {.name = "vkCreateInstance", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCreateInstance)},
            {.name = "vkDestroyInstance", .func = reinterpret_cast<PFN_vkVoidFunction>(vkDestroyInstance)},
            {.name = "vkEnumerateInstanceVersion", .func = reinterpret_cast<PFN_vkVoidFunction>(vkEnumerateInstanceVersion)},
            {.name = "vkEnumerateInstanceLayerProperties",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkEnumerateInstanceLayerProperties)},
            {.name = "vkEnumerateInstanceExtensionProperties",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkEnumerateInstanceExtensionProperties)},
            {.name = "vkEnumeratePhysicalDevices", .func = reinterpret_cast<PFN_vkVoidFunction>(vkEnumeratePhysicalDevices)},
            {.name = "vkEnumerateDeviceExtensionProperties",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkEnumerateDeviceExtensionProperties)},
            {.name = "vkGetPhysicalDeviceProperties", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceProperties)},
            {.name = "vkGetPhysicalDeviceProperties2", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceProperties2)},
            {.name = "vkGetPhysicalDeviceFeatures", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceFeatures)},
            {.name = "vkGetPhysicalDeviceFeatures2", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceFeatures2)},
            {.name = "vkGetPhysicalDeviceFormatProperties",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceFormatProperties)},
            {.name = "vkGetPhysicalDeviceFormatProperties2",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceFormatProperties2)},
            {.name = "vkGetPhysicalDeviceImageFormatProperties",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceImageFormatProperties)},
            {.name = "vkGetPhysicalDeviceImageFormatProperties2",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceImageFormatProperties2)},
            {.name = "vkGetPhysicalDeviceQueueFamilyProperties",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceQueueFamilyProperties)},
            {.name = "vkGetPhysicalDeviceQueueFamilyProperties2",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceQueueFamilyProperties2)},
            {.name = "vkGetPhysicalDeviceMemoryProperties2",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceMemoryProperties2)},
            {.name = "vkGetPhysicalDeviceSurfaceSupportKHR",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceSurfaceSupportKHR)},
            {.name = "vkGetPhysicalDeviceSurfaceFormatsKHR",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceSurfaceFormatsKHR)},
            {.name = "vkGetPhysicalDeviceSurfacePresentModesKHR",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceSurfacePresentModesKHR)},
            {.name = "vkGetPhysicalDeviceWin32PresentationSupportKHR",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceWin32PresentationSupportKHR)},
            {.name = "vkGetPhysicalDeviceExternalSemaphoreProperties",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceExternalSemaphoreProperties)},
            {.name = "vkGetPhysicalDeviceSparseImageFormatProperties",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceSparseImageFormatProperties)},
            {.name = "vkGetPhysicalDeviceSparseImageFormatProperties2",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceSparseImageFormatProperties2)},
            {.name = "vkGetPhysicalDeviceSurfaceCapabilities2KHR",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceSurfaceCapabilities2KHR)},
            {.name = "vkGetPhysicalDeviceSurfaceFormats2KHR",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceSurfaceFormats2KHR)},
            {.name = "vkGetPhysicalDeviceSurfacePresentModes2EXT",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceSurfacePresentModes2EXT)},
            {.name = "vkCmdBeginDebugUtilsLabelEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdBeginDebugUtilsLabelEXT)},
            {.name = "vkCmdEndDebugUtilsLabelEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdEndDebugUtilsLabelEXT)},
            {.name = "vkCmdInsertDebugUtilsLabelEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdInsertDebugUtilsLabelEXT)},
            {.name = "vkCreateDebugUtilsMessengerEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCreateDebugUtilsMessengerEXT)},
            {.name = "vkDestroyDebugUtilsMessengerEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkDestroyDebugUtilsMessengerEXT)},
            {.name = "vkSubmitDebugUtilsMessageEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkSubmitDebugUtilsMessageEXT)},
            {.name = "vkReleaseSwapchainImagesEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkReleaseSwapchainImagesEXT)},
            {.name = "vkCreateDevice", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCreateDevice)},
            {.name = "vkDestroyDevice", .func = reinterpret_cast<PFN_vkVoidFunction>(vkDestroyDevice)},
            {.name = "vkGetDeviceQueue", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceQueue)},
            {.name = "vkCreateCommandPool", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCreateCommandPool)},
            {.name = "vkDestroyCommandPool", .func = reinterpret_cast<PFN_vkVoidFunction>(vkDestroyCommandPool)},
            {.name = "vkAllocateCommandBuffers", .func = reinterpret_cast<PFN_vkVoidFunction>(vkAllocateCommandBuffers)},
            {.name = "vkFreeCommandBuffers", .func = reinterpret_cast<PFN_vkVoidFunction>(vkFreeCommandBuffers)},
            {.name = "vkBeginCommandBuffer", .func = reinterpret_cast<PFN_vkVoidFunction>(vkBeginCommandBuffer)},
            {.name = "vkEndCommandBuffer", .func = reinterpret_cast<PFN_vkVoidFunction>(vkEndCommandBuffer)},
            {.name = "vkCreateFence", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCreateFence)},
            {.name = "vkDestroyFence", .func = reinterpret_cast<PFN_vkVoidFunction>(vkDestroyFence)},
            {.name = "vkResetFences", .func = reinterpret_cast<PFN_vkVoidFunction>(vkResetFences)},
            {.name = "vkGetFenceStatus", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetFenceStatus)},
            {.name = "vkQueueSubmit", .func = reinterpret_cast<PFN_vkVoidFunction>(vkQueueSubmit)},
            {.name = "vkWaitForFences", .func = reinterpret_cast<PFN_vkVoidFunction>(vkWaitForFences)},
            {.name = "vkGetPhysicalDeviceMemoryProperties",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceMemoryProperties)},
            {.name = "vkAllocateMemory", .func = reinterpret_cast<PFN_vkVoidFunction>(vkAllocateMemory)},
            {.name = "vkFreeMemory", .func = reinterpret_cast<PFN_vkVoidFunction>(vkFreeMemory)},
            {.name = "vkMapMemory", .func = reinterpret_cast<PFN_vkVoidFunction>(vkMapMemory)},
            {.name = "vkUnmapMemory", .func = reinterpret_cast<PFN_vkVoidFunction>(vkUnmapMemory)},
            {.name = "vkCreateBuffer", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCreateBuffer)},
            {.name = "vkDestroyBuffer", .func = reinterpret_cast<PFN_vkVoidFunction>(vkDestroyBuffer)},
            {.name = "vkGetBufferMemoryRequirements", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetBufferMemoryRequirements)},
            {.name = "vkBindBufferMemory", .func = reinterpret_cast<PFN_vkVoidFunction>(vkBindBufferMemory)},
            {.name = "vkCmdFillBuffer", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdFillBuffer)},
            {.name = "vkCreateImage", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCreateImage)},
            {.name = "vkDestroyImage", .func = reinterpret_cast<PFN_vkVoidFunction>(vkDestroyImage)},
            {.name = "vkGetImageMemoryRequirements", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetImageMemoryRequirements)},
            {.name = "vkBindImageMemory", .func = reinterpret_cast<PFN_vkVoidFunction>(vkBindImageMemory)},
            {.name = "vkCmdPipelineBarrier", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdPipelineBarrier)},
            {.name = "vkCmdClearColorImage", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdClearColorImage)},
            {.name = "vkCmdCopyImageToBuffer", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdCopyImageToBuffer)},
            {.name = "vkCmdCopyBufferToImage", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdCopyBufferToImage)},
            {.name = "vkCreateSampler", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCreateSampler)},
            {.name = "vkDestroySampler", .func = reinterpret_cast<PFN_vkVoidFunction>(vkDestroySampler)},
            {.name = "vkCreateWin32SurfaceKHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCreateWin32SurfaceKHR)},
            {.name = "vkDestroySurfaceKHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkDestroySurfaceKHR)},
            {.name = "vkGetPhysicalDeviceSurfaceCapabilitiesKHR",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)},
            {.name = "vkCreateSwapchainKHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCreateSwapchainKHR)},
            {.name = "vkDestroySwapchainKHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkDestroySwapchainKHR)},
            {.name = "vkGetSwapchainImagesKHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetSwapchainImagesKHR)},
            {.name = "vkAcquireNextImageKHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkAcquireNextImageKHR)},
            {.name = "vkQueuePresentKHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkQueuePresentKHR)},
            {.name = "vkCreateShaderModule", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCreateShaderModule)},
            {.name = "vkDestroyShaderModule", .func = reinterpret_cast<PFN_vkVoidFunction>(vkDestroyShaderModule)},
            {.name = "vkCreateImageView", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCreateImageView)},
            {.name = "vkDestroyImageView", .func = reinterpret_cast<PFN_vkVoidFunction>(vkDestroyImageView)},
            {.name = "vkCreateRenderPass", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCreateRenderPass)},
            {.name = "vkDestroyRenderPass", .func = reinterpret_cast<PFN_vkVoidFunction>(vkDestroyRenderPass)},
            {.name = "vkCreateFramebuffer", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCreateFramebuffer)},
            {.name = "vkDestroyFramebuffer", .func = reinterpret_cast<PFN_vkVoidFunction>(vkDestroyFramebuffer)},
            {.name = "vkCreatePipelineLayout", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCreatePipelineLayout)},
            {.name = "vkDestroyPipelineLayout", .func = reinterpret_cast<PFN_vkVoidFunction>(vkDestroyPipelineLayout)},
            {.name = "vkCreateDescriptorSetLayout", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCreateDescriptorSetLayout)},
            {.name = "vkDestroyDescriptorSetLayout", .func = reinterpret_cast<PFN_vkVoidFunction>(vkDestroyDescriptorSetLayout)},
            {.name = "vkCreateDescriptorPool", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCreateDescriptorPool)},
            {.name = "vkDestroyDescriptorPool", .func = reinterpret_cast<PFN_vkVoidFunction>(vkDestroyDescriptorPool)},
            {.name = "vkAllocateDescriptorSets", .func = reinterpret_cast<PFN_vkVoidFunction>(vkAllocateDescriptorSets)},
            {.name = "vkUpdateDescriptorSets", .func = reinterpret_cast<PFN_vkVoidFunction>(vkUpdateDescriptorSets)},
            {.name = "vkCmdBindDescriptorSets", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdBindDescriptorSets)},
            {.name = "vkCreateGraphicsPipelines", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCreateGraphicsPipelines)},
            {.name = "vkDestroyPipeline", .func = reinterpret_cast<PFN_vkVoidFunction>(vkDestroyPipeline)},
            {.name = "vkCmdBeginRenderPass", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdBeginRenderPass)},
            {.name = "vkCmdBindPipeline", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdBindPipeline)},
            {.name = "vkCmdDraw", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdDraw)},
            {.name = "vkCmdBindVertexBuffers", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdBindVertexBuffers)},
            {.name = "vkCmdBindIndexBuffer", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdBindIndexBuffer)},
            {.name = "vkCmdDrawIndexed", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdDrawIndexed)},
            {.name = "vkCmdEndRenderPass", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdEndRenderPass)},
            {.name = "vkCmdPushConstants", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdPushConstants)},
        };

        for (const auto& e : table)
        {
            if (std::strcmp(e.name, pName) == 0)
            {
                return e.func;
            }
        }

        // Not implemented: return null per the Vulkan contract (callers use null to detect absent
        // optional functions/extensions) but log the name so missing entry points stay visible.
        std::array<char, 256> message{};
        std::snprintf(message.data(), message.size(), "vulkan-shim: no implementation for Vulkan function: %s (returning null)\n", pName);
        shim_log(message.data());
        return nullptr;
    }

    // Device-level entry points resolve through the same table for this minimal shim.
    __declspec(dllexport) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice, const char* pName)
    {
        return vkGetInstanceProcAddr(VK_NULL_HANDLE, pName);
    }

    // Some loaders / probes bootstrap through the ICD-style export instead.
    __declspec(dllexport) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char* pName)
    {
        return vkGetInstanceProcAddr(instance, pName);
    }
}
