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
#include <mutex>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
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

    // Flushes the coalesced descriptor-set updates (see vkUpdateDescriptorSets); defined below.
    void flush_descriptor_updates();

    bool bridge_call(uint32_t code, const void* in, DWORD in_len, void* out, DWORD out_len)
    {
        // Every other bridge call may make the host observe descriptor state (record, submit, ...), so drain
        // pending updates first to keep host state identical to the un-batched path.
        if (code != gb::ioctl_update_descriptor_sets_batch)
        {
            flush_descriptor_updates();
        }

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

    // Memory objects the bridge aliased straight into the guest address space (no staging copy). These are
    // not in g_mapped_ranges; vkUnmapMemory tears them down via the bridge instead of uploading.
    std::unordered_set<gb::object_id> g_direct_mapped;

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

    // Pending coalesced vkUpdateDescriptorSets blobs (the hottest bridge call - DXVK updates per draw).
    // Unlike the per-command-buffer streams (each synchronised to one thread by Vulkan), this global is
    // touched by every DXVK thread, and the preemptive time-slice can interrupt a thread mid-append, so the
    // mutex makes append and flush-swap atomic.
    std::vector<uint8_t> g_pending_descriptor_updates;
    std::mutex g_pending_descriptor_updates_mutex;

    void flush_descriptor_updates()
    {
        // Hold the lock across the IOCTL so concurrent flushes stay ordered: releasing it after the swap lets a
        // preempting thread append + flush and get its batch applied on the host before this (earlier) one. The
        // batch IOCTL never re-enters flush_descriptor_updates (bridge_call skips the flush for it) and runs
        // synchronously, so the lock cannot self-deadlock; a preempted appender just waits for this flush.
        std::lock_guard<std::mutex> lock(g_pending_descriptor_updates_mutex);
        if (g_pending_descriptor_updates.empty())
        {
            return;
        }

        std::vector<uint8_t> batch;
        batch.swap(g_pending_descriptor_updates);

        gb::result_response response{};
        bridge_call(gb::ioctl_update_descriptor_sets_batch, batch.data(), static_cast<DWORD>(batch.size()), &response, sizeof(response));
    }

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
                                      static_cast<size_t>(request.max_count) * sizeof(gb::queue_family_properties));
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
            reinterpret_cast<const gb::queue_family_properties*>(buffer.data() + sizeof(gb::get_queue_family_properties_response));
        for (uint32_t i = 0; i < written; ++i)
        {
            pProperties[i] = {.queueFlags = families[i].queue_flags,
                              .queueCount = families[i].queue_count,
                              .timestampValidBits = families[i].timestamp_valid_bits,
                              .minImageTransferGranularity = {.width = families[i].min_image_transfer_granularity_width,
                                                              .height = families[i].min_image_transfer_granularity_height,
                                                              .depth = families[i].min_image_transfer_granularity_depth}};
        }
        *pCount = written;
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice physicalDevice,
                                                                        const VkDeviceCreateInfo* pCreateInfo, const VkAllocationCallbacks*,
                                                                        VkDevice* pDevice)
    {
        gb::create_device_request request{};
        request.physical_device = to_object_id(physicalDevice);

        // Marshal every requested queue family (DXVK asks for a graphics queue plus a separate
        // transfer/compute family); the host must create a queue for each so later vkGetDeviceQueue
        // calls for those families succeed.
        std::vector<gb::device_queue_create_entry> queue_entries;
        if (pCreateInfo && pCreateInfo->queueCreateInfoCount > 0 && pCreateInfo->pQueueCreateInfos)
        {
            queue_entries.reserve(pCreateInfo->queueCreateInfoCount);
            for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; ++i)
            {
                queue_entries.push_back({.queue_family_index = pCreateInfo->pQueueCreateInfos[i].queueFamilyIndex,
                                         .queue_count = pCreateInfo->pQueueCreateInfos[i].queueCount});
            }
        }
        else
        {
            queue_entries.push_back({.queue_family_index = 0, .queue_count = 1});
        }
        request.queue_create_count = static_cast<uint32_t>(queue_entries.size());

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

        const size_t queue_bytes = queue_entries.size() * sizeof(gb::device_queue_create_entry);
        std::vector<std::byte> in(sizeof(request) + queue_bytes + extension_blob.size() + feature_blob.size());
        size_t offset = 0;
        std::memcpy(in.data() + offset, &request, sizeof(request));
        offset += sizeof(request);
        std::memcpy(in.data() + offset, queue_entries.data(), queue_bytes);
        offset += queue_bytes;
        if (!extension_blob.empty())
        {
            std::memcpy(in.data() + offset, extension_blob.data(), extension_blob.size());
            offset += extension_blob.size();
        }
        if (!feature_blob.empty())
        {
            std::memcpy(in.data() + offset, feature_blob.data(), feature_blob.size());
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
            request.level = static_cast<uint32_t>(pAllocateInfo->level);

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

        // A secondary command buffer carries inheritance; for dynamic rendering it has a chained
        // VkCommandBufferInheritanceRenderingInfo describing the attachment formats it renders to.
        const VkCommandBufferInheritanceRenderingInfo* rendering = nullptr;
        if (pBeginInfo && pBeginInfo->pInheritanceInfo)
        {
            request.is_secondary = 1;
            for (const auto* next = static_cast<const VkBaseInStructure*>(pBeginInfo->pInheritanceInfo->pNext); next; next = next->pNext)
            {
                if (next->sType == VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO)
                {
                    rendering = reinterpret_cast<const VkCommandBufferInheritanceRenderingInfo*>(next);
                    break;
                }
            }
        }

        std::vector<uint8_t> message(sizeof(request));
        if (rendering)
        {
            request.inherit_view_mask = rendering->viewMask;
            request.inherit_color_count = rendering->colorAttachmentCount;
            request.inherit_depth_format = static_cast<uint32_t>(rendering->depthAttachmentFormat);
            request.inherit_stencil_format = static_cast<uint32_t>(rendering->stencilAttachmentFormat);
            request.inherit_rasterization_samples = static_cast<uint32_t>(rendering->rasterizationSamples);
            request.inherit_rendering_flags = static_cast<uint32_t>(rendering->flags);
            message.resize(sizeof(request) + static_cast<size_t>(rendering->colorAttachmentCount) * sizeof(uint32_t));
            std::memcpy(message.data(), &request, sizeof(request));
            for (uint32_t i = 0; i < rendering->colorAttachmentCount; ++i)
            {
                const auto format = static_cast<uint32_t>(rendering->pColorAttachmentFormats[i]);
                std::memcpy(message.data() + sizeof(request) + i * sizeof(uint32_t), &format, sizeof(format));
            }
        }
        else
        {
            std::memcpy(message.data(), &request, sizeof(request));
        }

        g_command_streams[request.command_buffer].clear();
        record_command(request.command_buffer, gb::command::begin_command_buffer, message.data(), message.size());
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdExecuteCommands(VkCommandBuffer commandBuffer, uint32_t commandBufferCount,
                                                                          const VkCommandBuffer* pCommandBuffers)
    {
        const gb::object_id command_buffer = to_object_id(commandBuffer);
        std::vector<uint8_t> message(sizeof(gb::cmd_execute_commands_request) +
                                     static_cast<size_t>(commandBufferCount) * sizeof(gb::object_id));
        gb::cmd_execute_commands_request header{};
        header.command_buffer = command_buffer;
        header.count = commandBufferCount;
        std::memcpy(message.data(), &header, sizeof(header));
        for (uint32_t i = 0; i < commandBufferCount; ++i)
        {
            const gb::object_id id = to_object_id(pCommandBuffers[i]);
            std::memcpy(message.data() + sizeof(header) + i * sizeof(id), &id, sizeof(id));
        }
        record_command(command_buffer, gb::command::cmd_execute_commands, message.data(), message.size());
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

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandPool(VkDevice device, VkCommandPool commandPool,
                                                                            VkCommandPoolResetFlags flags)
    {
        gb::reset_command_pool_request request{};
        request.device = to_object_id(device);
        request.command_pool = to_object_id(commandPool);
        request.flags = flags;

        gb::result_response response{};
        if (!bridge_call(gb::ioctl_reset_command_pool, &request, sizeof(request), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        return static_cast<VkResult>(response.vk_result);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandBuffer(VkCommandBuffer commandBuffer,
                                                                              VkCommandBufferResetFlags flags)
    {
        // Drop any half-recorded local stream; recording only reaches the bridge at vkEndCommandBuffer.
        g_command_streams.erase(to_object_id(commandBuffer));

        gb::reset_command_buffer_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        request.flags = flags;

        gb::result_response response{};
        if (!bridge_call(gb::ioctl_reset_command_buffer, &request, sizeof(request), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        return static_cast<VkResult>(response.vk_result);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkQueueWaitIdle(VkQueue queue)
    {
        gb::queue_wait_idle_request request{};
        request.queue = to_object_id(queue);

        gb::result_response response{};
        if (!bridge_call(gb::ioctl_queue_wait_idle, &request, sizeof(request), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        return static_cast<VkResult>(response.vk_result);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkDeviceWaitIdle(VkDevice device)
    {
        gb::device_wait_idle_request request{};
        request.device = to_object_id(device);

        gb::result_response response{};
        if (!bridge_call(gb::ioctl_device_wait_idle, &request, sizeof(request), &response, sizeof(response)))
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

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkCreateEvent(VkDevice device, const VkEventCreateInfo* pCreateInfo,
                                                                       const VkAllocationCallbacks*, VkEvent* pEvent)
    {
        gb::create_event_request request{};
        request.device = to_object_id(device);
        request.flags = pCreateInfo ? pCreateInfo->flags : 0;

        gb::create_event_response response{};
        if (!bridge_call(gb::ioctl_create_event, &request, sizeof(request), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (response.vk_result != VK_SUCCESS)
        {
            return static_cast<VkResult>(response.vk_result);
        }

        *pEvent = to_handle<VkEvent>(response.event);
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkDestroyEvent(VkDevice device, VkEvent event, const VkAllocationCallbacks*)
    {
        gb::destroy_event_request request{};
        request.device = to_object_id(device);
        request.event = to_object_id(event);
        bridge_call(gb::ioctl_destroy_event, &request, sizeof(request), nullptr, 0);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkGetEventStatus(VkDevice, VkEvent event)
    {
        gb::get_event_status_request request{};
        request.event = to_object_id(event);

        gb::result_response response{};
        if (!bridge_call(gb::ioctl_get_event_status, &request, sizeof(request), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        return static_cast<VkResult>(response.vk_result);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkSetEvent(VkDevice device, VkEvent event)
    {
        gb::event_op_request request{};
        request.device = to_object_id(device);
        request.event = to_object_id(event);

        gb::result_response response{};
        if (!bridge_call(gb::ioctl_set_event, &request, sizeof(request), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        return static_cast<VkResult>(response.vk_result);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkResetEvent(VkDevice device, VkEvent event)
    {
        gb::event_op_request request{};
        request.device = to_object_id(device);
        request.event = to_object_id(event);

        gb::result_response response{};
        if (!bridge_call(gb::ioctl_reset_event, &request, sizeof(request), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        return static_cast<VkResult>(response.vk_result);
    }

    // GPU-side event commands are not forwarded to the host command buffer: the bridge's submission model
    // is effectively synchronous, so DXVK's vkGetEventStatus always reports the event as set (see the host).
    // These recorders are no-ops; they exist so DXVK's device function table is non-null and does not call
    // through a null pointer when it records them.
    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetEvent(VkCommandBuffer, VkEvent, VkPipelineStageFlags)
    {
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdResetEvent(VkCommandBuffer, VkEvent, VkPipelineStageFlags)
    {
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdWaitEvents(VkCommandBuffer, uint32_t, const VkEvent*, VkPipelineStageFlags,
                                                                     VkPipelineStageFlags, uint32_t, const VkMemoryBarrier*, uint32_t,
                                                                     const VkBufferMemoryBarrier*, uint32_t, const VkImageMemoryBarrier*)
    {
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetEvent2(VkCommandBuffer, VkEvent, const VkDependencyInfo*)
    {
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdResetEvent2(VkCommandBuffer, VkEvent, VkPipelineStageFlags2)
    {
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdWaitEvents2(VkCommandBuffer, uint32_t, const VkEvent*, const VkDependencyInfo*)
    {
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkCreateSemaphore(VkDevice device, const VkSemaphoreCreateInfo* pCreateInfo,
                                                                           const VkAllocationCallbacks*, VkSemaphore* pSemaphore)
    {
        gb::create_semaphore_request request{};
        request.device = to_object_id(device);
        request.flags = pCreateInfo ? pCreateInfo->flags : 0;
        request.semaphore_type = VK_SEMAPHORE_TYPE_BINARY;
        request.initial_value = 0;

        // DXVK creates timeline semaphores via a VkSemaphoreTypeCreateInfo on pNext; forward the type and
        // initial value so the host creates a real timeline semaphore (otherwise counter/signal/wait fail).
        if (pCreateInfo)
        {
            for (const auto* next = static_cast<const VkBaseInStructure*>(pCreateInfo->pNext); next; next = next->pNext)
            {
                if (next->sType == VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO)
                {
                    const auto* type_info = reinterpret_cast<const VkSemaphoreTypeCreateInfo*>(next);
                    request.semaphore_type = static_cast<uint32_t>(type_info->semaphoreType);
                    request.initial_value = type_info->initialValue;
                    break;
                }
            }
        }

        gb::create_semaphore_response response{};
        if (!bridge_call(gb::ioctl_create_semaphore, &request, sizeof(request), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (response.vk_result != VK_SUCCESS)
        {
            return static_cast<VkResult>(response.vk_result);
        }

        *pSemaphore = to_handle<VkSemaphore>(response.semaphore);
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkGetSemaphoreCounterValue(VkDevice device, VkSemaphore semaphore,
                                                                                    uint64_t* pValue)
    {
        gb::get_semaphore_counter_value_request request{};
        request.device = to_object_id(device);
        request.semaphore = to_object_id(semaphore);

        gb::get_semaphore_counter_value_response response{};
        if (!bridge_call(gb::ioctl_get_semaphore_counter_value, &request, sizeof(request), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (pValue)
        {
            *pValue = response.value;
        }
        return static_cast<VkResult>(response.vk_result);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkSignalSemaphore(VkDevice device, const VkSemaphoreSignalInfo* pSignalInfo)
    {
        if (!pSignalInfo)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        gb::signal_semaphore_request request{};
        request.device = to_object_id(device);
        request.semaphore = to_object_id(pSignalInfo->semaphore);
        request.value = pSignalInfo->value;

        gb::result_response response{};
        if (!bridge_call(gb::ioctl_signal_semaphore, &request, sizeof(request), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        return static_cast<VkResult>(response.vk_result);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkWaitSemaphores(VkDevice device, const VkSemaphoreWaitInfo* pWaitInfo,
                                                                          uint64_t timeout)
    {
        if (!pWaitInfo)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const uint32_t count = pWaitInfo->semaphoreCount;
        std::vector<uint8_t> message(sizeof(gb::wait_semaphores_request) + static_cast<size_t>(count) * sizeof(gb::wait_semaphore_entry));
        auto* header = reinterpret_cast<gb::wait_semaphores_request*>(message.data());
        header->device = to_object_id(device);
        header->flags = pWaitInfo->flags;
        header->semaphore_count = count;
        header->timeout = timeout;
        auto* entries = reinterpret_cast<gb::wait_semaphore_entry*>(message.data() + sizeof(gb::wait_semaphores_request));
        for (uint32_t i = 0; i < count; ++i)
        {
            entries[i].semaphore = to_object_id(pWaitInfo->pSemaphores[i]);
            entries[i].value = pWaitInfo->pValues[i];
        }

        gb::result_response response{};
        if (!bridge_call(gb::ioctl_wait_semaphores, message.data(), static_cast<DWORD>(message.size()), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        return static_cast<VkResult>(response.vk_result);
    }

    __declspec(dllexport) VKAPI_ATTR VkDeviceAddress VKAPI_CALL vkGetBufferDeviceAddress(VkDevice device,
                                                                                         const VkBufferDeviceAddressInfo* pInfo)
    {
        if (!pInfo)
        {
            return 0;
        }
        gb::get_buffer_device_address_request request{};
        request.device = to_object_id(device);
        request.buffer = to_object_id(pInfo->buffer);

        gb::get_buffer_device_address_response response{};
        if (!bridge_call(gb::ioctl_get_buffer_device_address, &request, sizeof(request), &response, sizeof(response)))
        {
            return 0;
        }
        return response.address;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkDestroySemaphore(VkDevice device, VkSemaphore semaphore,
                                                                        const VkAllocationCallbacks*)
    {
        if (!semaphore)
        {
            return;
        }
        gb::destroy_semaphore_request request{};
        request.device = to_object_id(device);
        request.semaphore = to_object_id(semaphore);
        bridge_call(gb::ioctl_destroy_semaphore, &request, sizeof(request), nullptr, 0);
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

    // synchronization2 submit: DXVK uses this for queue submission. Marshal each VkSubmitInfo2 (wait
    // semaphores, command buffers, signal semaphores) and forward to the host's real vkQueueSubmit2,
    // which has the real timeline semaphores. The fence is attached to the final submission.
    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit2(VkQueue queue, uint32_t submitCount, const VkSubmitInfo2* pSubmits,
                                                                        VkFence fence)
    {
        if (submitCount == 0)
        {
            VkSubmitInfo2 empty{};
            empty.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
            pSubmits = &empty;
            submitCount = fence ? 1 : 0;
        }

        for (uint32_t s = 0; s < submitCount; ++s)
        {
            const VkSubmitInfo2& si = pSubmits[s];
            const uint32_t wait_count = si.waitSemaphoreInfoCount;
            const uint32_t cmd_count = si.commandBufferInfoCount;
            const uint32_t signal_count = si.signalSemaphoreInfoCount;

            std::vector<uint8_t> message(sizeof(gb::queue_submit2_request) +
                                         static_cast<size_t>(wait_count + signal_count) * sizeof(gb::submit2_semaphore_entry) +
                                         static_cast<size_t>(cmd_count) * sizeof(uint64_t));
            auto* header = reinterpret_cast<gb::queue_submit2_request*>(message.data());
            header->queue = to_object_id(queue);
            header->fence = (s + 1 == submitCount) ? to_object_id(fence) : gb::null_object;
            header->wait_count = wait_count;
            header->command_buffer_count = cmd_count;
            header->signal_count = signal_count;
            header->reserved = 0;

            size_t offset = sizeof(*header);
            auto* waits = reinterpret_cast<gb::submit2_semaphore_entry*>(message.data() + offset);
            for (uint32_t i = 0; i < wait_count; ++i)
            {
                waits[i] = {.semaphore = to_object_id(si.pWaitSemaphoreInfos[i].semaphore),
                            .value = si.pWaitSemaphoreInfos[i].value,
                            .stage_mask = si.pWaitSemaphoreInfos[i].stageMask};
            }
            offset += static_cast<size_t>(wait_count) * sizeof(gb::submit2_semaphore_entry);

            auto* cmds = reinterpret_cast<uint64_t*>(message.data() + offset);
            for (uint32_t i = 0; i < cmd_count; ++i)
            {
                cmds[i] = to_object_id(si.pCommandBufferInfos[i].commandBuffer);
            }
            offset += static_cast<size_t>(cmd_count) * sizeof(uint64_t);

            auto* signals = reinterpret_cast<gb::submit2_semaphore_entry*>(message.data() + offset);
            for (uint32_t i = 0; i < signal_count; ++i)
            {
                signals[i] = {.semaphore = to_object_id(si.pSignalSemaphoreInfos[i].semaphore),
                              .value = si.pSignalSemaphoreInfos[i].value,
                              .stage_mask = si.pSignalSemaphoreInfos[i].stageMask};
            }

            gb::result_response response{};
            if (!bridge_call(gb::ioctl_queue_submit2, message.data(), static_cast<DWORD>(message.size()), &response, sizeof(response)) ||
                response.vk_result != VK_SUCCESS)
            {
                return VK_ERROR_INITIALIZATION_FAILED;
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

        // Prefer aliasing the host mapping straight into the guest address space (coherent, no copy). The
        // bridge returns guest_address = 0 when that is not possible, in which case we fall back to staging.
        if (actual > 0)
        {
            gb::map_memory_direct_request request{};
            request.device = to_object_id(device);
            request.memory = mem_id;
            request.offset = offset;
            request.size = actual;

            gb::map_memory_direct_response response{};
            if (bridge_call(gb::ioctl_map_memory_direct, &request, sizeof(request), &response, sizeof(response)) &&
                response.vk_result == VK_SUCCESS && response.guest_address != 0)
            {
                g_direct_mapped.insert(mem_id);
                *ppData = reinterpret_cast<void*>(static_cast<uintptr_t>(response.guest_address));
                return VK_SUCCESS;
            }
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

        if (g_direct_mapped.erase(mem_id) != 0)
        {
            gb::unmap_memory_direct_request request{};
            request.device = to_object_id(device);
            request.memory = mem_id;
            bridge_call(gb::ioctl_unmap_memory_direct, &request, sizeof(request), nullptr, 0);
            return;
        }

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

    // The bridge models a map as a private staging copy synced by download (map) / upload (unmap). DXVK may
    // instead keep a persistent mapping and explicitly flush, so honor flush by re-uploading the staging
    // copy and invalidate by re-downloading it. (Host-coherent memory makes these no-ops on a real driver.)
    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkFlushMappedMemoryRanges(VkDevice device, uint32_t memoryRangeCount,
                                                                                   const VkMappedMemoryRange* pMemoryRanges)
    {
        for (uint32_t i = 0; i < memoryRangeCount; ++i)
        {
            const gb::object_id mem_id = to_object_id(pMemoryRanges[i].memory);
            if (g_direct_mapped.contains(mem_id))
            {
                gb::mapped_memory_range_request request{};
                request.device = to_object_id(device);
                request.memory = mem_id;
                request.offset = pMemoryRanges[i].offset;
                request.size = pMemoryRanges[i].size;

                gb::result_response response{};
                if (!bridge_call(gb::ioctl_flush_mapped_memory_direct, &request, sizeof(request), &response, sizeof(response)))
                {
                    return VK_ERROR_DEVICE_LOST;
                }
                if (response.vk_result != VK_SUCCESS)
                {
                    return static_cast<VkResult>(response.vk_result);
                }
                continue;
            }

            const auto it = g_mapped_ranges.find(mem_id);
            if (it == g_mapped_ranges.end() || it->second.staging.empty())
            {
                continue;
            }

            std::vector<uint8_t> message(sizeof(gb::upload_memory_request) + it->second.staging.size());
            gb::upload_memory_request header{};
            header.device = to_object_id(device);
            header.memory = mem_id;
            header.offset = it->second.offset;
            header.size = it->second.size;
            std::memcpy(message.data(), &header, sizeof(header));
            std::memcpy(message.data() + sizeof(header), it->second.staging.data(), it->second.staging.size());

            gb::result_response response{};
            bridge_call(gb::ioctl_upload_memory, message.data(), static_cast<DWORD>(message.size()), &response, sizeof(response));
        }
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkInvalidateMappedMemoryRanges(VkDevice device, uint32_t memoryRangeCount,
                                                                                        const VkMappedMemoryRange* pMemoryRanges)
    {
        for (uint32_t i = 0; i < memoryRangeCount; ++i)
        {
            const gb::object_id mem_id = to_object_id(pMemoryRanges[i].memory);
            if (g_direct_mapped.contains(mem_id))
            {
                gb::mapped_memory_range_request request{};
                request.device = to_object_id(device);
                request.memory = mem_id;
                request.offset = pMemoryRanges[i].offset;
                request.size = pMemoryRanges[i].size;

                gb::result_response response{};
                if (!bridge_call(gb::ioctl_invalidate_mapped_memory_direct, &request, sizeof(request), &response, sizeof(response)))
                {
                    return VK_ERROR_DEVICE_LOST;
                }
                if (response.vk_result != VK_SUCCESS)
                {
                    return static_cast<VkResult>(response.vk_result);
                }
                continue;
            }

            const auto it = g_mapped_ranges.find(mem_id);
            if (it == g_mapped_ranges.end() || it->second.staging.empty())
            {
                continue;
            }

            gb::download_memory_request request{};
            request.device = to_object_id(device);
            request.memory = mem_id;
            request.offset = it->second.offset;
            request.size = it->second.size;
            bridge_call(gb::ioctl_download_memory, &request, sizeof(request), it->second.staging.data(),
                        static_cast<DWORD>(it->second.staging.size()));
        }
        return VK_SUCCESS;
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

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkCreateBufferView(VkDevice device, const VkBufferViewCreateInfo* pCreateInfo,
                                                                            const VkAllocationCallbacks*, VkBufferView* pView)
    {
        gb::create_buffer_view_request request{};
        request.device = to_object_id(device);
        request.buffer = to_object_id(pCreateInfo->buffer);
        request.format = static_cast<uint32_t>(pCreateInfo->format);
        request.offset = pCreateInfo->offset;
        request.range = pCreateInfo->range;

        gb::object_response response{};
        if (!bridge_call(gb::ioctl_create_buffer_view, &request, sizeof(request), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (response.vk_result != VK_SUCCESS)
        {
            return static_cast<VkResult>(response.vk_result);
        }

        *pView = to_handle<VkBufferView>(response.object);
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkDestroyBufferView(VkDevice device, VkBufferView bufferView,
                                                                         const VkAllocationCallbacks*)
    {
        if (!bufferView)
        {
            return;
        }
        gb::device_child_request request{};
        request.device = to_object_id(device);
        request.object = to_object_id(bufferView);
        bridge_call(gb::ioctl_destroy_buffer_view, &request, sizeof(request), nullptr, 0);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkCreateQueryPool(VkDevice device, const VkQueryPoolCreateInfo* pCreateInfo,
                                                                           const VkAllocationCallbacks*, VkQueryPool* pQueryPool)
    {
        gb::create_query_pool_request request{};
        request.device = to_object_id(device);
        request.query_type = static_cast<uint32_t>(pCreateInfo->queryType);
        request.query_count = pCreateInfo->queryCount;
        request.pipeline_statistics = static_cast<uint32_t>(pCreateInfo->pipelineStatistics);

        gb::object_response response{};
        if (!bridge_call(gb::ioctl_create_query_pool, &request, sizeof(request), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (response.vk_result != VK_SUCCESS)
        {
            return static_cast<VkResult>(response.vk_result);
        }

        *pQueryPool = to_handle<VkQueryPool>(response.object);
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkDestroyQueryPool(VkDevice device, VkQueryPool queryPool,
                                                                        const VkAllocationCallbacks*)
    {
        if (!queryPool)
        {
            return;
        }
        gb::device_child_request request{};
        request.device = to_object_id(device);
        request.object = to_object_id(queryPool);
        bridge_call(gb::ioctl_destroy_query_pool, &request, sizeof(request), nullptr, 0);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkResetQueryPool(VkDevice device, VkQueryPool queryPool, uint32_t firstQuery,
                                                                      uint32_t queryCount)
    {
        gb::reset_query_pool_request request{};
        request.device = to_object_id(device);
        request.query_pool = to_object_id(queryPool);
        request.first_query = firstQuery;
        request.query_count = queryCount;

        gb::result_response response{};
        bridge_call(gb::ioctl_reset_query_pool, &request, sizeof(request), &response, sizeof(response));
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkGetQueryPoolResults(VkDevice device, VkQueryPool queryPool, uint32_t firstQuery,
                                                                               uint32_t queryCount, size_t dataSize, void* pData,
                                                                               VkDeviceSize stride, VkQueryResultFlags flags)
    {
        gb::get_query_pool_results_request request{};
        request.device = to_object_id(device);
        request.query_pool = to_object_id(queryPool);
        request.first_query = firstQuery;
        request.query_count = queryCount;
        request.data_size = static_cast<uint32_t>(dataSize);
        request.stride = static_cast<uint32_t>(stride);
        request.flags = static_cast<uint32_t>(flags);

        std::vector<uint8_t> out(sizeof(gb::get_query_pool_results_response) + dataSize);
        if (!bridge_call(gb::ioctl_get_query_pool_results, &request, sizeof(request), out.data(), static_cast<DWORD>(out.size())))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        gb::get_query_pool_results_response response{};
        std::memcpy(&response, out.data(), sizeof(response));
        if (pData && dataSize > 0 && response.data_size > 0)
        {
            std::memcpy(pData, out.data() + sizeof(response), std::min<size_t>(dataSize, response.data_size));
        }
        return static_cast<VkResult>(response.vk_result);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdResetQueryPool(VkCommandBuffer commandBuffer, VkQueryPool queryPool,
                                                                         uint32_t firstQuery, uint32_t queryCount)
    {
        gb::cmd_reset_query_pool_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        request.query_pool = to_object_id(queryPool);
        request.first_query = firstQuery;
        request.query_count = queryCount;
        record_command(request.command_buffer, gb::command::cmd_reset_query_pool, &request, sizeof(request));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdBeginQuery(VkCommandBuffer commandBuffer, VkQueryPool queryPool, uint32_t query,
                                                                     VkQueryControlFlags flags)
    {
        gb::cmd_begin_query_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        request.query_pool = to_object_id(queryPool);
        request.query = query;
        request.flags = static_cast<uint32_t>(flags);
        record_command(request.command_buffer, gb::command::cmd_begin_query, &request, sizeof(request));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdEndQuery(VkCommandBuffer commandBuffer, VkQueryPool queryPool, uint32_t query)
    {
        gb::cmd_end_query_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        request.query_pool = to_object_id(queryPool);
        request.query = query;
        record_command(request.command_buffer, gb::command::cmd_end_query, &request, sizeof(request));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdWriteTimestamp(VkCommandBuffer commandBuffer,
                                                                         VkPipelineStageFlagBits pipelineStage, VkQueryPool queryPool,
                                                                         uint32_t query)
    {
        gb::cmd_write_timestamp_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        request.query_pool = to_object_id(queryPool);
        request.query = query;
        request.pipeline_stage = static_cast<uint32_t>(pipelineStage);
        record_command(request.command_buffer, gb::command::cmd_write_timestamp, &request, sizeof(request));
    }

    // --- Extended-dynamic-state setters: record into the command stream, replayed on the host. ---

    static void record_set_dynamic_u32(VkCommandBuffer commandBuffer, gb::dynamic_state_u32 state, uint32_t value)
    {
        gb::cmd_set_dynamic_u32_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        request.state = static_cast<uint32_t>(state);
        request.value = value;
        record_command(request.command_buffer, gb::command::cmd_set_dynamic_u32, &request, sizeof(request));
    }

    static void record_set_stencil(VkCommandBuffer commandBuffer, gb::stencil_dynamic_state which, VkStencilFaceFlags faceMask,
                                   uint32_t value)
    {
        gb::cmd_set_stencil_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        request.which = static_cast<uint32_t>(which);
        request.face_mask = static_cast<uint32_t>(faceMask);
        request.value = value;
        record_command(request.command_buffer, gb::command::cmd_set_stencil, &request, sizeof(request));
    }

    static void record_set_viewport(VkCommandBuffer commandBuffer, uint32_t first, uint32_t count, const VkViewport* pViewports,
                                    bool with_count)
    {
        const gb::object_id command_buffer = to_object_id(commandBuffer);
        std::vector<uint8_t> message(sizeof(gb::cmd_set_viewport_request) + static_cast<size_t>(count) * sizeof(gb::viewport_entry));
        gb::cmd_set_viewport_request header{};
        header.command_buffer = command_buffer;
        header.first = first;
        header.count = count;
        header.with_count = with_count ? 1u : 0u;
        std::memcpy(message.data(), &header, sizeof(header));
        for (uint32_t i = 0; i < count; ++i)
        {
            const gb::viewport_entry entry{
                .x = pViewports[i].x,
                .y = pViewports[i].y,
                .width = pViewports[i].width,
                .height = pViewports[i].height,
                .min_depth = pViewports[i].minDepth,
                .max_depth = pViewports[i].maxDepth,
            };
            std::memcpy(message.data() + sizeof(header) + static_cast<size_t>(i) * sizeof(entry), &entry, sizeof(entry));
        }
        record_command(command_buffer, gb::command::cmd_set_viewport, message.data(), message.size());
    }

    static void record_set_scissor(VkCommandBuffer commandBuffer, uint32_t first, uint32_t count, const VkRect2D* pScissors,
                                   bool with_count)
    {
        const gb::object_id command_buffer = to_object_id(commandBuffer);
        std::vector<uint8_t> message(sizeof(gb::cmd_set_scissor_request) + static_cast<size_t>(count) * sizeof(gb::scissor_entry));
        gb::cmd_set_scissor_request header{};
        header.command_buffer = command_buffer;
        header.first = first;
        header.count = count;
        header.with_count = with_count ? 1u : 0u;
        std::memcpy(message.data(), &header, sizeof(header));
        for (uint32_t i = 0; i < count; ++i)
        {
            const gb::scissor_entry entry{
                .offset_x = pScissors[i].offset.x,
                .offset_y = pScissors[i].offset.y,
                .width = pScissors[i].extent.width,
                .height = pScissors[i].extent.height,
            };
            std::memcpy(message.data() + sizeof(header) + static_cast<size_t>(i) * sizeof(entry), &entry, sizeof(entry));
        }
        record_command(command_buffer, gb::command::cmd_set_scissor, message.data(), message.size());
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetViewport(VkCommandBuffer commandBuffer, uint32_t firstViewport,
                                                                      uint32_t viewportCount, const VkViewport* pViewports)
    {
        record_set_viewport(commandBuffer, firstViewport, viewportCount, pViewports, false);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetViewportWithCount(VkCommandBuffer commandBuffer, uint32_t viewportCount,
                                                                               const VkViewport* pViewports)
    {
        record_set_viewport(commandBuffer, 0, viewportCount, pViewports, true);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetScissor(VkCommandBuffer commandBuffer, uint32_t firstScissor,
                                                                     uint32_t scissorCount, const VkRect2D* pScissors)
    {
        record_set_scissor(commandBuffer, firstScissor, scissorCount, pScissors, false);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetScissorWithCount(VkCommandBuffer commandBuffer, uint32_t scissorCount,
                                                                              const VkRect2D* pScissors)
    {
        record_set_scissor(commandBuffer, 0, scissorCount, pScissors, true);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthBias(VkCommandBuffer commandBuffer, float depthBiasConstantFactor,
                                                                       float depthBiasClamp, float depthBiasSlopeFactor)
    {
        gb::cmd_set_depth_bias_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        request.constant_factor = depthBiasConstantFactor;
        request.clamp = depthBiasClamp;
        request.slope_factor = depthBiasSlopeFactor;
        record_command(request.command_buffer, gb::command::cmd_set_depth_bias, &request, sizeof(request));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetBlendConstants(VkCommandBuffer commandBuffer, const float blendConstants[4])
    {
        gb::cmd_set_blend_constants_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        request.constants[0] = blendConstants[0];
        request.constants[1] = blendConstants[1];
        request.constants[2] = blendConstants[2];
        request.constants[3] = blendConstants[3];
        record_command(request.command_buffer, gb::command::cmd_set_blend_constants, &request, sizeof(request));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthBounds(VkCommandBuffer commandBuffer, float minDepthBounds,
                                                                         float maxDepthBounds)
    {
        gb::cmd_set_depth_bounds_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        request.min_depth_bounds = minDepthBounds;
        request.max_depth_bounds = maxDepthBounds;
        record_command(request.command_buffer, gb::command::cmd_set_depth_bounds, &request, sizeof(request));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetLineWidth(VkCommandBuffer commandBuffer, float lineWidth)
    {
        gb::cmd_set_line_width_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        request.line_width = lineWidth;
        record_command(request.command_buffer, gb::command::cmd_set_line_width, &request, sizeof(request));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilCompareMask(VkCommandBuffer commandBuffer, VkStencilFaceFlags faceMask,
                                                                                uint32_t compareMask)
    {
        record_set_stencil(commandBuffer, gb::stencil_dynamic_state::compare_mask, faceMask, compareMask);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilWriteMask(VkCommandBuffer commandBuffer, VkStencilFaceFlags faceMask,
                                                                              uint32_t writeMask)
    {
        record_set_stencil(commandBuffer, gb::stencil_dynamic_state::write_mask, faceMask, writeMask);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilReference(VkCommandBuffer commandBuffer, VkStencilFaceFlags faceMask,
                                                                              uint32_t reference)
    {
        record_set_stencil(commandBuffer, gb::stencil_dynamic_state::reference, faceMask, reference);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilOp(VkCommandBuffer commandBuffer, VkStencilFaceFlags faceMask,
                                                                       VkStencilOp failOp, VkStencilOp passOp, VkStencilOp depthFailOp,
                                                                       VkCompareOp compareOp)
    {
        gb::cmd_set_stencil_op_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        request.face_mask = static_cast<uint32_t>(faceMask);
        request.fail_op = static_cast<uint32_t>(failOp);
        request.pass_op = static_cast<uint32_t>(passOp);
        request.depth_fail_op = static_cast<uint32_t>(depthFailOp);
        request.compare_op = static_cast<uint32_t>(compareOp);
        record_command(request.command_buffer, gb::command::cmd_set_stencil_op, &request, sizeof(request));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetCullMode(VkCommandBuffer commandBuffer, VkCullModeFlags cullMode)
    {
        record_set_dynamic_u32(commandBuffer, gb::dynamic_state_u32::cull_mode, static_cast<uint32_t>(cullMode));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetFrontFace(VkCommandBuffer commandBuffer, VkFrontFace frontFace)
    {
        record_set_dynamic_u32(commandBuffer, gb::dynamic_state_u32::front_face, static_cast<uint32_t>(frontFace));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetPrimitiveTopology(VkCommandBuffer commandBuffer,
                                                                               VkPrimitiveTopology primitiveTopology)
    {
        record_set_dynamic_u32(commandBuffer, gb::dynamic_state_u32::primitive_topology, static_cast<uint32_t>(primitiveTopology));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthTestEnable(VkCommandBuffer commandBuffer, VkBool32 depthTestEnable)
    {
        record_set_dynamic_u32(commandBuffer, gb::dynamic_state_u32::depth_test_enable, depthTestEnable);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthWriteEnable(VkCommandBuffer commandBuffer, VkBool32 depthWriteEnable)
    {
        record_set_dynamic_u32(commandBuffer, gb::dynamic_state_u32::depth_write_enable, depthWriteEnable);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthCompareOp(VkCommandBuffer commandBuffer, VkCompareOp depthCompareOp)
    {
        record_set_dynamic_u32(commandBuffer, gb::dynamic_state_u32::depth_compare_op, static_cast<uint32_t>(depthCompareOp));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthBoundsTestEnable(VkCommandBuffer commandBuffer,
                                                                                   VkBool32 depthBoundsTestEnable)
    {
        record_set_dynamic_u32(commandBuffer, gb::dynamic_state_u32::depth_bounds_test_enable, depthBoundsTestEnable);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthClipEnableEXT(VkCommandBuffer commandBuffer, VkBool32 depthClipEnable)
    {
        record_set_dynamic_u32(commandBuffer, gb::dynamic_state_u32::depth_clip_enable, depthClipEnable);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilTestEnable(VkCommandBuffer commandBuffer, VkBool32 stencilTestEnable)
    {
        record_set_dynamic_u32(commandBuffer, gb::dynamic_state_u32::stencil_test_enable, stencilTestEnable);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetRasterizerDiscardEnable(VkCommandBuffer commandBuffer,
                                                                                     VkBool32 rasterizerDiscardEnable)
    {
        record_set_dynamic_u32(commandBuffer, gb::dynamic_state_u32::rasterizer_discard_enable, rasterizerDiscardEnable);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthBiasEnable(VkCommandBuffer commandBuffer, VkBool32 depthBiasEnable)
    {
        record_set_dynamic_u32(commandBuffer, gb::dynamic_state_u32::depth_bias_enable, depthBiasEnable);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetPrimitiveRestartEnable(VkCommandBuffer commandBuffer,
                                                                                    VkBool32 primitiveRestartEnable)
    {
        record_set_dynamic_u32(commandBuffer, gb::dynamic_state_u32::primitive_restart_enable, primitiveRestartEnable);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdCopyBuffer(VkCommandBuffer commandBuffer, VkBuffer srcBuffer, VkBuffer dstBuffer,
                                                                     uint32_t regionCount, const VkBufferCopy* pRegions)
    {
        const gb::object_id command_buffer = to_object_id(commandBuffer);
        std::vector<uint8_t> message(sizeof(gb::cmd_copy_buffer_request) +
                                     static_cast<size_t>(regionCount) * sizeof(gb::buffer_copy_region));
        gb::cmd_copy_buffer_request header{};
        header.command_buffer = command_buffer;
        header.src_buffer = to_object_id(srcBuffer);
        header.dst_buffer = to_object_id(dstBuffer);
        header.region_count = regionCount;
        std::memcpy(message.data(), &header, sizeof(header));
        for (uint32_t i = 0; i < regionCount; ++i)
        {
            gb::buffer_copy_region region{};
            region.src_offset = pRegions[i].srcOffset;
            region.dst_offset = pRegions[i].dstOffset;
            region.size = pRegions[i].size;
            std::memcpy(message.data() + sizeof(header) + i * sizeof(region), &region, sizeof(region));
        }
        record_command(command_buffer, gb::command::cmd_copy_buffer, message.data(), message.size());
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdCopyBuffer2(VkCommandBuffer commandBuffer,
                                                                      const VkCopyBufferInfo2* pCopyBufferInfo)
    {
        const gb::object_id command_buffer = to_object_id(commandBuffer);
        const uint32_t regionCount = pCopyBufferInfo->regionCount;
        std::vector<uint8_t> message(sizeof(gb::cmd_copy_buffer_request) +
                                     static_cast<size_t>(regionCount) * sizeof(gb::buffer_copy_region));
        gb::cmd_copy_buffer_request header{};
        header.command_buffer = command_buffer;
        header.src_buffer = to_object_id(pCopyBufferInfo->srcBuffer);
        header.dst_buffer = to_object_id(pCopyBufferInfo->dstBuffer);
        header.region_count = regionCount;
        std::memcpy(message.data(), &header, sizeof(header));
        for (uint32_t i = 0; i < regionCount; ++i)
        {
            gb::buffer_copy_region region{};
            region.src_offset = pCopyBufferInfo->pRegions[i].srcOffset;
            region.dst_offset = pCopyBufferInfo->pRegions[i].dstOffset;
            region.size = pCopyBufferInfo->pRegions[i].size;
            std::memcpy(message.data() + sizeof(header) + i * sizeof(region), &region, sizeof(region));
        }
        record_command(command_buffer, gb::command::cmd_copy_buffer, message.data(), message.size());
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

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements2(VkDevice device,
                                                                                    const VkBufferMemoryRequirementsInfo2* pInfo,
                                                                                    VkMemoryRequirements2* pMemoryRequirements)
    {
        if (!pInfo || !pMemoryRequirements)
        {
            return;
        }
        vkGetBufferMemoryRequirements(device, pInfo->buffer, &pMemoryRequirements->memoryRequirements);
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
        request.samples = static_cast<uint32_t>(pCreateInfo->samples);
        request.image_type = static_cast<uint32_t>(pCreateInfo->imageType);
        request.depth = pCreateInfo->extent.depth;
        request.mip_levels = pCreateInfo->mipLevels;
        request.array_layers = pCreateInfo->arrayLayers;
        request.flags = pCreateInfo->flags;

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

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkGetImageSubresourceLayout(VkDevice device, VkImage image,
                                                                                 const VkImageSubresource* pSubresource,
                                                                                 VkSubresourceLayout* pLayout)
    {
        if (!pLayout)
        {
            return;
        }
        *pLayout = {};

        gb::get_image_subresource_layout_request request{};
        request.device = to_object_id(device);
        request.image = to_object_id(image);
        if (pSubresource)
        {
            request.aspect_mask = pSubresource->aspectMask;
            request.mip_level = pSubresource->mipLevel;
            request.array_layer = pSubresource->arrayLayer;
        }

        gb::get_image_subresource_layout_response response{};
        if (!bridge_call(gb::ioctl_get_image_subresource_layout, &request, sizeof(request), &response, sizeof(response)))
        {
            return;
        }

        pLayout->offset = response.offset;
        pLayout->size = response.size;
        pLayout->rowPitch = response.row_pitch;
        pLayout->arrayPitch = response.array_pitch;
        pLayout->depthPitch = response.depth_pitch;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkGetImageSubresourceLayout2KHR(VkDevice device, VkImage image,
                                                                                     const VkImageSubresource2KHR* pSubresource,
                                                                                     VkSubresourceLayout2KHR* pLayout)
    {
        if (!pSubresource || !pLayout)
        {
            return;
        }
        vkGetImageSubresourceLayout(device, image, &pSubresource->imageSubresource, &pLayout->subresourceLayout);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkGetImageMemoryRequirements2(VkDevice device,
                                                                                   const VkImageMemoryRequirementsInfo2* pInfo,
                                                                                   VkMemoryRequirements2* pMemoryRequirements)
    {
        if (!pInfo || !pMemoryRequirements)
        {
            return;
        }
        vkGetImageMemoryRequirements(device, pInfo->image, &pMemoryRequirements->memoryRequirements);
    }

    // Vulkan 1.3 (maintenance4) lets callers query memory requirements without a live object. The bridge
    // has no equivalent query, so probe a throwaway object created from the supplied create-info. DXVK's
    // memory allocator relies on these during device construction.
    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkGetDeviceBufferMemoryRequirements(VkDevice device,
                                                                                         const VkDeviceBufferMemoryRequirements* pInfo,
                                                                                         VkMemoryRequirements2* pMemoryRequirements)
    {
        if (!pInfo || !pInfo->pCreateInfo || !pMemoryRequirements)
        {
            return;
        }
        VkBuffer buffer = VK_NULL_HANDLE;
        if (vkCreateBuffer(device, pInfo->pCreateInfo, nullptr, &buffer) != VK_SUCCESS)
        {
            return;
        }
        vkGetBufferMemoryRequirements(device, buffer, &pMemoryRequirements->memoryRequirements);
        vkDestroyBuffer(device, buffer, nullptr);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkGetDeviceImageMemoryRequirements(VkDevice device,
                                                                                        const VkDeviceImageMemoryRequirements* pInfo,
                                                                                        VkMemoryRequirements2* pMemoryRequirements)
    {
        if (!pInfo || !pInfo->pCreateInfo || !pMemoryRequirements)
        {
            return;
        }
        VkImage image = VK_NULL_HANDLE;
        if (vkCreateImage(device, pInfo->pCreateInfo, nullptr, &image) != VK_SUCCESS)
        {
            return;
        }
        vkGetImageMemoryRequirements(device, image, &pMemoryRequirements->memoryRequirements);
        vkDestroyImage(device, image, nullptr);
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

    // DXVK binds memory through the *2 entry points on Vulkan 1.1+ devices; forward each bind info to the
    // existing single-bind bridge command.
    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory2(VkDevice device, uint32_t bindInfoCount,
                                                                             const VkBindBufferMemoryInfo* pBindInfos)
    {
        for (uint32_t i = 0; i < bindInfoCount; ++i)
        {
            const VkResult r = vkBindBufferMemory(device, pBindInfos[i].buffer, pBindInfos[i].memory, pBindInfos[i].memoryOffset);
            if (r != VK_SUCCESS)
            {
                return r;
            }
        }
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkBindImageMemory2(VkDevice device, uint32_t bindInfoCount,
                                                                            const VkBindImageMemoryInfo* pBindInfos)
    {
        for (uint32_t i = 0; i < bindInfoCount; ++i)
        {
            const VkResult r = vkBindImageMemory(device, pBindInfos[i].image, pBindInfos[i].memory, pBindInfos[i].memoryOffset);
            if (r != VK_SUCCESS)
            {
                return r;
            }
        }
        return VK_SUCCESS;
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

    // synchronization2 barrier: DXVK uses this exclusively. Lower its image barriers to the v1
    // cmd_pipeline_barrier records. The VkPipelineStageFlags2/VkAccessFlags2 (64-bit) don't map cleanly to
    // the v1 32-bit flags and a 0 stage mask is invalid in v1, so use ALL_COMMANDS stages (the bridge
    // executes submissions synchronously, so over-synchronizing is harmless). Layout transitions -- the
    // part DXVK actually relies on -- are preserved. Global/buffer memory barriers are no-ops here.
    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier2(VkCommandBuffer commandBuffer,
                                                                           const VkDependencyInfo* pDependencyInfo)
    {
        if (!pDependencyInfo)
        {
            return;
        }
        for (uint32_t i = 0; i < pDependencyInfo->imageMemoryBarrierCount; ++i)
        {
            const VkImageMemoryBarrier2& b = pDependencyInfo->pImageMemoryBarriers[i];
            gb::cmd_pipeline_barrier_request request{};
            request.command_buffer = to_object_id(commandBuffer);
            request.image = to_object_id(b.image);
            request.subresource = to_wire_range(b.subresourceRange);
            request.src_stage_mask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            request.dst_stage_mask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            request.src_access_mask = static_cast<uint32_t>(b.srcAccessMask);
            request.dst_access_mask = static_cast<uint32_t>(b.dstAccessMask);
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

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdClearDepthStencilImage(VkCommandBuffer commandBuffer, VkImage image,
                                                                                 VkImageLayout imageLayout,
                                                                                 const VkClearDepthStencilValue* pDepthStencil,
                                                                                 uint32_t rangeCount,
                                                                                 const VkImageSubresourceRange* pRanges)
    {
        for (uint32_t i = 0; i < rangeCount; ++i)
        {
            gb::cmd_clear_depth_stencil_image_request request{};
            request.command_buffer = to_object_id(commandBuffer);
            request.image = to_object_id(image);
            request.subresource = to_wire_range(pRanges[i]);
            request.image_layout = static_cast<uint32_t>(imageLayout);
            request.depth = pDepthStencil->depth;
            request.stencil = pDepthStencil->stencil;
            record_command(request.command_buffer, gb::command::cmd_clear_depth_stencil_image, &request, sizeof(request));
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
            request.buffer_offset = r.bufferOffset;
            request.image_layout = static_cast<uint32_t>(dstImageLayout);
            request.buffer_row_length = r.bufferRowLength;
            request.buffer_image_height = r.bufferImageHeight;
            request.image_offset_x = r.imageOffset.x;
            request.image_offset_y = r.imageOffset.y;
            request.image_offset_z = r.imageOffset.z;
            request.width = r.imageExtent.width;
            request.height = r.imageExtent.height;
            request.depth = r.imageExtent.depth;
            request.mip_level = r.imageSubresource.mipLevel;
            request.base_array_layer = r.imageSubresource.baseArrayLayer;
            request.layer_count = r.imageSubresource.layerCount;
            request.aspect_mask = r.imageSubresource.aspectMask;
            record_command(request.command_buffer, gb::command::cmd_copy_buffer_to_image, &request, sizeof(request));
        }
    }

    // DXVK on a Vulkan 1.3 device issues the KHR_copy_commands2 / core-1.3 variants. They carry the
    // same per-region data as the legacy entry points, so they reuse the existing copy commands.
    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdCopyBufferToImage2(VkCommandBuffer commandBuffer,
                                                                             const VkCopyBufferToImageInfo2* pCopyBufferToImageInfo)
    {
        for (uint32_t i = 0; i < pCopyBufferToImageInfo->regionCount; ++i)
        {
            const VkBufferImageCopy2& r = pCopyBufferToImageInfo->pRegions[i];
            gb::cmd_copy_buffer_to_image_request request{};
            request.command_buffer = to_object_id(commandBuffer);
            request.buffer = to_object_id(pCopyBufferToImageInfo->srcBuffer);
            request.image = to_object_id(pCopyBufferToImageInfo->dstImage);
            request.buffer_offset = r.bufferOffset;
            request.image_layout = static_cast<uint32_t>(pCopyBufferToImageInfo->dstImageLayout);
            request.buffer_row_length = r.bufferRowLength;
            request.buffer_image_height = r.bufferImageHeight;
            request.image_offset_x = r.imageOffset.x;
            request.image_offset_y = r.imageOffset.y;
            request.image_offset_z = r.imageOffset.z;
            request.width = r.imageExtent.width;
            request.height = r.imageExtent.height;
            request.depth = r.imageExtent.depth;
            request.mip_level = r.imageSubresource.mipLevel;
            request.base_array_layer = r.imageSubresource.baseArrayLayer;
            request.layer_count = r.imageSubresource.layerCount;
            request.aspect_mask = r.imageSubresource.aspectMask;
            record_command(request.command_buffer, gb::command::cmd_copy_buffer_to_image, &request, sizeof(request));
        }
    }

    // Resolves a multisampled source image into a single-sample destination. DXVK uses this to resolve an
    // MSAA backbuffer before presenting it. Remoted assuming full image, mip 0 / layer 0, offset 0.
    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdResolveImage(VkCommandBuffer commandBuffer, VkImage srcImage,
                                                                       VkImageLayout srcImageLayout, VkImage dstImage,
                                                                       VkImageLayout dstImageLayout, uint32_t regionCount,
                                                                       const VkImageResolve* pRegions)
    {
        for (uint32_t i = 0; i < regionCount; ++i)
        {
            const VkImageResolve& r = pRegions[i];
            gb::cmd_resolve_image_request request{};
            request.command_buffer = to_object_id(commandBuffer);
            request.src_image = to_object_id(srcImage);
            request.dst_image = to_object_id(dstImage);
            request.src_layout = static_cast<uint32_t>(srcImageLayout);
            request.dst_layout = static_cast<uint32_t>(dstImageLayout);
            request.width = r.extent.width;
            request.height = r.extent.height;
            request.aspect_mask = r.srcSubresource.aspectMask;
            record_command(request.command_buffer, gb::command::cmd_resolve_image, &request, sizeof(request));
        }
    }

    // Updates a buffer region inline. DXVK uses this for small dynamic uploads (constant/uniform data); the
    // bytes trail the request header in the recorded stream.
    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdUpdateBuffer(VkCommandBuffer commandBuffer, VkBuffer dstBuffer,
                                                                       VkDeviceSize dstOffset, VkDeviceSize dataSize, const void* pData)
    {
        const auto size = static_cast<uint32_t>(dataSize);
        std::vector<uint8_t> message(sizeof(gb::cmd_update_buffer_request) + size);
        gb::cmd_update_buffer_request header{};
        header.command_buffer = to_object_id(commandBuffer);
        header.buffer = to_object_id(dstBuffer);
        header.offset = dstOffset;
        header.size = size;
        std::memcpy(message.data(), &header, sizeof(header));
        if (size > 0 && pData)
        {
            std::memcpy(message.data() + sizeof(header), pData, size);
        }
        record_command(header.command_buffer, gb::command::cmd_update_buffer, message.data(), message.size());
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdResolveImage2(VkCommandBuffer commandBuffer,
                                                                        const VkResolveImageInfo2* pResolveImageInfo)
    {
        for (uint32_t i = 0; i < pResolveImageInfo->regionCount; ++i)
        {
            const VkImageResolve2& r = pResolveImageInfo->pRegions[i];
            gb::cmd_resolve_image_request request{};
            request.command_buffer = to_object_id(commandBuffer);
            request.src_image = to_object_id(pResolveImageInfo->srcImage);
            request.dst_image = to_object_id(pResolveImageInfo->dstImage);
            request.src_layout = static_cast<uint32_t>(pResolveImageInfo->srcImageLayout);
            request.dst_layout = static_cast<uint32_t>(pResolveImageInfo->dstImageLayout);
            request.width = r.extent.width;
            request.height = r.extent.height;
            request.aspect_mask = r.srcSubresource.aspectMask;
            record_command(request.command_buffer, gb::command::cmd_resolve_image, &request, sizeof(request));
        }
    }

    // Image-to-image copy. DXVK issues this for texture-to-texture transfers during scene rendering. One
    // VkImageCopy region per recorded command (the shim loops over regions).
    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdCopyImage(VkCommandBuffer commandBuffer, VkImage srcImage,
                                                                    VkImageLayout srcImageLayout, VkImage dstImage,
                                                                    VkImageLayout dstImageLayout, uint32_t regionCount,
                                                                    const VkImageCopy* pRegions)
    {
        for (uint32_t i = 0; i < regionCount; ++i)
        {
            const VkImageCopy& r = pRegions[i];
            gb::cmd_copy_image_request request{};
            request.command_buffer = to_object_id(commandBuffer);
            request.src_image = to_object_id(srcImage);
            request.dst_image = to_object_id(dstImage);
            request.src_layout = static_cast<uint32_t>(srcImageLayout);
            request.dst_layout = static_cast<uint32_t>(dstImageLayout);
            request.src_aspect_mask = r.srcSubresource.aspectMask;
            request.src_mip_level = r.srcSubresource.mipLevel;
            request.src_base_array_layer = r.srcSubresource.baseArrayLayer;
            request.src_layer_count = r.srcSubresource.layerCount;
            request.src_offset_x = r.srcOffset.x;
            request.src_offset_y = r.srcOffset.y;
            request.src_offset_z = r.srcOffset.z;
            request.dst_aspect_mask = r.dstSubresource.aspectMask;
            request.dst_mip_level = r.dstSubresource.mipLevel;
            request.dst_base_array_layer = r.dstSubresource.baseArrayLayer;
            request.dst_layer_count = r.dstSubresource.layerCount;
            request.dst_offset_x = r.dstOffset.x;
            request.dst_offset_y = r.dstOffset.y;
            request.dst_offset_z = r.dstOffset.z;
            request.width = r.extent.width;
            request.height = r.extent.height;
            request.depth = r.extent.depth;
            record_command(request.command_buffer, gb::command::cmd_copy_image, &request, sizeof(request));
        }
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdCopyImage2(VkCommandBuffer commandBuffer, const VkCopyImageInfo2* pCopyImageInfo)
    {
        for (uint32_t i = 0; i < pCopyImageInfo->regionCount; ++i)
        {
            const VkImageCopy2& r = pCopyImageInfo->pRegions[i];
            gb::cmd_copy_image_request request{};
            request.command_buffer = to_object_id(commandBuffer);
            request.src_image = to_object_id(pCopyImageInfo->srcImage);
            request.dst_image = to_object_id(pCopyImageInfo->dstImage);
            request.src_layout = static_cast<uint32_t>(pCopyImageInfo->srcImageLayout);
            request.dst_layout = static_cast<uint32_t>(pCopyImageInfo->dstImageLayout);
            request.src_aspect_mask = r.srcSubresource.aspectMask;
            request.src_mip_level = r.srcSubresource.mipLevel;
            request.src_base_array_layer = r.srcSubresource.baseArrayLayer;
            request.src_layer_count = r.srcSubresource.layerCount;
            request.src_offset_x = r.srcOffset.x;
            request.src_offset_y = r.srcOffset.y;
            request.src_offset_z = r.srcOffset.z;
            request.dst_aspect_mask = r.dstSubresource.aspectMask;
            request.dst_mip_level = r.dstSubresource.mipLevel;
            request.dst_base_array_layer = r.dstSubresource.baseArrayLayer;
            request.dst_layer_count = r.dstSubresource.layerCount;
            request.dst_offset_x = r.dstOffset.x;
            request.dst_offset_y = r.dstOffset.y;
            request.dst_offset_z = r.dstOffset.z;
            request.width = r.extent.width;
            request.height = r.extent.height;
            request.depth = r.extent.depth;
            record_command(request.command_buffer, gb::command::cmd_copy_image, &request, sizeof(request));
        }
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdBlitImage2(VkCommandBuffer commandBuffer, const VkBlitImageInfo2* pBlitImageInfo)
    {
        if (!pBlitImageInfo)
        {
            return;
        }

        for (uint32_t i = 0; i < pBlitImageInfo->regionCount; ++i)
        {
            const VkImageBlit2& r = pBlitImageInfo->pRegions[i];
            gb::cmd_blit_image_request request{};
            request.command_buffer = to_object_id(commandBuffer);
            request.src_image = to_object_id(pBlitImageInfo->srcImage);
            request.dst_image = to_object_id(pBlitImageInfo->dstImage);
            request.src_layout = static_cast<uint32_t>(pBlitImageInfo->srcImageLayout);
            request.dst_layout = static_cast<uint32_t>(pBlitImageInfo->dstImageLayout);
            request.src_aspect_mask = r.srcSubresource.aspectMask;
            request.src_mip_level = r.srcSubresource.mipLevel;
            request.src_base_array_layer = r.srcSubresource.baseArrayLayer;
            request.src_layer_count = r.srcSubresource.layerCount;
            request.src_offset_x0 = r.srcOffsets[0].x;
            request.src_offset_y0 = r.srcOffsets[0].y;
            request.src_offset_z0 = r.srcOffsets[0].z;
            request.src_offset_x1 = r.srcOffsets[1].x;
            request.src_offset_y1 = r.srcOffsets[1].y;
            request.src_offset_z1 = r.srcOffsets[1].z;
            request.dst_aspect_mask = r.dstSubresource.aspectMask;
            request.dst_mip_level = r.dstSubresource.mipLevel;
            request.dst_base_array_layer = r.dstSubresource.baseArrayLayer;
            request.dst_layer_count = r.dstSubresource.layerCount;
            request.dst_offset_x0 = r.dstOffsets[0].x;
            request.dst_offset_y0 = r.dstOffsets[0].y;
            request.dst_offset_z0 = r.dstOffsets[0].z;
            request.dst_offset_x1 = r.dstOffsets[1].x;
            request.dst_offset_y1 = r.dstOffsets[1].y;
            request.dst_offset_z1 = r.dstOffsets[1].z;
            request.filter = static_cast<uint32_t>(pBlitImageInfo->filter);
            record_command(request.command_buffer, gb::command::cmd_blit_image, &request, sizeof(request));
        }
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdBlitImage2KHR(VkCommandBuffer commandBuffer,
                                                                        const VkBlitImageInfo2* pBlitImageInfo)
    {
        vkCmdBlitImage2(commandBuffer, pBlitImageInfo);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdCopyImageToBuffer2(VkCommandBuffer commandBuffer,
                                                                             const VkCopyImageToBufferInfo2* pCopyImageToBufferInfo)
    {
        for (uint32_t i = 0; i < pCopyImageToBufferInfo->regionCount; ++i)
        {
            const VkBufferImageCopy2& r = pCopyImageToBufferInfo->pRegions[i];
            gb::cmd_copy_image_to_buffer_request request{};
            request.command_buffer = to_object_id(commandBuffer);
            request.image = to_object_id(pCopyImageToBufferInfo->srcImage);
            request.buffer = to_object_id(pCopyImageToBufferInfo->dstBuffer);
            request.image_layout = static_cast<uint32_t>(pCopyImageToBufferInfo->srcImageLayout);
            request.width = r.imageExtent.width;
            request.height = r.imageExtent.height;
            request.aspect_mask = r.imageSubresource.aspectMask;
            record_command(request.command_buffer, gb::command::cmd_copy_image_to_buffer, &request, sizeof(request));
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
        if (!pProperties)
        {
            return;
        }

        // Base VkPhysicalDeviceProperties goes through the already-remoted base query.
        vkGetPhysicalDeviceProperties(physicalDevice, &pProperties->properties);

        // Remote the chained property structs (e.g. VkPhysicalDeviceRobustness2PropertiesEXT, whose
        // alignment fields DXVK divides by). Same chain convention as vkGetPhysicalDeviceFeatures2.
        struct dest
        {
            uint8_t* body;
            uint32_t body_size;
        };
        std::vector<dest> dests;
        std::vector<gb::feature_chain_record> records;
        for (auto* next = static_cast<VkBaseOutStructure*>(pProperties->pNext); next; next = next->pNext)
        {
            const auto body_size = static_cast<uint32_t>(gb::property_body_size(next->sType));
            if (body_size == 0)
            {
                continue; // struct the host does not know; left as the caller initialized it
            }
            dests.push_back({.body = reinterpret_cast<uint8_t*>(next) + gb::feature_chain_header_size, .body_size = body_size});
            records.push_back({.s_type = static_cast<uint32_t>(next->sType), .body_size = body_size});
        }

        if (records.empty())
        {
            return;
        }

        std::vector<std::byte> in(sizeof(gb::get_physical_device_properties2_request) + records.size() * sizeof(gb::feature_chain_record));
        auto* request = reinterpret_cast<gb::get_physical_device_properties2_request*>(in.data());
        request->physical_device = to_object_id(physicalDevice);
        request->struct_count = static_cast<uint32_t>(records.size());
        request->reserved = 0;
        std::memcpy(in.data() + sizeof(*request), records.data(), records.size() * sizeof(gb::feature_chain_record));

        size_t out_capacity = sizeof(gb::get_physical_device_properties2_response);
        for (const auto& d : dests)
        {
            out_capacity += sizeof(gb::feature_chain_record) + d.body_size;
        }
        std::vector<std::byte> out(out_capacity);

        if (!bridge_call(gb::ioctl_get_physical_device_properties2, in.data(), static_cast<DWORD>(in.size()), out.data(),
                         static_cast<DWORD>(out.size())))
        {
            return;
        }

        const auto* response = reinterpret_cast<const gb::get_physical_device_properties2_response*>(out.data());
        if (response->vk_result != VK_SUCCESS)
        {
            return;
        }

        size_t offset = sizeof(gb::get_physical_device_properties2_response);
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

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL
    vkGetPhysicalDeviceMemoryProperties2(VkPhysicalDevice physicalDevice, VkPhysicalDeviceMemoryProperties2* pMemoryProperties)
    {
        if (!pMemoryProperties)
        {
            return;
        }

        vkGetPhysicalDeviceMemoryProperties(physicalDevice, &pMemoryProperties->memoryProperties);

        // Fill any chained VK_EXT_memory_budget request: DXGI's QueryVideoMemoryInfo reports 0 available
        // VRAM (and apps may abort) unless the host's real per-heap budget/usage is forwarded.
        for (auto* next = static_cast<VkBaseOutStructure*>(pMemoryProperties->pNext); next != nullptr; next = next->pNext)
        {
            if (next->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT)
            {
                continue;
            }

            auto* budget = reinterpret_cast<VkPhysicalDeviceMemoryBudgetPropertiesEXT*>(next);

            gb::get_physical_device_memory_budget_request request{};
            request.physical_device = to_object_id(physicalDevice);

            gb::get_physical_device_memory_budget_response response{};
            if (bridge_call(gb::ioctl_get_physical_device_memory_budget, &request, sizeof(request), &response, sizeof(response)) &&
                response.vk_result == VK_SUCCESS)
            {
                const uint32_t count = std::min<uint32_t>(response.heap_count, VK_MAX_MEMORY_HEAPS);
                for (uint32_t i = 0; i < count; ++i)
                {
                    budget->heapBudget[i] = response.heap_budget[i];
                    budget->heapUsage[i] = response.heap_usage[i];
                }
            }
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

        gb::get_queue_family_properties_request request{};
        request.physical_device = to_object_id(physicalDevice);
        request.max_count = *pCount;
        for (uint32_t i = 0; i < *pCount && request.query_ownership_transfer == 0; ++i)
        {
            for (const auto* next = reinterpret_cast<const VkBaseOutStructure*>(pProperties[i].pNext); next; next = next->pNext)
            {
                if (next->sType == VK_STRUCTURE_TYPE_QUEUE_FAMILY_OWNERSHIP_TRANSFER_PROPERTIES_KHR)
                {
                    request.query_ownership_transfer = 1;
                    break;
                }
            }
        }
        std::vector<std::byte> buffer(sizeof(gb::get_queue_family_properties_response) +
                                      static_cast<size_t>(request.max_count) * sizeof(gb::queue_family_properties));
        if (!bridge_call(gb::ioctl_get_queue_family_properties, &request, sizeof(request), buffer.data(),
                         static_cast<DWORD>(buffer.size())))
        {
            *pCount = 0;
            return;
        }

        const auto* response = reinterpret_cast<const gb::get_queue_family_properties_response*>(buffer.data());
        const uint32_t count = std::min(response->count, *pCount);
        const auto* families =
            reinterpret_cast<const gb::queue_family_properties*>(buffer.data() + sizeof(gb::get_queue_family_properties_response));
        for (uint32_t i = 0; i < count; ++i)
        {
            pProperties[i].queueFamilyProperties = {
                .queueFlags = families[i].queue_flags,
                .queueCount = families[i].queue_count,
                .timestampValidBits = families[i].timestamp_valid_bits,
                .minImageTransferGranularity = {.width = families[i].min_image_transfer_granularity_width,
                                                .height = families[i].min_image_transfer_granularity_height,
                                                .depth = families[i].min_image_transfer_granularity_depth}};
            for (auto* next = reinterpret_cast<VkBaseOutStructure*>(pProperties[i].pNext); next; next = next->pNext)
            {
                if (next->sType == VK_STRUCTURE_TYPE_QUEUE_FAMILY_OWNERSHIP_TRANSFER_PROPERTIES_KHR)
                {
                    reinterpret_cast<VkQueueFamilyOwnershipTransferPropertiesKHR*>(next)->optimalImageTransferToQueueFamilies =
                        families[i].optimal_image_transfer_to_queue_families;
                }
            }
        }
        *pCount = count;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice physicalDevice, VkFormat format,
                                                                                         VkFormatProperties* pFormatProperties)
    {
        if (!pFormatProperties)
        {
            return;
        }
        *pFormatProperties = {};

        // Remote the device's real per-format support. The previous format-agnostic stub reported every
        // format as supporting everything, which made DXVK's format table accept invalid mappings (e.g. a
        // compressed format as a render target) and then reject resources that should succeed.
        gb::get_physical_device_format_properties_request request{};
        request.physical_device = to_object_id(physicalDevice);
        request.format = static_cast<uint32_t>(format);

        gb::get_physical_device_format_properties_response response{};
        if (!bridge_call(gb::ioctl_get_physical_device_format_properties, &request, sizeof(request), &response, sizeof(response)))
        {
            return;
        }
        pFormatProperties->linearTilingFeatures = response.linear_tiling_features;
        pFormatProperties->optimalTilingFeatures = response.optimal_tiling_features;
        pFormatProperties->bufferFeatures = response.buffer_features;
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

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceImageFormatProperties(
        VkPhysicalDevice physicalDevice, VkFormat format, VkImageType type, VkImageTiling tiling, VkImageUsageFlags usage,
        VkImageCreateFlags flags, VkImageFormatProperties* pImageFormatProperties)
    {
        if (!pImageFormatProperties)
        {
            return VK_ERROR_FORMAT_NOT_SUPPORTED;
        }

        gb::get_physical_device_image_format_properties_request request{};
        request.physical_device = to_object_id(physicalDevice);
        request.format = static_cast<uint32_t>(format);
        request.type = static_cast<uint32_t>(type);
        request.tiling = static_cast<uint32_t>(tiling);
        request.usage = usage;
        request.flags = flags;

        gb::get_physical_device_image_format_properties_response response{};
        if (!bridge_call(gb::ioctl_get_physical_device_image_format_properties, &request, sizeof(request), &response, sizeof(response)))
        {
            return VK_ERROR_FORMAT_NOT_SUPPORTED;
        }
        if (response.vk_result != VK_SUCCESS)
        {
            return static_cast<VkResult>(response.vk_result);
        }

        *pImageFormatProperties = {};
        pImageFormatProperties->maxExtent = {
            .width = response.max_extent_width, .height = response.max_extent_height, .depth = response.max_extent_depth};
        pImageFormatProperties->maxMipLevels = response.max_mip_levels;
        pImageFormatProperties->maxArrayLayers = response.max_array_layers;
        pImageFormatProperties->sampleCounts = response.sample_counts;
        pImageFormatProperties->maxResourceSize = response.max_resource_size;
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

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImageKHR(VkDevice, VkSwapchainKHR swapchain, uint64_t,
                                                                               VkSemaphore semaphore, VkFence fence, uint32_t* pImageIndex)
    {
        // The image is always immediately available, but the caller makes its render submit wait on the
        // semaphore (and may wait on the fence), so they must still be signalled by the bridge.
        gb::acquire_next_image_request request{};
        request.swapchain = to_object_id(swapchain);
        request.semaphore = to_object_id(semaphore);
        request.fence = to_object_id(fence);

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
        request.view_type = static_cast<uint32_t>(pCreateInfo->viewType);
        request.base_mip_level = pCreateInfo->subresourceRange.baseMipLevel;
        request.level_count = pCreateInfo->subresourceRange.levelCount;
        request.base_array_layer = pCreateInfo->subresourceRange.baseArrayLayer;
        request.layer_count = pCreateInfo->subresourceRange.layerCount;
        request.swizzle_r = static_cast<uint32_t>(pCreateInfo->components.r);
        request.swizzle_g = static_cast<uint32_t>(pCreateInfo->components.g);
        request.swizzle_b = static_cast<uint32_t>(pCreateInfo->components.b);
        request.swizzle_a = static_cast<uint32_t>(pCreateInfo->components.a);

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

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkResetDescriptorPool(VkDevice device, VkDescriptorPool descriptorPool,
                                                                               VkDescriptorPoolResetFlags flags)
    {
        gb::reset_descriptor_pool_request request{};
        request.device = to_object_id(device);
        request.descriptor_pool = to_object_id(descriptorPool);
        request.flags = flags;

        gb::result_response response{};
        if (!bridge_call(gb::ioctl_reset_descriptor_pool, &request, sizeof(request), &response, sizeof(response)) ||
            response.vk_result != VK_SUCCESS)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        return VK_SUCCESS;
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

        // Append to the pending batch instead of issuing an IOCTL now (drained before the next bridge call).
        const auto* header_bytes = reinterpret_cast<const uint8_t*>(&header);
        const auto* write_bytes = reinterpret_cast<const uint8_t*>(writes.data());
        std::lock_guard<std::mutex> lock(g_pending_descriptor_updates_mutex);
        g_pending_descriptor_updates.insert(g_pending_descriptor_updates.end(), header_bytes, header_bytes + sizeof(header));
        if (!writes.empty())
        {
            g_pending_descriptor_updates.insert(g_pending_descriptor_updates.end(), write_bytes,
                                                write_bytes + writes.size() * sizeof(gb::descriptor_write));
        }
    }

    // Descriptor update templates are lowered entirely inside the shim: the template definition is kept
    // host-side (guest-side, in the shim) and vkUpdateDescriptorSetWithTemplate gathers the strided
    // caller data into ordinary VkWriteDescriptorSet records, reusing vkUpdateDescriptorSets. This avoids
    // a dedicated bridge command for an otherwise pure convenience API.
    namespace
    {
        struct shim_descriptor_update_template
        {
            std::vector<VkDescriptorUpdateTemplateEntry> entries;
        };

        bool is_image_descriptor(VkDescriptorType type)
        {
            return type == VK_DESCRIPTOR_TYPE_SAMPLER || type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
                   type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
                   type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
        }

        bool is_texel_buffer_descriptor(VkDescriptorType type)
        {
            return type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
        }
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL
    vkCreateDescriptorUpdateTemplate(VkDevice, const VkDescriptorUpdateTemplateCreateInfo* pCreateInfo, const VkAllocationCallbacks*,
                                     VkDescriptorUpdateTemplate* pDescriptorUpdateTemplate)
    {
        if (!pCreateInfo || !pDescriptorUpdateTemplate)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        auto* tmpl = new (std::nothrow) shim_descriptor_update_template{};
        if (!tmpl)
        {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        tmpl->entries.assign(pCreateInfo->pDescriptorUpdateEntries,
                             pCreateInfo->pDescriptorUpdateEntries + pCreateInfo->descriptorUpdateEntryCount);
        *pDescriptorUpdateTemplate = reinterpret_cast<VkDescriptorUpdateTemplate>(tmpl);
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorUpdateTemplate(VkDevice,
                                                                                       VkDescriptorUpdateTemplate descriptorUpdateTemplate,
                                                                                       const VkAllocationCallbacks*)
    {
        delete reinterpret_cast<shim_descriptor_update_template*>(descriptorUpdateTemplate);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkUpdateDescriptorSetWithTemplate(VkDevice device, VkDescriptorSet descriptorSet,
                                                                                       VkDescriptorUpdateTemplate descriptorUpdateTemplate,
                                                                                       const void* pData)
    {
        const auto* tmpl = reinterpret_cast<const shim_descriptor_update_template*>(descriptorUpdateTemplate);
        if (!tmpl || !pData)
        {
            return;
        }

        std::vector<VkWriteDescriptorSet> writes;
        std::vector<std::vector<VkDescriptorImageInfo>> image_infos;
        std::vector<std::vector<VkDescriptorBufferInfo>> buffer_infos;
        std::vector<std::vector<VkBufferView>> texel_views;
        writes.reserve(tmpl->entries.size());
        image_infos.reserve(tmpl->entries.size());
        buffer_infos.reserve(tmpl->entries.size());
        texel_views.reserve(tmpl->entries.size());

        const auto* base = static_cast<const uint8_t*>(pData);
        for (const auto& entry : tmpl->entries)
        {
            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = descriptorSet;
            write.dstBinding = entry.dstBinding;
            write.dstArrayElement = entry.dstArrayElement;
            write.descriptorCount = entry.descriptorCount;
            write.descriptorType = entry.descriptorType;

            if (is_image_descriptor(entry.descriptorType))
            {
                auto& arr = image_infos.emplace_back();
                arr.reserve(entry.descriptorCount);
                for (uint32_t e = 0; e < entry.descriptorCount; ++e)
                {
                    arr.push_back(*reinterpret_cast<const VkDescriptorImageInfo*>(base + entry.offset + e * entry.stride));
                }
                write.pImageInfo = arr.data();
            }
            else if (is_texel_buffer_descriptor(entry.descriptorType))
            {
                auto& arr = texel_views.emplace_back();
                arr.reserve(entry.descriptorCount);
                for (uint32_t e = 0; e < entry.descriptorCount; ++e)
                {
                    arr.push_back(*reinterpret_cast<const VkBufferView*>(base + entry.offset + e * entry.stride));
                }
                write.pTexelBufferView = arr.data();
            }
            else
            {
                auto& arr = buffer_infos.emplace_back();
                arr.reserve(entry.descriptorCount);
                for (uint32_t e = 0; e < entry.descriptorCount; ++e)
                {
                    arr.push_back(*reinterpret_cast<const VkDescriptorBufferInfo*>(base + entry.offset + e * entry.stride));
                }
                write.pBufferInfo = arr.data();
            }

            writes.push_back(write);
        }

        vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
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
        request.mipmap_mode = static_cast<uint32_t>(pCreateInfo->mipmapMode);
        request.compare_enable = pCreateInfo->compareEnable;
        request.compare_op = static_cast<uint32_t>(pCreateInfo->compareOp);
        request.anisotropy_enable = pCreateInfo->anisotropyEnable;
        request.border_color = static_cast<uint32_t>(pCreateInfo->borderColor);
        request.mip_lod_bias = pCreateInfo->mipLodBias;
        request.max_anisotropy = pCreateInfo->maxAnisotropy;
        request.min_lod = pCreateInfo->minLod;
        request.max_lod = pCreateInfo->maxLod;

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

    namespace
    {
        // DXVK (VK_KHR_maintenance5) chains the SPIR-V inline through a VkShaderModuleCreateInfo on the
        // stage's pNext instead of passing a VkShaderModule. The bridge models explicit shader modules,
        // so materialize a temporary one from the inline code. Returns the module to use (and sets
        // `owned` when the caller must destroy it after pipeline creation).
        VkShaderModule resolve_stage_module(VkDevice device, const VkPipelineShaderStageCreateInfo& stage, bool& owned)
        {
            owned = false;
            if (stage.module != VK_NULL_HANDLE)
            {
                return stage.module;
            }
            for (const auto* next = static_cast<const VkBaseInStructure*>(stage.pNext); next != nullptr; next = next->pNext)
            {
                if (next->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO)
                {
                    const auto* info = reinterpret_cast<const VkShaderModuleCreateInfo*>(next);
                    VkShaderModule module = VK_NULL_HANDLE;
                    if (vkCreateShaderModule(device, info, nullptr, &module) == VK_SUCCESS)
                    {
                        owned = true;
                        return module;
                    }
                    break;
                }
            }
            return VK_NULL_HANDLE;
        }
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
            const VkSpecializationInfo* vs_spec = nullptr;
            const VkSpecializationInfo* fs_spec = nullptr;
            VkShaderModule owned_modules[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
            uint32_t owned_count = 0;
            for (uint32_t s = 0; s < ci.stageCount; ++s)
            {
                bool owned = false;
                const VkShaderModule module = resolve_stage_module(device, ci.pStages[s], owned);
                if (owned && owned_count < 2)
                {
                    owned_modules[owned_count++] = module;
                }
                if (ci.pStages[s].stage == VK_SHADER_STAGE_VERTEX_BIT)
                {
                    vertex_shader = to_object_id(module);
                    vs_spec = ci.pStages[s].pSpecializationInfo;
                }
                else if (ci.pStages[s].stage == VK_SHADER_STAGE_FRAGMENT_BIT)
                {
                    fragment_shader = to_object_id(module);
                    fs_spec = ci.pStages[s].pSpecializationInfo;
                }
            }
            const uint32_t vs_spec_entries = (vs_spec && vs_spec->pMapEntries) ? vs_spec->mapEntryCount : 0u;
            const uint32_t vs_spec_bytes = (vs_spec && vs_spec->pData) ? static_cast<uint32_t>(vs_spec->dataSize) : 0u;
            const uint32_t fs_spec_entries = (fs_spec && fs_spec->pMapEntries) ? fs_spec->mapEntryCount : 0u;
            const uint32_t fs_spec_bytes = (fs_spec && fs_spec->pData) ? static_cast<uint32_t>(fs_spec->dataSize) : 0u;

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
            request.rasterization_samples = ci.pMultisampleState ? static_cast<uint32_t>(ci.pMultisampleState->rasterizationSamples) : 1u;

            request.primitive_topology = static_cast<uint32_t>(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);

            request.primitive_restart_enable = 0;

            if (ci.pInputAssemblyState)
            {
                request.primitive_topology = static_cast<uint32_t>(ci.pInputAssemblyState->topology);

                request.primitive_restart_enable = ci.pInputAssemblyState->primitiveRestartEnable ? 1u : 0u;
            }

            // Forward the dynamic-state list verbatim. DXVK marks vertex-binding stride, cull, topology,
            // depth/stencil etc. dynamic and sets them via vkCmdSet*/vkCmdBindVertexBuffers2; if the host
            // pipeline does not also declare them dynamic it bakes (often wrong) defaults instead.
            const uint32_t dynamic_state_count = ci.pDynamicState ? ci.pDynamicState->dynamicStateCount : 0;
            request.dynamic_state_count = dynamic_state_count;
            request.vs_spec_entry_count = vs_spec_entries;
            request.vs_spec_data_size = vs_spec_bytes;
            request.fs_spec_entry_count = fs_spec_entries;
            request.fs_spec_data_size = fs_spec_bytes;

            // Per-attachment blend state. DXVK bakes D3D9 alpha blending statically into pColorBlendState; without
            // forwarding it the host defaults to blend-disabled and transparent geometry renders fully opaque.
            if (ci.pColorBlendState && ci.pColorBlendState->pAttachments)
            {
                request.blend_attachment_count = ci.pColorBlendState->attachmentCount < gb::max_color_attachments
                                                     ? ci.pColorBlendState->attachmentCount
                                                     : gb::max_color_attachments;
                for (uint32_t a = 0; a < request.blend_attachment_count; ++a)
                {
                    const VkPipelineColorBlendAttachmentState& src = ci.pColorBlendState->pAttachments[a];
                    gb::pipeline_blend_attachment& dst = request.blend_attachments[a];
                    dst.blend_enable = src.blendEnable ? 1u : 0u;
                    dst.src_color_blend_factor = static_cast<uint32_t>(src.srcColorBlendFactor);
                    dst.dst_color_blend_factor = static_cast<uint32_t>(src.dstColorBlendFactor);
                    dst.color_blend_op = static_cast<uint32_t>(src.colorBlendOp);
                    dst.src_alpha_blend_factor = static_cast<uint32_t>(src.srcAlphaBlendFactor);
                    dst.dst_alpha_blend_factor = static_cast<uint32_t>(src.dstAlphaBlendFactor);
                    dst.alpha_blend_op = static_cast<uint32_t>(src.alphaBlendOp);
                    dst.color_write_mask = static_cast<uint32_t>(src.colorWriteMask);
                }
            }

            // DXVK 2.x builds pipelines with VK_KHR_dynamic_rendering: renderPass is VK_NULL_HANDLE and the
            // attachment formats live in a VkPipelineRenderingCreateInfo on the pNext chain. Forward those so
            // the host can rebuild that info instead of failing for the missing render pass.
            for (const auto* base = static_cast<const VkBaseInStructure*>(ci.pNext); base != nullptr; base = base->pNext)
            {
                if (base->sType != VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO)
                {
                    continue;
                }

                const auto* rendering = reinterpret_cast<const VkPipelineRenderingCreateInfo*>(base);
                request.color_attachment_count = rendering->colorAttachmentCount < gb::max_color_attachments
                                                     ? rendering->colorAttachmentCount
                                                     : gb::max_color_attachments;
                for (uint32_t c = 0; c < request.color_attachment_count; ++c)
                {
                    request.color_formats[c] = static_cast<uint32_t>(rendering->pColorAttachmentFormats[c]);
                }
                request.depth_format = static_cast<uint32_t>(rendering->depthAttachmentFormat);
                request.stencil_format = static_cast<uint32_t>(rendering->stencilAttachmentFormat);
                break;
            }

            const auto spec_block_bytes = [](uint32_t entries, uint32_t bytes) {
                return static_cast<size_t>(entries) * sizeof(gb::specialization_map_entry) + bytes;
            };
            std::vector<uint8_t> message(sizeof(request) + static_cast<size_t>(binding_count) * sizeof(gb::vertex_input_binding) +
                                         static_cast<size_t>(attribute_count) * sizeof(gb::vertex_input_attribute) +
                                         static_cast<size_t>(dynamic_state_count) * sizeof(uint32_t) +
                                         spec_block_bytes(vs_spec_entries, vs_spec_bytes) +
                                         spec_block_bytes(fs_spec_entries, fs_spec_bytes));
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
            for (uint32_t d = 0; d < dynamic_state_count; ++d)
            {
                const auto value = static_cast<uint32_t>(ci.pDynamicState->pDynamicStates[d]);
                std::memcpy(message.data() + cursor, &value, sizeof(value));
                cursor += sizeof(value);
            }
            const auto append_spec = [&](const VkSpecializationInfo* spec, uint32_t entries, uint32_t bytes) {
                for (uint32_t e = 0; e < entries; ++e)
                {
                    gb::specialization_map_entry wire{};
                    wire.constant_id = spec->pMapEntries[e].constantID;
                    wire.offset = spec->pMapEntries[e].offset;
                    wire.size = static_cast<uint32_t>(spec->pMapEntries[e].size);
                    std::memcpy(message.data() + cursor, &wire, sizeof(wire));
                    cursor += sizeof(wire);
                }
                if (bytes > 0)
                {
                    std::memcpy(message.data() + cursor, spec->pData, bytes);
                    cursor += bytes;
                }
            };
            append_spec(vs_spec, vs_spec_entries, vs_spec_bytes);
            append_spec(fs_spec, fs_spec_entries, fs_spec_bytes);

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

            for (uint32_t m = 0; m < owned_count; ++m)
            {
                vkDestroyShaderModule(device, owned_modules[m], nullptr);
            }
        }
        return overall;
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkCreateComputePipelines(VkDevice device, VkPipelineCache,
                                                                                  uint32_t createInfoCount,
                                                                                  const VkComputePipelineCreateInfo* pCreateInfos,
                                                                                  const VkAllocationCallbacks*, VkPipeline* pPipelines)
    {
        VkResult overall = VK_SUCCESS;
        for (uint32_t i = 0; i < createInfoCount; ++i)
        {
            const VkComputePipelineCreateInfo& ci = pCreateInfos[i];

            bool owned = false;
            const VkShaderModule module = resolve_stage_module(device, ci.stage, owned);

            gb::create_compute_pipeline_request request{};
            request.device = to_object_id(device);
            request.pipeline_layout = to_object_id(ci.layout);
            request.shader_module = to_object_id(module);

            gb::create_compute_pipeline_response response{};
            const bool ok = bridge_call(gb::ioctl_create_compute_pipeline, &request, sizeof(request), &response, sizeof(response));
            if (!ok || response.vk_result != VK_SUCCESS)
            {
                pPipelines[i] = VK_NULL_HANDLE;
                overall = ok ? static_cast<VkResult>(response.vk_result) : VK_ERROR_INITIALIZATION_FAILED;
            }
            else
            {
                pPipelines[i] = to_handle<VkPipeline>(response.pipeline);
            }

            if (owned)
            {
                vkDestroyShaderModule(device, module, nullptr);
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

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdBindPipeline(VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint,
                                                                       VkPipeline pipeline)
    {
        gb::cmd_bind_pipeline_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        request.pipeline = to_object_id(pipeline);
        request.bind_point = static_cast<uint32_t>(pipelineBindPoint);
        record_command(request.command_buffer, gb::command::cmd_bind_pipeline, &request, sizeof(request));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdDispatch(VkCommandBuffer commandBuffer, uint32_t groupCountX,
                                                                   uint32_t groupCountY, uint32_t groupCountZ)
    {
        gb::cmd_dispatch_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        request.group_count_x = groupCountX;
        request.group_count_y = groupCountY;
        request.group_count_z = groupCountZ;
        record_command(request.command_buffer, gb::command::cmd_dispatch, &request, sizeof(request));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdDispatchIndirect(VkCommandBuffer commandBuffer, VkBuffer buffer,
                                                                           VkDeviceSize offset)
    {
        gb::cmd_dispatch_indirect_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        request.buffer = to_object_id(buffer);
        request.offset = offset;
        record_command(request.command_buffer, gb::command::cmd_dispatch_indirect, &request, sizeof(request));
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

    // Core 1.3 / VK_EXT_extended_dynamic_state: vertex binding with dynamic per-binding size and stride.
    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdBindVertexBuffers2(VkCommandBuffer commandBuffer, uint32_t firstBinding,
                                                                             uint32_t bindingCount, const VkBuffer* pBuffers,
                                                                             const VkDeviceSize* pOffsets, const VkDeviceSize* pSizes,
                                                                             const VkDeviceSize* pStrides)
    {
        const gb::object_id command_buffer = to_object_id(commandBuffer);
        std::vector<uint8_t> message(sizeof(gb::cmd_bind_vertex_buffers2_request) +
                                     static_cast<size_t>(bindingCount) * sizeof(gb::vertex_buffer_binding2));
        gb::cmd_bind_vertex_buffers2_request header{};
        header.command_buffer = command_buffer;
        header.first_binding = firstBinding;
        header.binding_count = bindingCount;
        header.has_sizes = pSizes ? 1u : 0u;
        header.has_strides = pStrides ? 1u : 0u;
        std::memcpy(message.data(), &header, sizeof(header));
        for (uint32_t i = 0; i < bindingCount; ++i)
        {
            gb::vertex_buffer_binding2 vb{};
            vb.buffer = to_object_id(pBuffers[i]);
            vb.offset = pOffsets ? pOffsets[i] : 0;
            vb.size = pSizes ? pSizes[i] : 0;
            vb.stride = pStrides ? pStrides[i] : 0;
            std::memcpy(message.data() + sizeof(header) + i * sizeof(vb), &vb, sizeof(vb));
        }
        record_command(command_buffer, gb::command::cmd_bind_vertex_buffers2, message.data(), message.size());
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

    // VK_KHR_maintenance5 / core 1.4: like vkCmdBindIndexBuffer but with an explicit size. The bridge does
    // not model the size (the base bind covers the buffer from offset, which matches the usual VK_WHOLE_SIZE).
    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdBindIndexBuffer2KHR(VkCommandBuffer commandBuffer, VkBuffer buffer,
                                                                              VkDeviceSize offset, VkDeviceSize /*size*/,
                                                                              VkIndexType indexType)
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

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets(VkCommandBuffer commandBuffer,
                                                                             VkPipelineBindPoint pipelineBindPoint, VkPipelineLayout layout,
                                                                             uint32_t firstSet, uint32_t descriptorSetCount,
                                                                             const VkDescriptorSet* pDescriptorSets,
                                                                             uint32_t dynamicOffsetCount, const uint32_t* pDynamicOffsets)
    {
        // Forward the dynamic offsets: DXVK binds UNIFORM_BUFFER_DYNAMIC descriptors and indexes the current
        // frame's shader constants in a ring buffer through them. Dropping them reads stale constants.
        const gb::object_id command_buffer = to_object_id(commandBuffer);
        std::vector<uint8_t> message(sizeof(gb::cmd_bind_descriptor_sets_request) +
                                     static_cast<size_t>(descriptorSetCount) * sizeof(gb::object_id) +
                                     static_cast<size_t>(dynamicOffsetCount) * sizeof(uint32_t));
        gb::cmd_bind_descriptor_sets_request header{};
        header.command_buffer = command_buffer;
        header.pipeline_layout = to_object_id(layout);
        header.first_set = firstSet;
        header.set_count = descriptorSetCount;
        header.bind_point = static_cast<uint32_t>(pipelineBindPoint);
        header.dynamic_offset_count = dynamicOffsetCount;
        std::memcpy(message.data(), &header, sizeof(header));
        size_t cursor = sizeof(header);
        for (uint32_t i = 0; i < descriptorSetCount; ++i)
        {
            const gb::object_id id = to_object_id(pDescriptorSets[i]);
            std::memcpy(message.data() + cursor, &id, sizeof(id));
            cursor += sizeof(id);
        }
        for (uint32_t i = 0; i < dynamicOffsetCount; ++i)
        {
            std::memcpy(message.data() + cursor, &pDynamicOffsets[i], sizeof(uint32_t));
            cursor += sizeof(uint32_t);
        }
        record_command(command_buffer, gb::command::cmd_bind_descriptor_sets, message.data(), message.size());
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets2KHR(VkCommandBuffer commandBuffer,
                                                                                 const VkBindDescriptorSetsInfo* pBindDescriptorSetsInfo)
    {
        if (!pBindDescriptorSetsInfo)
        {
            return;
        }

        const auto bind = [&](const VkPipelineBindPoint bind_point) {
            vkCmdBindDescriptorSets(commandBuffer, bind_point, pBindDescriptorSetsInfo->layout, pBindDescriptorSetsInfo->firstSet,
                                    pBindDescriptorSetsInfo->descriptorSetCount, pBindDescriptorSetsInfo->pDescriptorSets,
                                    pBindDescriptorSetsInfo->dynamicOffsetCount, pBindDescriptorSetsInfo->pDynamicOffsets);
        };
        if (pBindDescriptorSetsInfo->stageFlags & VK_SHADER_STAGE_ALL_GRAPHICS)
        {
            bind(VK_PIPELINE_BIND_POINT_GRAPHICS);
        }
        if (pBindDescriptorSetsInfo->stageFlags & VK_SHADER_STAGE_COMPUTE_BIT)
        {
            bind(VK_PIPELINE_BIND_POINT_COMPUTE);
        }
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdEndRenderPass(VkCommandBuffer commandBuffer)
    {
        gb::cmd_end_render_pass_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        record_command(request.command_buffer, gb::command::cmd_end_render_pass, &request, sizeof(request));
    }

    // Dynamic rendering (VK_KHR_dynamic_rendering / core 1.3): DXVK 2.x records draws inside this instead of
    // a render pass + framebuffer. Marshal the VkRenderingInfo and its attachment arrays across the bridge.
    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdBeginRendering(VkCommandBuffer commandBuffer,
                                                                         const VkRenderingInfo* pRenderingInfo)
    {
        const auto to_wire = [](const VkRenderingAttachmentInfo& a) {
            gb::rendering_attachment w{};
            w.image_view = to_object_id(a.imageView);
            w.resolve_image_view = to_object_id(a.resolveImageView);
            w.image_layout = static_cast<uint32_t>(a.imageLayout);
            w.resolve_image_layout = static_cast<uint32_t>(a.resolveImageLayout);
            w.resolve_mode = static_cast<uint32_t>(a.resolveMode);
            w.load_op = static_cast<uint32_t>(a.loadOp);
            w.store_op = static_cast<uint32_t>(a.storeOp);
            std::memcpy(w.clear_value.data(), &a.clearValue, sizeof(w.clear_value));
            return w;
        };

        const gb::object_id command_buffer = to_object_id(commandBuffer);
        const uint32_t color_count = pRenderingInfo->colorAttachmentCount;
        const bool has_depth = pRenderingInfo->pDepthAttachment != nullptr && pRenderingInfo->pDepthAttachment->imageView != VK_NULL_HANDLE;
        const bool has_stencil =
            pRenderingInfo->pStencilAttachment != nullptr && pRenderingInfo->pStencilAttachment->imageView != VK_NULL_HANDLE;
        const uint32_t total = color_count + (has_depth ? 1u : 0u) + (has_stencil ? 1u : 0u);

        std::vector<uint8_t> message(sizeof(gb::cmd_begin_rendering_request) +
                                     static_cast<size_t>(total) * sizeof(gb::rendering_attachment));
        gb::cmd_begin_rendering_request header{};
        header.command_buffer = command_buffer;
        header.render_area_x = pRenderingInfo->renderArea.offset.x;
        header.render_area_y = pRenderingInfo->renderArea.offset.y;
        header.render_area_width = pRenderingInfo->renderArea.extent.width;
        header.render_area_height = pRenderingInfo->renderArea.extent.height;
        header.layer_count = pRenderingInfo->layerCount;
        header.view_mask = pRenderingInfo->viewMask;
        header.color_attachment_count = color_count;
        header.has_depth = has_depth ? 1u : 0u;
        header.has_stencil = has_stencil ? 1u : 0u;
        header.flags = static_cast<uint32_t>(pRenderingInfo->flags);
        std::memcpy(message.data(), &header, sizeof(header));

        size_t offset = sizeof(header);
        for (uint32_t i = 0; i < color_count; ++i)
        {
            const auto w = to_wire(pRenderingInfo->pColorAttachments[i]);
            std::memcpy(message.data() + offset, &w, sizeof(w));
            offset += sizeof(w);
        }
        if (has_depth)
        {
            const auto w = to_wire(*pRenderingInfo->pDepthAttachment);
            std::memcpy(message.data() + offset, &w, sizeof(w));
            offset += sizeof(w);
        }
        if (has_stencil)
        {
            const auto w = to_wire(*pRenderingInfo->pStencilAttachment);
            std::memcpy(message.data() + offset, &w, sizeof(w));
            offset += sizeof(w);
        }

        record_command(command_buffer, gb::command::cmd_begin_rendering, message.data(), message.size());
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdEndRendering(VkCommandBuffer commandBuffer)
    {
        gb::cmd_end_rendering_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        record_command(request.command_buffer, gb::command::cmd_end_rendering, &request, sizeof(request));
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

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdPushConstants2KHR(VkCommandBuffer commandBuffer,
                                                                            const VkPushConstantsInfo* pPushConstantsInfo)
    {
        if (!pPushConstantsInfo)
        {
            return;
        }

        vkCmdPushConstants(commandBuffer, pPushConstantsInfo->layout, pPushConstantsInfo->stageFlags, pPushConstantsInfo->offset,
                           pPushConstantsInfo->size, pPushConstantsInfo->pValues);
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
            {.name = "vkResetCommandPool", .func = reinterpret_cast<PFN_vkVoidFunction>(vkResetCommandPool)},
            {.name = "vkResetCommandBuffer", .func = reinterpret_cast<PFN_vkVoidFunction>(vkResetCommandBuffer)},
            {.name = "vkQueueWaitIdle", .func = reinterpret_cast<PFN_vkVoidFunction>(vkQueueWaitIdle)},
            {.name = "vkDeviceWaitIdle", .func = reinterpret_cast<PFN_vkVoidFunction>(vkDeviceWaitIdle)},
            {.name = "vkCreateEvent", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCreateEvent)},
            {.name = "vkDestroyEvent", .func = reinterpret_cast<PFN_vkVoidFunction>(vkDestroyEvent)},
            {.name = "vkGetEventStatus", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetEventStatus)},
            {.name = "vkSetEvent", .func = reinterpret_cast<PFN_vkVoidFunction>(vkSetEvent)},
            {.name = "vkResetEvent", .func = reinterpret_cast<PFN_vkVoidFunction>(vkResetEvent)},
            {.name = "vkCmdSetEvent", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetEvent)},
            {.name = "vkCmdResetEvent", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdResetEvent)},
            {.name = "vkCmdWaitEvents", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdWaitEvents)},
            {.name = "vkCmdSetEvent2", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetEvent2)},
            {.name = "vkCmdSetEvent2KHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetEvent2)},
            {.name = "vkCmdResetEvent2", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdResetEvent2)},
            {.name = "vkCmdResetEvent2KHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdResetEvent2)},
            {.name = "vkCmdWaitEvents2", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdWaitEvents2)},
            {.name = "vkCmdWaitEvents2KHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdWaitEvents2)},
            {.name = "vkCreateFence", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCreateFence)},
            {.name = "vkCreateSemaphore", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCreateSemaphore)},
            {.name = "vkDestroySemaphore", .func = reinterpret_cast<PFN_vkVoidFunction>(vkDestroySemaphore)},
            {.name = "vkGetSemaphoreCounterValue", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetSemaphoreCounterValue)},
            {.name = "vkGetSemaphoreCounterValueKHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetSemaphoreCounterValue)},
            {.name = "vkSignalSemaphore", .func = reinterpret_cast<PFN_vkVoidFunction>(vkSignalSemaphore)},
            {.name = "vkSignalSemaphoreKHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkSignalSemaphore)},
            {.name = "vkWaitSemaphores", .func = reinterpret_cast<PFN_vkVoidFunction>(vkWaitSemaphores)},
            {.name = "vkWaitSemaphoresKHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkWaitSemaphores)},
            {.name = "vkGetBufferDeviceAddress", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetBufferDeviceAddress)},
            {.name = "vkGetBufferDeviceAddressKHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetBufferDeviceAddress)},
            {.name = "vkGetBufferDeviceAddressEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetBufferDeviceAddress)},
            {.name = "vkDestroyFence", .func = reinterpret_cast<PFN_vkVoidFunction>(vkDestroyFence)},
            {.name = "vkResetFences", .func = reinterpret_cast<PFN_vkVoidFunction>(vkResetFences)},
            {.name = "vkGetFenceStatus", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetFenceStatus)},
            {.name = "vkQueueSubmit", .func = reinterpret_cast<PFN_vkVoidFunction>(vkQueueSubmit)},
            {.name = "vkQueueSubmit2", .func = reinterpret_cast<PFN_vkVoidFunction>(vkQueueSubmit2)},
            {.name = "vkQueueSubmit2KHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkQueueSubmit2)},
            {.name = "vkWaitForFences", .func = reinterpret_cast<PFN_vkVoidFunction>(vkWaitForFences)},
            {.name = "vkGetPhysicalDeviceMemoryProperties",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceMemoryProperties)},
            {.name = "vkAllocateMemory", .func = reinterpret_cast<PFN_vkVoidFunction>(vkAllocateMemory)},
            {.name = "vkFreeMemory", .func = reinterpret_cast<PFN_vkVoidFunction>(vkFreeMemory)},
            {.name = "vkMapMemory", .func = reinterpret_cast<PFN_vkVoidFunction>(vkMapMemory)},
            {.name = "vkUnmapMemory", .func = reinterpret_cast<PFN_vkVoidFunction>(vkUnmapMemory)},
            {.name = "vkCreateBuffer", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCreateBuffer)},
            {.name = "vkDestroyBuffer", .func = reinterpret_cast<PFN_vkVoidFunction>(vkDestroyBuffer)},
            {.name = "vkCreateBufferView", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCreateBufferView)},
            {.name = "vkDestroyBufferView", .func = reinterpret_cast<PFN_vkVoidFunction>(vkDestroyBufferView)},
            {.name = "vkCmdCopyBuffer", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdCopyBuffer)},
            {.name = "vkCmdCopyBuffer2", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdCopyBuffer2)},
            {.name = "vkCmdCopyBuffer2KHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdCopyBuffer2)},
            {.name = "vkCreateQueryPool", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCreateQueryPool)},
            {.name = "vkDestroyQueryPool", .func = reinterpret_cast<PFN_vkVoidFunction>(vkDestroyQueryPool)},
            {.name = "vkResetQueryPool", .func = reinterpret_cast<PFN_vkVoidFunction>(vkResetQueryPool)},
            {.name = "vkResetQueryPoolEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkResetQueryPool)},
            {.name = "vkGetQueryPoolResults", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetQueryPoolResults)},
            {.name = "vkCmdResetQueryPool", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdResetQueryPool)},
            {.name = "vkCmdBeginQuery", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdBeginQuery)},
            {.name = "vkCmdEndQuery", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdEndQuery)},
            {.name = "vkCmdWriteTimestamp", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdWriteTimestamp)},
            {.name = "vkFlushMappedMemoryRanges", .func = reinterpret_cast<PFN_vkVoidFunction>(vkFlushMappedMemoryRanges)},
            {.name = "vkInvalidateMappedMemoryRanges", .func = reinterpret_cast<PFN_vkVoidFunction>(vkInvalidateMappedMemoryRanges)},
            {.name = "vkGetBufferMemoryRequirements", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetBufferMemoryRequirements)},
            {.name = "vkGetBufferMemoryRequirements2", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetBufferMemoryRequirements2)},
            {.name = "vkGetBufferMemoryRequirements2KHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetBufferMemoryRequirements2)},
            {.name = "vkBindBufferMemory", .func = reinterpret_cast<PFN_vkVoidFunction>(vkBindBufferMemory)},
            {.name = "vkBindBufferMemory2", .func = reinterpret_cast<PFN_vkVoidFunction>(vkBindBufferMemory2)},
            {.name = "vkBindBufferMemory2KHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkBindBufferMemory2)},
            {.name = "vkBindImageMemory2", .func = reinterpret_cast<PFN_vkVoidFunction>(vkBindImageMemory2)},
            {.name = "vkBindImageMemory2KHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkBindImageMemory2)},
            {.name = "vkCmdFillBuffer", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdFillBuffer)},
            {.name = "vkCreateImage", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCreateImage)},
            {.name = "vkDestroyImage", .func = reinterpret_cast<PFN_vkVoidFunction>(vkDestroyImage)},
            {.name = "vkGetImageMemoryRequirements", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetImageMemoryRequirements)},
            {.name = "vkGetImageSubresourceLayout", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetImageSubresourceLayout)},
            {.name = "vkGetImageSubresourceLayout2KHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetImageSubresourceLayout2KHR)},
            {.name = "vkGetImageSubresourceLayout2EXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetImageSubresourceLayout2KHR)},
            {.name = "vkGetImageMemoryRequirements2", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetImageMemoryRequirements2)},
            {.name = "vkGetImageMemoryRequirements2KHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetImageMemoryRequirements2)},
            {.name = "vkGetDeviceBufferMemoryRequirements",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceBufferMemoryRequirements)},
            {.name = "vkGetDeviceBufferMemoryRequirementsKHR",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceBufferMemoryRequirements)},
            {.name = "vkGetDeviceImageMemoryRequirements",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceImageMemoryRequirements)},
            {.name = "vkGetDeviceImageMemoryRequirementsKHR",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceImageMemoryRequirements)},
            {.name = "vkBindImageMemory", .func = reinterpret_cast<PFN_vkVoidFunction>(vkBindImageMemory)},
            {.name = "vkCmdPipelineBarrier", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdPipelineBarrier)},
            {.name = "vkCmdPipelineBarrier2", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdPipelineBarrier2)},
            {.name = "vkCmdPipelineBarrier2KHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdPipelineBarrier2)},
            {.name = "vkCmdClearColorImage", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdClearColorImage)},
            {.name = "vkCmdClearDepthStencilImage", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdClearDepthStencilImage)},
            {.name = "vkCmdCopyImageToBuffer", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdCopyImageToBuffer)},
            {.name = "vkCmdCopyImageToBuffer2", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdCopyImageToBuffer2)},
            {.name = "vkCmdCopyImageToBuffer2KHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdCopyImageToBuffer2)},
            {.name = "vkCmdCopyBufferToImage", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdCopyBufferToImage)},
            {.name = "vkCmdCopyBufferToImage2", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdCopyBufferToImage2)},
            {.name = "vkCmdCopyBufferToImage2KHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdCopyBufferToImage2)},
            {.name = "vkCmdCopyImage", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdCopyImage)},
            {.name = "vkCmdCopyImage2", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdCopyImage2)},
            {.name = "vkCmdCopyImage2KHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdCopyImage2)},
            {.name = "vkCmdUpdateBuffer", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdUpdateBuffer)},
            {.name = "vkCmdResolveImage", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdResolveImage)},
            {.name = "vkCmdResolveImage2", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdResolveImage2)},
            {.name = "vkCmdResolveImage2KHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdResolveImage2)},
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
            {.name = "vkResetDescriptorPool", .func = reinterpret_cast<PFN_vkVoidFunction>(vkResetDescriptorPool)},
            {.name = "vkAllocateDescriptorSets", .func = reinterpret_cast<PFN_vkVoidFunction>(vkAllocateDescriptorSets)},
            {.name = "vkUpdateDescriptorSets", .func = reinterpret_cast<PFN_vkVoidFunction>(vkUpdateDescriptorSets)},
            {.name = "vkCreateDescriptorUpdateTemplate", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCreateDescriptorUpdateTemplate)},
            {.name = "vkCreateDescriptorUpdateTemplateKHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCreateDescriptorUpdateTemplate)},
            {.name = "vkDestroyDescriptorUpdateTemplate", .func = reinterpret_cast<PFN_vkVoidFunction>(vkDestroyDescriptorUpdateTemplate)},
            {.name = "vkDestroyDescriptorUpdateTemplateKHR",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkDestroyDescriptorUpdateTemplate)},
            {.name = "vkUpdateDescriptorSetWithTemplate", .func = reinterpret_cast<PFN_vkVoidFunction>(vkUpdateDescriptorSetWithTemplate)},
            {.name = "vkUpdateDescriptorSetWithTemplateKHR",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkUpdateDescriptorSetWithTemplate)},
            {.name = "vkCmdBindDescriptorSets", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdBindDescriptorSets)},
            {.name = "vkCreateGraphicsPipelines", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCreateGraphicsPipelines)},
            {.name = "vkCreateComputePipelines", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCreateComputePipelines)},
            {.name = "vkDestroyPipeline", .func = reinterpret_cast<PFN_vkVoidFunction>(vkDestroyPipeline)},
            {.name = "vkCmdBeginRenderPass", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdBeginRenderPass)},
            {.name = "vkCmdBeginRendering", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdBeginRendering)},
            {.name = "vkCmdBeginRenderingKHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdBeginRendering)},
            {.name = "vkCmdEndRendering", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdEndRendering)},
            {.name = "vkCmdEndRenderingKHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdEndRendering)},
            {.name = "vkCmdExecuteCommands", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdExecuteCommands)},
            {.name = "vkCmdBindPipeline", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdBindPipeline)},
            {.name = "vkCmdDraw", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdDraw)},
            {.name = "vkCmdDispatch", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdDispatch)},
            {.name = "vkCmdDispatchIndirect", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdDispatchIndirect)},
            {.name = "vkCmdBlitImage2", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdBlitImage2)},
            {.name = "vkCmdBlitImage2KHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdBlitImage2KHR)},
            {.name = "vkCmdBindVertexBuffers", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdBindVertexBuffers)},
            {.name = "vkCmdBindVertexBuffers2", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdBindVertexBuffers2)},
            {.name = "vkCmdBindVertexBuffers2EXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdBindVertexBuffers2)},
            {.name = "vkCmdBindDescriptorSets2KHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdBindDescriptorSets2KHR)},
            {.name = "vkCmdBindDescriptorSets2", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdBindDescriptorSets2KHR)},
            {.name = "vkCmdBindIndexBuffer", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdBindIndexBuffer)},
            {.name = "vkCmdBindIndexBuffer2KHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdBindIndexBuffer2KHR)},
            {.name = "vkCmdBindIndexBuffer2", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdBindIndexBuffer2KHR)},
            {.name = "vkCmdDrawIndexed", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdDrawIndexed)},
            {.name = "vkCmdEndRenderPass", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdEndRenderPass)},
            {.name = "vkCmdPushConstants", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdPushConstants)},
            {.name = "vkCmdPushConstants2KHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdPushConstants2KHR)},
            {.name = "vkCmdPushConstants2", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdPushConstants2KHR)},
            {.name = "vkCmdSetViewport", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetViewport)},
            {.name = "vkCmdSetViewportWithCount", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetViewportWithCount)},
            {.name = "vkCmdSetViewportWithCountEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetViewportWithCount)},
            {.name = "vkCmdSetScissor", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetScissor)},
            {.name = "vkCmdSetScissorWithCount", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetScissorWithCount)},
            {.name = "vkCmdSetScissorWithCountEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetScissorWithCount)},
            {.name = "vkCmdSetDepthBias", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetDepthBias)},
            {.name = "vkCmdSetBlendConstants", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetBlendConstants)},
            {.name = "vkCmdSetDepthBounds", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetDepthBounds)},
            {.name = "vkCmdSetLineWidth", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetLineWidth)},
            {.name = "vkCmdSetStencilCompareMask", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetStencilCompareMask)},
            {.name = "vkCmdSetStencilWriteMask", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetStencilWriteMask)},
            {.name = "vkCmdSetStencilReference", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetStencilReference)},
            {.name = "vkCmdSetStencilOp", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetStencilOp)},
            {.name = "vkCmdSetStencilOpEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetStencilOp)},
            {.name = "vkCmdSetCullMode", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetCullMode)},
            {.name = "vkCmdSetCullModeEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetCullMode)},
            {.name = "vkCmdSetFrontFace", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetFrontFace)},
            {.name = "vkCmdSetFrontFaceEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetFrontFace)},
            {.name = "vkCmdSetPrimitiveTopology", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetPrimitiveTopology)},
            {.name = "vkCmdSetPrimitiveTopologyEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetPrimitiveTopology)},
            {.name = "vkCmdSetDepthTestEnable", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetDepthTestEnable)},
            {.name = "vkCmdSetDepthTestEnableEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetDepthTestEnable)},
            {.name = "vkCmdSetDepthWriteEnable", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetDepthWriteEnable)},
            {.name = "vkCmdSetDepthWriteEnableEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetDepthWriteEnable)},
            {.name = "vkCmdSetDepthCompareOp", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetDepthCompareOp)},
            {.name = "vkCmdSetDepthCompareOpEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetDepthCompareOp)},
            {.name = "vkCmdSetDepthBoundsTestEnable", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetDepthBoundsTestEnable)},
            {.name = "vkCmdSetDepthBoundsTestEnableEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetDepthBoundsTestEnable)},
            {.name = "vkCmdSetDepthClipEnableEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetDepthClipEnableEXT)},
            {.name = "vkCmdSetStencilTestEnable", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetStencilTestEnable)},
            {.name = "vkCmdSetStencilTestEnableEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetStencilTestEnable)},
            {.name = "vkCmdSetRasterizerDiscardEnable", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetRasterizerDiscardEnable)},
            {.name = "vkCmdSetRasterizerDiscardEnableEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetRasterizerDiscardEnable)},
            {.name = "vkCmdSetDepthBiasEnable", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetDepthBiasEnable)},
            {.name = "vkCmdSetDepthBiasEnableEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetDepthBiasEnable)},
            {.name = "vkCmdSetPrimitiveRestartEnable", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetPrimitiveRestartEnable)},
            {.name = "vkCmdSetPrimitiveRestartEnableEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetPrimitiveRestartEnable)},
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
