// Guest-side Vulkan shim: a drop-in vulkan-1.dll that implements the Vulkan entry points by
// forwarding them across the Sogen GPU bridge (\\.\SogenGpu) to the host's real driver. An app
// loads it exactly like a normal ICD/loader -- LoadLibrary + vkGetInstanceProcAddr -- and never
// sees the bridge. This is the guest counterpart to the host gpu_bridge io_device.
//
// Opaque Vulkan handles (VkInstance, VkPhysicalDevice, ...) are pointer-sized, so the bridge's
// object_id is stored directly in the handle value; no guest-side handle table is required.

#include <windows.h>

#include <cstring>
#include <unordered_map>
#include <vector>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan_win32.h>

#include <gpu_bridge_protocol.hpp>

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

    template <typename Handle>
    gb::object_id to_object_id(Handle handle)
    {
        return static_cast<gb::object_id>(reinterpret_cast<uintptr_t>(handle));
    }

    template <typename Handle>
    Handle to_handle(gb::object_id id)
    {
        return reinterpret_cast<Handle>(static_cast<uintptr_t>(id));
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
}

extern "C"
{
    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo*,
                                                                          const VkAllocationCallbacks*, VkInstance* pInstance)
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
        if (!bridge_call(gb::ioctl_enumerate_physical_devices, &request, sizeof(request), buffer.data(),
                         static_cast<DWORD>(buffer.size())))
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

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties(
        VkPhysicalDevice physicalDevice, uint32_t* pCount, VkQueueFamilyProperties* pProperties)
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
                                                                        const VkDeviceCreateInfo* pCreateInfo,
                                                                        const VkAllocationCallbacks*, VkDevice* pDevice)
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

        gb::create_device_response response{};
        if (!bridge_call(gb::ioctl_create_device, &request, sizeof(request), &response, sizeof(response)))
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

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex,
                                                                      uint32_t queueIndex, VkQueue* pQueue)
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

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkCreateCommandPool(VkDevice device,
                                                                            const VkCommandPoolCreateInfo* pCreateInfo,
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
        gb::begin_command_buffer_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        request.flags = pBeginInfo ? pBeginInfo->flags : 0;

        gb::result_response response{};
        if (!bridge_call(gb::ioctl_begin_command_buffer, &request, sizeof(request), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        return static_cast<VkResult>(response.vk_result);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer commandBuffer)
    {
        gb::end_command_buffer_request request{};
        request.command_buffer = to_object_id(commandBuffer);

        gb::result_response response{};
        if (!bridge_call(gb::ioctl_end_command_buffer, &request, sizeof(request), &response, sizeof(response)))
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

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkResetFences(VkDevice device, uint32_t fenceCount,
                                                                      const VkFence* pFences)
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

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(VkQueue queue, uint32_t submitCount,
                                                                      const VkSubmitInfo* pSubmits, VkFence fence)
    {
        // Count the command buffers so the fence can be attached to the final submission only.
        uint32_t total = 0;
        for (uint32_t s = 0; s < submitCount; ++s)
        {
            total += pSubmits[s].commandBufferCount;
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
    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkWaitForFences(VkDevice device, uint32_t fenceCount,
                                                                        const VkFence* pFences, VkBool32 waitAll,
                                                                        uint64_t timeout)
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

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(
        VkPhysicalDevice physicalDevice, VkPhysicalDeviceMemoryProperties* pMemoryProperties)
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

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(VkDevice device,
                                                                          const VkMemoryAllocateInfo* pAllocateInfo,
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

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkFreeMemory(VkDevice device, VkDeviceMemory memory,
                                                                  const VkAllocationCallbacks*)
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

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkMapMemory(VkDevice device, VkDeviceMemory memory,
                                                                     VkDeviceSize offset, VkDeviceSize size,
                                                                     VkMemoryMapFlags, void** ppData)
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
            bridge_call(gb::ioctl_upload_memory, message.data(), static_cast<DWORD>(message.size()), &response,
                        sizeof(response));
        }

        g_mapped_ranges.erase(it);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkCreateBuffer(VkDevice device,
                                                                        const VkBufferCreateInfo* pCreateInfo,
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

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkDestroyBuffer(VkDevice device, VkBuffer buffer,
                                                                     const VkAllocationCallbacks*)
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

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory(VkDevice device, VkBuffer buffer,
                                                                            VkDeviceMemory memory, VkDeviceSize memoryOffset)
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

        gb::result_response response{};
        bridge_call(gb::ioctl_cmd_fill_buffer, &request, sizeof(request), &response, sizeof(response));
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage(VkDevice device,
                                                                       const VkImageCreateInfo* pCreateInfo,
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

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkDestroyImage(VkDevice device, VkImage image,
                                                                    const VkAllocationCallbacks*)
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

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkBindImageMemory(VkDevice device, VkImage image,
                                                                           VkDeviceMemory memory, VkDeviceSize memoryOffset)
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
    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier(
        VkCommandBuffer commandBuffer, VkPipelineStageFlags srcStageMask, VkPipelineStageFlags dstStageMask,
        VkDependencyFlags, uint32_t, const VkMemoryBarrier*, uint32_t, const VkBufferMemoryBarrier*,
        uint32_t imageMemoryBarrierCount, const VkImageMemoryBarrier* pImageMemoryBarriers)
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

            gb::result_response response{};
            bridge_call(gb::ioctl_cmd_pipeline_barrier, &request, sizeof(request), &response, sizeof(response));
        }
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdClearColorImage(VkCommandBuffer commandBuffer, VkImage image,
                                                                          VkImageLayout imageLayout,
                                                                          const VkClearColorValue* pColor,
                                                                          uint32_t rangeCount,
                                                                          const VkImageSubresourceRange* pRanges)
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

            gb::result_response response{};
            bridge_call(gb::ioctl_cmd_clear_color_image, &request, sizeof(request), &response, sizeof(response));
        }
    }

    // Copies are remoted assuming tight packing of mip 0 / layer 0 at image offset 0 to buffer offset 0
    // (bufferRowLength/bufferImageHeight/imageOffset are not yet honored).
    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdCopyImageToBuffer(VkCommandBuffer commandBuffer, VkImage srcImage,
                                                                            VkImageLayout srcImageLayout, VkBuffer dstBuffer,
                                                                            uint32_t regionCount,
                                                                            const VkBufferImageCopy* pRegions)
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

            gb::result_response response{};
            bridge_call(gb::ioctl_cmd_copy_image_to_buffer, &request, sizeof(request), &response, sizeof(response));
        }
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkCreateWin32SurfaceKHR(VkInstance,
                                                                                 const VkWin32SurfaceCreateInfoKHR* pCreateInfo,
                                                                                 const VkAllocationCallbacks*,
                                                                                 VkSurfaceKHR* pSurface)
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

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkDestroySurfaceKHR(VkInstance, VkSurfaceKHR surface,
                                                                         const VkAllocationCallbacks*)
    {
        if (!surface)
        {
            return;
        }
        gb::destroy_surface_request request{};
        request.surface = to_object_id(surface);
        bridge_call(gb::ioctl_destroy_surface, &request, sizeof(request), nullptr, 0);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkCreateSwapchainKHR(VkDevice device,
                                                                              const VkSwapchainCreateInfoKHR* pCreateInfo,
                                                                              const VkAllocationCallbacks*,
                                                                              VkSwapchainKHR* pSwapchain)
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
                                                                                 uint32_t* pSwapchainImageCount,
                                                                                 VkImage* pSwapchainImages)
    {
        const uint32_t capacity = pSwapchainImages ? *pSwapchainImageCount : 0;

        gb::get_swapchain_images_request request{};
        request.swapchain = to_object_id(swapchain);
        request.max_count = capacity;

        std::vector<uint8_t> out(sizeof(gb::get_swapchain_images_response) + capacity * sizeof(gb::object_id));
        if (!bridge_call(gb::ioctl_get_swapchain_images, &request, sizeof(request), out.data(),
                         static_cast<DWORD>(out.size())))
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
                                                                               VkSemaphore, VkFence, uint32_t* pImageIndex)
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
            const bool ok =
                bridge_call(gb::ioctl_queue_present, &request, sizeof(request), &response, sizeof(response));
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
            {.name = "vkEnumeratePhysicalDevices", .func = reinterpret_cast<PFN_vkVoidFunction>(vkEnumeratePhysicalDevices)},
            {.name = "vkGetPhysicalDeviceProperties",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceProperties)},
            {.name = "vkGetPhysicalDeviceQueueFamilyProperties",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceQueueFamilyProperties)},
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
            {.name = "vkGetBufferMemoryRequirements",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetBufferMemoryRequirements)},
            {.name = "vkBindBufferMemory", .func = reinterpret_cast<PFN_vkVoidFunction>(vkBindBufferMemory)},
            {.name = "vkCmdFillBuffer", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdFillBuffer)},
            {.name = "vkCreateImage", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCreateImage)},
            {.name = "vkDestroyImage", .func = reinterpret_cast<PFN_vkVoidFunction>(vkDestroyImage)},
            {.name = "vkGetImageMemoryRequirements",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetImageMemoryRequirements)},
            {.name = "vkBindImageMemory", .func = reinterpret_cast<PFN_vkVoidFunction>(vkBindImageMemory)},
            {.name = "vkCmdPipelineBarrier", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdPipelineBarrier)},
            {.name = "vkCmdClearColorImage", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdClearColorImage)},
            {.name = "vkCmdCopyImageToBuffer", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdCopyImageToBuffer)},
            {.name = "vkCreateWin32SurfaceKHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCreateWin32SurfaceKHR)},
            {.name = "vkDestroySurfaceKHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkDestroySurfaceKHR)},
            {.name = "vkCreateSwapchainKHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCreateSwapchainKHR)},
            {.name = "vkDestroySwapchainKHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkDestroySwapchainKHR)},
            {.name = "vkGetSwapchainImagesKHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetSwapchainImagesKHR)},
            {.name = "vkAcquireNextImageKHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkAcquireNextImageKHR)},
            {.name = "vkQueuePresentKHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkQueuePresentKHR)},
        };

        for (const auto& e : table)
        {
            if (std::strcmp(e.name, pName) == 0)
            {
                return e.func;
            }
        }

        return nullptr;
    }

    // Device-level entry points resolve through the same table for this minimal shim.
    __declspec(dllexport) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice, const char* pName)
    {
        return vkGetInstanceProcAddr(VK_NULL_HANDLE, pName);
    }

    // Some loaders / probes bootstrap through the ICD-style export instead.
    __declspec(dllexport) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance,
                                                                                             const char* pName)
    {
        return vkGetInstanceProcAddr(instance, pName);
    }
}
