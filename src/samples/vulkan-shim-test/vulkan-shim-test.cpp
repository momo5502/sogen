// Exercises the guest Vulkan shim (vulkan-shim.dll) through the genuine Vulkan loading dance --
// LoadLibrary + vkGetInstanceProcAddr -- exactly as a real application or the Khronos loader would.
// The shim forwards each call across the Sogen GPU bridge to the host driver, so this enumerates
// the host's physical devices end-to-end: guest app -> shim -> bridge -> host Vulkan -> GPU.

#include <windows.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

namespace
{
    // Records an empty primary command buffer, submits it with a fence, and waits on the fence
    // through the shim's poll-and-yield vkWaitForFences -- exercising the full submission + sync path
    // against the host GPU.
    void submit_and_wait(PFN_vkGetInstanceProcAddr get_instance_proc, VkInstance instance, VkDevice device, VkQueue queue,
                         uint32_t queue_family)
    {
        const auto create_command_pool = reinterpret_cast<PFN_vkCreateCommandPool>(get_instance_proc(instance, "vkCreateCommandPool"));
        const auto destroy_command_pool = reinterpret_cast<PFN_vkDestroyCommandPool>(get_instance_proc(instance, "vkDestroyCommandPool"));
        const auto allocate_command_buffers =
            reinterpret_cast<PFN_vkAllocateCommandBuffers>(get_instance_proc(instance, "vkAllocateCommandBuffers"));
        const auto begin_command_buffer = reinterpret_cast<PFN_vkBeginCommandBuffer>(get_instance_proc(instance, "vkBeginCommandBuffer"));
        const auto end_command_buffer = reinterpret_cast<PFN_vkEndCommandBuffer>(get_instance_proc(instance, "vkEndCommandBuffer"));
        const auto create_fence = reinterpret_cast<PFN_vkCreateFence>(get_instance_proc(instance, "vkCreateFence"));
        const auto destroy_fence = reinterpret_cast<PFN_vkDestroyFence>(get_instance_proc(instance, "vkDestroyFence"));
        const auto queue_submit = reinterpret_cast<PFN_vkQueueSubmit>(get_instance_proc(instance, "vkQueueSubmit"));
        const auto wait_for_fences = reinterpret_cast<PFN_vkWaitForFences>(get_instance_proc(instance, "vkWaitForFences"));

        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.queueFamilyIndex = queue_family;

        VkCommandPool pool = VK_NULL_HANDLE;
        if (create_command_pool(device, &pool_info, nullptr, &pool) != VK_SUCCESS)
        {
            std::printf("[shim-test] vkCreateCommandPool failed\n");
            return;
        }

        VkCommandBufferAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc_info.commandPool = pool;
        alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc_info.commandBufferCount = 1;

        VkCommandBuffer command_buffer = VK_NULL_HANDLE;
        allocate_command_buffers(device, &alloc_info, &command_buffer);

        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        begin_command_buffer(command_buffer, &begin_info);
        end_command_buffer(command_buffer);

        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence = VK_NULL_HANDLE;
        create_fence(device, &fence_info, nullptr, &fence);

        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command_buffer;

        const VkResult submit_result = queue_submit(queue, 1, &submit, fence);
        // First submission absorbs a software-driver (SwiftShader) JIT cold-start of several seconds;
        // a short timeout would spuriously report VK_TIMEOUT here.
        const VkResult wait_result = wait_for_fences(device, 1, &fence, VK_TRUE, 30000000000ULL /* 30s */);
        std::printf("[shim-test] vkQueueSubmit -> %d, vkWaitForFences -> %d\n", submit_result, wait_result);

        // A zero-batch submission (no command buffers) must still signal the fence; otherwise the wait
        // would spin forever.
        VkFence empty_fence = VK_NULL_HANDLE;
        create_fence(device, &fence_info, nullptr, &empty_fence);
        const VkResult empty_submit = queue_submit(queue, 0, nullptr, empty_fence);
        const VkResult empty_wait = wait_for_fences(device, 1, &empty_fence, VK_TRUE, 30000000000ULL /* 30s */);
        std::printf("[shim-test] empty submit -> %d, fence wait -> %d -> %s\n", empty_submit, empty_wait,
                    (empty_submit == VK_SUCCESS && empty_wait == VK_SUCCESS) ? "PASS" : "FAIL");
        destroy_fence(device, empty_fence, nullptr);

        destroy_fence(device, fence, nullptr);
        destroy_command_pool(device, pool, nullptr);
    }

    // Records both promoted spellings with synchronization2-only stage bits. Ending the command buffer
    // flushes the shim command stream, so this catches any truncation or legacy-stage substitution.
    bool test_write_timestamp2(PFN_vkGetInstanceProcAddr get_instance_proc, VkInstance instance, VkDevice device,
                               uint32_t queue_family, PFN_vkCmdWriteTimestamp2 write_timestamp2,
                               PFN_vkCmdWriteTimestamp2KHR write_timestamp2_khr)
    {
        const auto create_command_pool = reinterpret_cast<PFN_vkCreateCommandPool>(get_instance_proc(instance, "vkCreateCommandPool"));
        const auto destroy_command_pool = reinterpret_cast<PFN_vkDestroyCommandPool>(get_instance_proc(instance, "vkDestroyCommandPool"));
        const auto allocate_command_buffers =
            reinterpret_cast<PFN_vkAllocateCommandBuffers>(get_instance_proc(instance, "vkAllocateCommandBuffers"));
        const auto begin_command_buffer = reinterpret_cast<PFN_vkBeginCommandBuffer>(get_instance_proc(instance, "vkBeginCommandBuffer"));
        const auto end_command_buffer = reinterpret_cast<PFN_vkEndCommandBuffer>(get_instance_proc(instance, "vkEndCommandBuffer"));
        const auto create_query_pool = reinterpret_cast<PFN_vkCreateQueryPool>(get_instance_proc(instance, "vkCreateQueryPool"));
        const auto destroy_query_pool = reinterpret_cast<PFN_vkDestroyQueryPool>(get_instance_proc(instance, "vkDestroyQueryPool"));
        if (!create_command_pool || !destroy_command_pool || !allocate_command_buffers || !begin_command_buffer ||
            !end_command_buffer || !create_query_pool || !destroy_query_pool)
        {
            std::printf("[shim-test] timestamp2 support entry point missing\n");
            return false;
        }

        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.queueFamilyIndex = queue_family;

        VkCommandPool pool = VK_NULL_HANDLE;
        if (create_command_pool(device, &pool_info, nullptr, &pool) != VK_SUCCESS)
        {
            std::printf("[shim-test] timestamp2 vkCreateCommandPool failed\n");
            return false;
        }

        VkCommandBufferAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc_info.commandPool = pool;
        alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc_info.commandBufferCount = 1;

        VkCommandBuffer command_buffer = VK_NULL_HANDLE;
        const VkResult allocate_result = allocate_command_buffers(device, &alloc_info, &command_buffer);

        VkQueryPoolCreateInfo query_info{};
        query_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        query_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        query_info.queryCount = 2;

        VkQueryPool query_pool = VK_NULL_HANDLE;
        const VkResult query_result = create_query_pool(device, &query_info, nullptr, &query_pool);

        VkResult begin_result = VK_ERROR_INITIALIZATION_FAILED;
        VkResult end_result = VK_ERROR_INITIALIZATION_FAILED;
        if (allocate_result == VK_SUCCESS && query_result == VK_SUCCESS)
        {
            VkCommandBufferBeginInfo begin_info{};
            begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            begin_result = begin_command_buffer(command_buffer, &begin_info);
            if (begin_result == VK_SUCCESS)
            {
                write_timestamp2(command_buffer, VK_PIPELINE_STAGE_2_COPY_BIT, query_pool, 0);
                write_timestamp2_khr(command_buffer, VK_PIPELINE_STAGE_2_RESOLVE_BIT, query_pool, 1);
                end_result = end_command_buffer(command_buffer);
            }
        }

        const bool ok = allocate_result == VK_SUCCESS && query_result == VK_SUCCESS && begin_result == VK_SUCCESS &&
                        end_result == VK_SUCCESS;
        std::printf("[shim-test] timestamp2 stage2-only recording -> %s\n", ok ? "PASS" : "FAIL");

        if (query_pool != VK_NULL_HANDLE)
        {
            destroy_query_pool(device, query_pool, nullptr);
        }
        destroy_command_pool(device, pool, nullptr);
        return ok;
    }

    // Picks the first memory type that is set in `type_bits` and carries all of `required` property
    // flags. Returns UINT32_MAX if none qualifies.
    uint32_t find_memory_type(const VkPhysicalDeviceMemoryProperties& props, uint32_t type_bits, VkMemoryPropertyFlags required)
    {
        for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
        {
            if ((type_bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & required) == required)
            {
                return i;
            }
        }
        return UINT32_MAX;
    }

    // Allocates a host-visible buffer, has the GPU fill it with a known pattern via vkCmdFillBuffer,
    // then maps it back and verifies the bytes -- the first end-to-end "GPU produces data the guest
    // reads back" path across the bridge.
    bool fill_buffer_and_readback(PFN_vkGetInstanceProcAddr get_instance_proc, VkInstance instance, VkPhysicalDevice physical_device,
                                  VkDevice device, VkQueue queue, uint32_t queue_family)
    {
        const auto get_memory_properties =
            reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(get_instance_proc(instance, "vkGetPhysicalDeviceMemoryProperties"));
        const auto create_buffer = reinterpret_cast<PFN_vkCreateBuffer>(get_instance_proc(instance, "vkCreateBuffer"));
        const auto destroy_buffer = reinterpret_cast<PFN_vkDestroyBuffer>(get_instance_proc(instance, "vkDestroyBuffer"));
        const auto get_buffer_reqs =
            reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(get_instance_proc(instance, "vkGetBufferMemoryRequirements"));
        const auto allocate_memory = reinterpret_cast<PFN_vkAllocateMemory>(get_instance_proc(instance, "vkAllocateMemory"));
        const auto free_memory = reinterpret_cast<PFN_vkFreeMemory>(get_instance_proc(instance, "vkFreeMemory"));
        const auto bind_buffer_memory = reinterpret_cast<PFN_vkBindBufferMemory>(get_instance_proc(instance, "vkBindBufferMemory"));
        const auto map_memory = reinterpret_cast<PFN_vkMapMemory>(get_instance_proc(instance, "vkMapMemory"));
        const auto unmap_memory = reinterpret_cast<PFN_vkUnmapMemory>(get_instance_proc(instance, "vkUnmapMemory"));
        const auto cmd_fill_buffer = reinterpret_cast<PFN_vkCmdFillBuffer>(get_instance_proc(instance, "vkCmdFillBuffer"));

        const auto create_command_pool = reinterpret_cast<PFN_vkCreateCommandPool>(get_instance_proc(instance, "vkCreateCommandPool"));
        const auto destroy_command_pool = reinterpret_cast<PFN_vkDestroyCommandPool>(get_instance_proc(instance, "vkDestroyCommandPool"));
        const auto allocate_command_buffers =
            reinterpret_cast<PFN_vkAllocateCommandBuffers>(get_instance_proc(instance, "vkAllocateCommandBuffers"));
        const auto begin_command_buffer = reinterpret_cast<PFN_vkBeginCommandBuffer>(get_instance_proc(instance, "vkBeginCommandBuffer"));
        const auto end_command_buffer = reinterpret_cast<PFN_vkEndCommandBuffer>(get_instance_proc(instance, "vkEndCommandBuffer"));
        const auto create_fence = reinterpret_cast<PFN_vkCreateFence>(get_instance_proc(instance, "vkCreateFence"));
        const auto destroy_fence = reinterpret_cast<PFN_vkDestroyFence>(get_instance_proc(instance, "vkDestroyFence"));
        const auto queue_submit = reinterpret_cast<PFN_vkQueueSubmit>(get_instance_proc(instance, "vkQueueSubmit"));
        const auto wait_for_fences = reinterpret_cast<PFN_vkWaitForFences>(get_instance_proc(instance, "vkWaitForFences"));

        constexpr VkDeviceSize buffer_size = 256;
        constexpr uint32_t fill_value = 0xDEADBEEFu;

        VkBufferCreateInfo buffer_info{};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = buffer_size;
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VkBuffer buffer = VK_NULL_HANDLE;
        if (create_buffer(device, &buffer_info, nullptr, &buffer) != VK_SUCCESS)
        {
            std::printf("[shim-test] vkCreateBuffer failed\n");
            return false;
        }

        VkMemoryRequirements reqs{};
        get_buffer_reqs(device, buffer, &reqs);

        VkPhysicalDeviceMemoryProperties mem_props{};
        get_memory_properties(physical_device, &mem_props);
        const uint32_t type_index =
            find_memory_type(mem_props, reqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (type_index == UINT32_MAX)
        {
            std::printf("[shim-test] no host-visible memory type\n");
            destroy_buffer(device, buffer, nullptr);
            return false;
        }

        VkMemoryAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = reqs.size;
        alloc.memoryTypeIndex = type_index;

        VkDeviceMemory memory = VK_NULL_HANDLE;
        if (allocate_memory(device, &alloc, nullptr, &memory) != VK_SUCCESS)
        {
            std::printf("[shim-test] vkAllocateMemory failed\n");
            destroy_buffer(device, buffer, nullptr);
            return false;
        }
        bind_buffer_memory(device, buffer, memory, 0);

        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.queueFamilyIndex = queue_family;
        VkCommandPool pool = VK_NULL_HANDLE;
        create_command_pool(device, &pool_info, nullptr, &pool);

        VkCommandBufferAllocateInfo cb_info{};
        cb_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cb_info.commandPool = pool;
        cb_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cb_info.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        allocate_command_buffers(device, &cb_info, &cmd);

        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        begin_command_buffer(cmd, &begin);
        cmd_fill_buffer(cmd, buffer, 0, buffer_size, fill_value);
        end_command_buffer(cmd);

        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence = VK_NULL_HANDLE;
        create_fence(device, &fence_info, nullptr, &fence);

        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        queue_submit(queue, 1, &submit, fence);
        // SwiftShader (software) JIT-compiles its submission path on first use, which can take several
        // seconds of wall-clock; give the fence a generous timeout so cold-start doesn't read as failure.
        const VkResult wait_result = wait_for_fences(device, 1, &fence, VK_TRUE, 30000000000ULL /* 30s */);
        std::printf("[shim-test] fill: vkWaitForFences -> %d\n", wait_result);

        bool ok = (wait_result == VK_SUCCESS);
        if (ok)
        {
            void* mapped = nullptr;
            if (map_memory(device, memory, 0, buffer_size, 0, &mapped) == VK_SUCCESS && mapped)
            {
                const auto* words = static_cast<const uint32_t*>(mapped);
                for (uint32_t i = 0; i < buffer_size / sizeof(uint32_t); ++i)
                {
                    if (words[i] != fill_value)
                    {
                        ok = false;
                        std::printf("[shim-test] readback mismatch at word %u: 0x%08X != 0x%08X\n", i, words[i], fill_value);
                        break;
                    }
                }
                unmap_memory(device, memory);
            }
            else
            {
                ok = false;
                std::printf("[shim-test] vkMapMemory failed\n");
            }
        }

        std::printf("[shim-test] fill+readback (0x%08X x%u) -> %s\n", fill_value, static_cast<uint32_t>(buffer_size / sizeof(uint32_t)),
                    ok ? "PASS" : "FAIL");

        destroy_fence(device, fence, nullptr);
        destroy_command_pool(device, pool, nullptr);
        free_memory(device, memory, nullptr);
        destroy_buffer(device, buffer, nullptr);
        return ok;
    }

    // Renders into an image the simplest possible way -- clears it to a known color on the GPU -- then
    // copies it into a host-visible buffer and reads the pixels back. This is the offscreen render
    // target + readback path that windowed present will reuse (present = clear/draw -> copy -> readback).
    bool clear_image_and_readback(PFN_vkGetInstanceProcAddr get_instance_proc, VkInstance instance, VkPhysicalDevice physical_device,
                                  VkDevice device, VkQueue queue, uint32_t queue_family)
    {
        const auto get_memory_properties =
            reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(get_instance_proc(instance, "vkGetPhysicalDeviceMemoryProperties"));
        const auto create_image = reinterpret_cast<PFN_vkCreateImage>(get_instance_proc(instance, "vkCreateImage"));
        const auto destroy_image = reinterpret_cast<PFN_vkDestroyImage>(get_instance_proc(instance, "vkDestroyImage"));
        const auto get_image_reqs =
            reinterpret_cast<PFN_vkGetImageMemoryRequirements>(get_instance_proc(instance, "vkGetImageMemoryRequirements"));
        const auto bind_image_memory = reinterpret_cast<PFN_vkBindImageMemory>(get_instance_proc(instance, "vkBindImageMemory"));
        const auto create_buffer = reinterpret_cast<PFN_vkCreateBuffer>(get_instance_proc(instance, "vkCreateBuffer"));
        const auto destroy_buffer = reinterpret_cast<PFN_vkDestroyBuffer>(get_instance_proc(instance, "vkDestroyBuffer"));
        const auto get_buffer_reqs =
            reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(get_instance_proc(instance, "vkGetBufferMemoryRequirements"));
        const auto allocate_memory = reinterpret_cast<PFN_vkAllocateMemory>(get_instance_proc(instance, "vkAllocateMemory"));
        const auto free_memory = reinterpret_cast<PFN_vkFreeMemory>(get_instance_proc(instance, "vkFreeMemory"));
        const auto bind_buffer_memory = reinterpret_cast<PFN_vkBindBufferMemory>(get_instance_proc(instance, "vkBindBufferMemory"));
        const auto map_memory = reinterpret_cast<PFN_vkMapMemory>(get_instance_proc(instance, "vkMapMemory"));
        const auto unmap_memory = reinterpret_cast<PFN_vkUnmapMemory>(get_instance_proc(instance, "vkUnmapMemory"));
        const auto cmd_pipeline_barrier = reinterpret_cast<PFN_vkCmdPipelineBarrier>(get_instance_proc(instance, "vkCmdPipelineBarrier"));
        const auto cmd_clear_color_image = reinterpret_cast<PFN_vkCmdClearColorImage>(get_instance_proc(instance, "vkCmdClearColorImage"));
        const auto cmd_copy_image_to_buffer =
            reinterpret_cast<PFN_vkCmdCopyImageToBuffer>(get_instance_proc(instance, "vkCmdCopyImageToBuffer"));

        const auto create_command_pool = reinterpret_cast<PFN_vkCreateCommandPool>(get_instance_proc(instance, "vkCreateCommandPool"));
        const auto destroy_command_pool = reinterpret_cast<PFN_vkDestroyCommandPool>(get_instance_proc(instance, "vkDestroyCommandPool"));
        const auto allocate_command_buffers =
            reinterpret_cast<PFN_vkAllocateCommandBuffers>(get_instance_proc(instance, "vkAllocateCommandBuffers"));
        const auto begin_command_buffer = reinterpret_cast<PFN_vkBeginCommandBuffer>(get_instance_proc(instance, "vkBeginCommandBuffer"));
        const auto end_command_buffer = reinterpret_cast<PFN_vkEndCommandBuffer>(get_instance_proc(instance, "vkEndCommandBuffer"));
        const auto create_fence = reinterpret_cast<PFN_vkCreateFence>(get_instance_proc(instance, "vkCreateFence"));
        const auto destroy_fence = reinterpret_cast<PFN_vkDestroyFence>(get_instance_proc(instance, "vkDestroyFence"));
        const auto queue_submit = reinterpret_cast<PFN_vkQueueSubmit>(get_instance_proc(instance, "vkQueueSubmit"));
        const auto wait_for_fences = reinterpret_cast<PFN_vkWaitForFences>(get_instance_proc(instance, "vkWaitForFences"));

        constexpr uint32_t width = 16;
        constexpr uint32_t height = 16;
        constexpr VkDeviceSize readback_size = static_cast<VkDeviceSize>(width) * height * 4;
        // Clear color (R,G,B,A) = (1,0,0,1); R8G8B8A8_UNORM little-endian word = 0xFF0000FF.
        constexpr uint32_t expected = 0xFF0000FFu;

        VkPhysicalDeviceMemoryProperties mem_props{};
        get_memory_properties(physical_device, &mem_props);

        // --- offscreen render target image ---
        VkImageCreateInfo image_info{};
        image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        image_info.extent = {.width = width, .height = height, .depth = 1};
        image_info.mipLevels = 1;
        image_info.arrayLayers = 1;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

        VkImage image = VK_NULL_HANDLE;
        if (create_image(device, &image_info, nullptr, &image) != VK_SUCCESS)
        {
            std::printf("[shim-test] vkCreateImage failed\n");
            return false;
        }

        VkMemoryRequirements image_reqs{};
        get_image_reqs(device, image, &image_reqs);
        const uint32_t image_type = find_memory_type(mem_props, image_reqs.memoryTypeBits, 0);

        VkMemoryAllocateInfo image_alloc{};
        image_alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        image_alloc.allocationSize = image_reqs.size;
        image_alloc.memoryTypeIndex = image_type;
        VkDeviceMemory image_memory = VK_NULL_HANDLE;
        allocate_memory(device, &image_alloc, nullptr, &image_memory);
        bind_image_memory(device, image, image_memory, 0);

        // --- host-visible readback buffer ---
        VkBufferCreateInfo buffer_info{};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = readback_size;
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkBuffer buffer = VK_NULL_HANDLE;
        create_buffer(device, &buffer_info, nullptr, &buffer);

        VkMemoryRequirements buffer_reqs{};
        get_buffer_reqs(device, buffer, &buffer_reqs);
        const uint32_t buffer_type = find_memory_type(mem_props, buffer_reqs.memoryTypeBits,
                                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        VkMemoryAllocateInfo buffer_alloc{};
        buffer_alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        buffer_alloc.allocationSize = buffer_reqs.size;
        buffer_alloc.memoryTypeIndex = buffer_type;
        VkDeviceMemory buffer_memory = VK_NULL_HANDLE;
        allocate_memory(device, &buffer_alloc, nullptr, &buffer_memory);
        bind_buffer_memory(device, buffer, buffer_memory, 0);

        if (image_type == UINT32_MAX || buffer_type == UINT32_MAX)
        {
            std::printf("[shim-test] no suitable memory type for image/readback\n");
            destroy_buffer(device, buffer, nullptr);
            free_memory(device, buffer_memory, nullptr);
            destroy_image(device, image, nullptr);
            free_memory(device, image_memory, nullptr);
            return false;
        }

        // --- record: transition -> clear -> transition -> copy to buffer ---
        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.queueFamilyIndex = queue_family;
        VkCommandPool pool = VK_NULL_HANDLE;
        create_command_pool(device, &pool_info, nullptr, &pool);

        VkCommandBufferAllocateInfo cb_info{};
        cb_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cb_info.commandPool = pool;
        cb_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cb_info.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        allocate_command_buffers(device, &cb_info, &cmd);

        VkImageSubresourceRange full_range{};
        full_range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        full_range.baseMipLevel = 0;
        full_range.levelCount = 1;
        full_range.baseArrayLayer = 0;
        full_range.layerCount = 1;

        const auto transition = [&](VkImageLayout old_layout, VkImageLayout new_layout, VkAccessFlags src_access, VkAccessFlags dst_access,
                                    VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.srcAccessMask = src_access;
            barrier.dstAccessMask = dst_access;
            barrier.oldLayout = old_layout;
            barrier.newLayout = new_layout;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image;
            barrier.subresourceRange = full_range;
            cmd_pipeline_barrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        };

        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        begin_command_buffer(cmd, &begin);

        transition(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

        VkClearColorValue clear{};
        clear.float32[0] = 1.0f;
        clear.float32[1] = 0.0f;
        clear.float32[2] = 0.0f;
        clear.float32[3] = 1.0f;
        cmd_clear_color_image(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &full_range);

        transition(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {.width = width, .height = height, .depth = 1};
        cmd_copy_image_to_buffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);

        end_command_buffer(cmd);

        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence = VK_NULL_HANDLE;
        create_fence(device, &fence_info, nullptr, &fence);

        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        queue_submit(queue, 1, &submit, fence);
        const VkResult wait_result = wait_for_fences(device, 1, &fence, VK_TRUE, 30000000000ULL /* 30s */);

        bool ok = (wait_result == VK_SUCCESS);
        if (ok)
        {
            void* mapped = nullptr;
            if (map_memory(device, buffer_memory, 0, readback_size, 0, &mapped) == VK_SUCCESS && mapped)
            {
                const auto* pixels = static_cast<const uint32_t*>(mapped);
                for (uint32_t i = 0; i < width * height; ++i)
                {
                    if (pixels[i] != expected)
                    {
                        ok = false;
                        std::printf("[shim-test] clear readback mismatch at pixel %u: 0x%08X != 0x%08X\n", i, pixels[i], expected);
                        break;
                    }
                }
                unmap_memory(device, buffer_memory);
            }
            else
            {
                ok = false;
                std::printf("[shim-test] readback vkMapMemory failed\n");
            }
        }

        std::printf("[shim-test] clear+readback (%ux%u -> 0x%08X) wait=%d -> %s\n", width, height, expected, wait_result,
                    ok ? "PASS" : "FAIL");

        destroy_fence(device, fence, nullptr);
        destroy_command_pool(device, pool, nullptr);
        destroy_buffer(device, buffer, nullptr);
        free_memory(device, buffer_memory, nullptr);
        destroy_image(device, image, nullptr);
        free_memory(device, image_memory, nullptr);
        return ok;
    }

    bool test_shader_module_identifier(PFN_vkGetInstanceProcAddr get_instance_proc, VkInstance instance, VkDevice device,
                                       bool pipeline_creation_cache_control_enabled)
    {
        const auto get_device_proc = reinterpret_cast<PFN_vkGetDeviceProcAddr>(get_instance_proc(instance, "vkGetDeviceProcAddr"));
        if (!get_device_proc)
        {
            std::printf("[shim-test] no vkGetDeviceProcAddr for shader identifier test\n");
            return false;
        }

        const auto get_create_info_identifier = reinterpret_cast<PFN_vkGetShaderModuleCreateInfoIdentifierEXT>(
            get_device_proc(device, "vkGetShaderModuleCreateInfoIdentifierEXT"));
        const auto get_module_identifier =
            reinterpret_cast<PFN_vkGetShaderModuleIdentifierEXT>(get_device_proc(device, "vkGetShaderModuleIdentifierEXT"));
        const auto create_shader_module = reinterpret_cast<PFN_vkCreateShaderModule>(get_device_proc(device, "vkCreateShaderModule"));
        const auto destroy_shader_module = reinterpret_cast<PFN_vkDestroyShaderModule>(get_device_proc(device, "vkDestroyShaderModule"));
        const auto create_pipeline_layout = reinterpret_cast<PFN_vkCreatePipelineLayout>(get_device_proc(device, "vkCreatePipelineLayout"));
        const auto destroy_pipeline_layout =
            reinterpret_cast<PFN_vkDestroyPipelineLayout>(get_device_proc(device, "vkDestroyPipelineLayout"));
        const auto create_compute_pipelines =
            reinterpret_cast<PFN_vkCreateComputePipelines>(get_device_proc(device, "vkCreateComputePipelines"));
        const auto destroy_pipeline = reinterpret_cast<PFN_vkDestroyPipeline>(get_device_proc(device, "vkDestroyPipeline"));
        const auto create_pipeline_cache =
            reinterpret_cast<PFN_vkCreatePipelineCache>(get_device_proc(device, "vkCreatePipelineCache"));
        const auto destroy_pipeline_cache =
            reinterpret_cast<PFN_vkDestroyPipelineCache>(get_device_proc(device, "vkDestroyPipelineCache"));
        const auto get_pipeline_cache_data =
            reinterpret_cast<PFN_vkGetPipelineCacheData>(get_device_proc(device, "vkGetPipelineCacheData"));
        const auto merge_pipeline_caches =
            reinterpret_cast<PFN_vkMergePipelineCaches>(get_device_proc(device, "vkMergePipelineCaches"));
        if (!get_create_info_identifier || !get_module_identifier || !create_shader_module || !destroy_shader_module ||
            !create_pipeline_layout || !destroy_pipeline_layout || !create_compute_pipelines || !destroy_pipeline ||
            !create_pipeline_cache || !destroy_pipeline_cache || !get_pipeline_cache_data || !merge_pipeline_caches)
        {
            std::printf("[shim-test] shader module identifier entry point missing\n");
            return false;
        }

        // Minimal SPIR-V 1.0 compute shader: layout(local_size_x=1, local_size_y=1, local_size_z=1) in; void main() {}
        constexpr std::array<uint32_t, 42> compute_spirv{
            0x07230203, 0x00010000, 0x00000000, 0x00000005, 0x00000000, 0x00020011, 0x00000001, 0x0003000e, 0x00000000,
            0x00000001, 0x0005000f, 0x00000005, 0x00000003, 0x6e69616d, 0x00000000, 0x00060010, 0x00000003, 0x00000011,
            0x00000001, 0x00000001, 0x00000001, 0x00030003, 0x00000002, 0x000001c2, 0x00040005, 0x00000003, 0x6e69616d,
            0x00000000, 0x00020013, 0x00000001, 0x00030021, 0x00000002, 0x00000001, 0x00050036, 0x00000001, 0x00000003,
            0x00000000, 0x00000002, 0x000200f8, 0x00000004, 0x000100fd, 0x00010038,
        };

        VkShaderModuleCreateInfo shader_info{};
        shader_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        shader_info.codeSize = compute_spirv.size() * sizeof(uint32_t);
        shader_info.pCode = compute_spirv.data();

        VkShaderModuleIdentifierEXT from_create_info{};
        from_create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_IDENTIFIER_EXT;
        get_create_info_identifier(device, &shader_info, &from_create_info);

        VkShaderModule shader = VK_NULL_HANDLE;
        const VkResult shader_result = create_shader_module(device, &shader_info, nullptr, &shader);
        if (shader_result != VK_SUCCESS)
        {
            std::printf("[shim-test] shader identifier vkCreateShaderModule -> %d -> FAIL\n", shader_result);
            return false;
        }

        VkShaderModuleIdentifierEXT from_module{};
        from_module.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_IDENTIFIER_EXT;
        get_module_identifier(device, shader, &from_module);

        const bool identifiers_match =
            from_create_info.identifierSize > 0 && from_create_info.identifierSize <= VK_MAX_SHADER_MODULE_IDENTIFIER_SIZE_EXT &&
            from_create_info.identifierSize == from_module.identifierSize &&
            std::memcmp(from_create_info.identifier, from_module.identifier, from_create_info.identifierSize) == 0;

        VkPipelineLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        const VkResult layout_result = create_pipeline_layout(device, &layout_info, nullptr, &layout);

        VkPipelineCacheCreateInfo cache_info{};
        cache_info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
        VkPipelineCache pipeline_cache = VK_NULL_HANDLE;
        const VkResult cache_create_result = create_pipeline_cache(device, &cache_info, nullptr, &pipeline_cache);

        VkResult pipeline_result = VK_ERROR_INITIALIZATION_FAILED;
        VkPipeline pipeline = VK_NULL_HANDLE;
        if (layout_result == VK_SUCCESS && identifiers_match && cache_create_result == VK_SUCCESS)
        {
            VkPipelineShaderStageModuleIdentifierCreateInfoEXT identifier_info{};
            identifier_info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_MODULE_IDENTIFIER_CREATE_INFO_EXT;
            identifier_info.identifierSize = from_create_info.identifierSize;
            identifier_info.pIdentifier = from_create_info.identifier;

            VkPipelineShaderStageCreateInfo stage{};
            stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stage.pNext = &identifier_info;
            stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            stage.module = VK_NULL_HANDLE;
            stage.pName = "main";

            VkComputePipelineCreateInfo pipeline_info{};
            pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            pipeline_info.flags =
                pipeline_creation_cache_control_enabled ? VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT
                                                        : 0;
            pipeline_info.stage = stage;
            pipeline_info.layout = layout;
            pipeline_result = create_compute_pipelines(device, pipeline_cache, 1, &pipeline_info, nullptr, &pipeline);
        }

        const bool pipeline_ok = pipeline_result == VK_SUCCESS || pipeline_result == VK_PIPELINE_COMPILE_REQUIRED_EXT;

        size_t cache_data_size = 0;
        VkResult cache_query_result = VK_ERROR_INITIALIZATION_FAILED;
        VkResult cache_data_result = VK_ERROR_INITIALIZATION_FAILED;
        VkResult seeded_cache_result = VK_ERROR_INITIALIZATION_FAILED;
        VkResult merge_result = VK_ERROR_INITIALIZATION_FAILED;
        VkPipelineCache seeded_cache = VK_NULL_HANDLE;
        std::vector<uint8_t> cache_data;
        if (cache_create_result == VK_SUCCESS)
        {
            cache_query_result = get_pipeline_cache_data(device, pipeline_cache, &cache_data_size, nullptr);
            if (cache_query_result == VK_SUCCESS)
            {
                cache_data.resize(cache_data_size);
                cache_data_result = get_pipeline_cache_data(device, pipeline_cache, &cache_data_size, cache_data.data());
                cache_data.resize(cache_data_size);

                VkPipelineCacheCreateInfo seeded_info{};
                seeded_info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
                seeded_info.initialDataSize = cache_data.size();
                seeded_info.pInitialData = cache_data.empty() ? nullptr : cache_data.data();
                seeded_cache_result = create_pipeline_cache(device, &seeded_info, nullptr, &seeded_cache);
                if (seeded_cache_result == VK_SUCCESS)
                {
                    merge_result = merge_pipeline_caches(device, pipeline_cache, 1, &seeded_cache);
                }
            }
        }
        const bool cache_ok = cache_create_result == VK_SUCCESS && cache_query_result == VK_SUCCESS &&
                              cache_data_result == VK_SUCCESS && seeded_cache_result == VK_SUCCESS && merge_result == VK_SUCCESS;
        std::printf("[shim-test] shader identifiers size=%u match=%s, identifier pipeline=%d, pipeline cache=%s -> %s\n",
                    from_create_info.identifierSize, identifiers_match ? "yes" : "no", pipeline_result, cache_ok ? "yes" : "no",
                    (identifiers_match && pipeline_ok && cache_ok) ? "PASS" : "FAIL");

        if (seeded_cache != VK_NULL_HANDLE)
        {
            destroy_pipeline_cache(device, seeded_cache, nullptr);
        }
        if (pipeline_cache != VK_NULL_HANDLE)
        {
            destroy_pipeline_cache(device, pipeline_cache, nullptr);
        }
        if (pipeline != VK_NULL_HANDLE)
        {
            destroy_pipeline(device, pipeline, nullptr);
        }
        if (layout != VK_NULL_HANDLE)
        {
            destroy_pipeline_layout(device, layout, nullptr);
        }
        destroy_shader_module(device, shader, nullptr);
        return identifiers_match && pipeline_ok && cache_ok;
    }
}

int main(int argc, char** argv)
{
    const char* dll = (argc > 1) ? argv[1] : "vulkan-shim.dll";
    std::printf("[shim-test] loading %s\n", dll);

    const HMODULE mod = LoadLibraryA(dll);
    if (!mod)
    {
        std::printf("[shim-test] LoadLibrary failed: %lu\n", GetLastError());
        return 1;
    }

    const auto get_instance_proc =
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(reinterpret_cast<void*>(GetProcAddress(mod, "vkGetInstanceProcAddr")));
    if (!get_instance_proc)
    {
        std::printf("[shim-test] no vkGetInstanceProcAddr export\n");
        return 2;
    }

    const auto write_timestamp2 = reinterpret_cast<PFN_vkCmdWriteTimestamp2>(get_instance_proc(nullptr, "vkCmdWriteTimestamp2"));
    const auto write_timestamp2_khr = reinterpret_cast<PFN_vkCmdWriteTimestamp2KHR>(get_instance_proc(nullptr, "vkCmdWriteTimestamp2KHR"));
    const auto draw_indirect_count_khr =
        reinterpret_cast<PFN_vkCmdDrawIndirectCountKHR>(get_instance_proc(nullptr, "vkCmdDrawIndirectCountKHR"));
    const auto draw_indexed_indirect_count_khr =
        reinterpret_cast<PFN_vkCmdDrawIndexedIndirectCountKHR>(get_instance_proc(nullptr, "vkCmdDrawIndexedIndirectCountKHR"));
    if (!write_timestamp2 || !write_timestamp2_khr || !draw_indirect_count_khr || !draw_indexed_indirect_count_khr)
    {
        std::printf("[shim-test] promoted command alias missing\n");
        return 3;
    }

    const auto create_instance = reinterpret_cast<PFN_vkCreateInstance>(get_instance_proc(nullptr, "vkCreateInstance"));
    if (!create_instance)
    {
        std::printf("[shim-test] no vkCreateInstance\n");
        return 3;
    }

    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &app_info;

    VkInstance instance = VK_NULL_HANDLE;
    VkResult result = create_instance(&create_info, nullptr, &instance);
    std::printf("[shim-test] vkCreateInstance -> %d, instance=%p\n", result, static_cast<void*>(instance));
    if (result != VK_SUCCESS || instance == VK_NULL_HANDLE)
    {
        return 4;
    }

    const auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(get_instance_proc(instance, "vkEnumeratePhysicalDevices"));
    const auto get_properties =
        reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(get_instance_proc(instance, "vkGetPhysicalDeviceProperties"));
    const auto destroy_instance = reinterpret_cast<PFN_vkDestroyInstance>(get_instance_proc(instance, "vkDestroyInstance"));

    bool shader_identifier_test_ok = true;
    bool timestamp2_test_ok = true;

    uint32_t count = 0;
    result = enumerate(instance, &count, nullptr);
    std::printf("[shim-test] vkEnumeratePhysicalDevices -> %d, count=%u\n", result, count);

    if (count > 0 && get_properties)
    {
        std::vector<VkPhysicalDevice> devices(count);
        enumerate(instance, &count, devices.data());

        for (uint32_t i = 0; i < count; ++i)
        {
            VkPhysicalDeviceProperties props{};
            get_properties(devices[i], &props);
            std::printf("[shim-test] device[%u]: '%s' type=%u api=%u.%u.%u\n", i, props.deviceName, props.deviceType,
                        VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
                        VK_API_VERSION_PATCH(props.apiVersion));
        }

        // Create a logical device on the first physical device, using a graphics-capable queue family.
        const auto get_queue_families = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
            get_instance_proc(instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
        const auto create_device = reinterpret_cast<PFN_vkCreateDevice>(get_instance_proc(instance, "vkCreateDevice"));
        const auto get_device_queue = reinterpret_cast<PFN_vkGetDeviceQueue>(get_instance_proc(instance, "vkGetDeviceQueue"));
        const auto destroy_device = reinterpret_cast<PFN_vkDestroyDevice>(get_instance_proc(instance, "vkDestroyDevice"));
        const auto enumerate_device_extensions =
            reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(get_instance_proc(instance, "vkEnumerateDeviceExtensionProperties"));
        const auto get_features2 =
            reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(get_instance_proc(instance, "vkGetPhysicalDeviceFeatures2"));
        const auto get_properties2 =
            reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(get_instance_proc(instance, "vkGetPhysicalDeviceProperties2"));

        bool identifier_extension_present = false;
        if (enumerate_device_extensions)
        {
            uint32_t extension_count = 0;
            enumerate_device_extensions(devices[0], nullptr, &extension_count, nullptr);
            std::vector<VkExtensionProperties> extensions(extension_count);
            enumerate_device_extensions(devices[0], nullptr, &extension_count, extensions.data());

            for (const auto& extension : extensions)
            {
                if (std::strcmp(extension.extensionName, VK_EXT_SHADER_MODULE_IDENTIFIER_EXTENSION_NAME) == 0)
                {
                    identifier_extension_present = true;
                    break;
                }
            }
        }

        // Both tested operations consume Vulkan 1.3 feature bits. Query them once, then enable only
        // the bits used by tests that can actually run on this physical device.
        VkPhysicalDeviceVulkan13Features available_vulkan13_features{};
        available_vulkan13_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        VkPhysicalDeviceShaderModuleIdentifierFeaturesEXT available_identifier_features{};
        available_identifier_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MODULE_IDENTIFIER_FEATURES_EXT;
        if (identifier_extension_present)
        {
            available_vulkan13_features.pNext = &available_identifier_features;
        }

        if (get_features2)
        {
            VkPhysicalDeviceFeatures2 features2{};
            features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            features2.pNext = &available_vulkan13_features;
            get_features2(devices[0], &features2);
        }

        VkPhysicalDeviceShaderModuleIdentifierPropertiesEXT identifier_properties{};
        identifier_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MODULE_IDENTIFIER_PROPERTIES_EXT;
        if (identifier_extension_present && get_properties2)
        {
            VkPhysicalDeviceProperties2 properties2{};
            properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            properties2.pNext = &identifier_properties;
            get_properties2(devices[0], &properties2);
        }

        const bool shader_identifier_supported =
            identifier_extension_present && available_identifier_features.shaderModuleIdentifier == VK_TRUE;
        const bool synchronization2_supported = available_vulkan13_features.synchronization2 == VK_TRUE;
        const bool pipeline_creation_cache_control_supported =
            available_vulkan13_features.pipelineCreationCacheControl == VK_TRUE;
        const bool pipeline_creation_cache_control_enabled =
            shader_identifier_supported && pipeline_creation_cache_control_supported;
        std::printf("[shim-test] %s advertised=%s feature=%u algorithm UUID=%02X%02X%02X%02X...\n",
                    VK_EXT_SHADER_MODULE_IDENTIFIER_EXTENSION_NAME, identifier_extension_present ? "yes" : "no",
                    available_identifier_features.shaderModuleIdentifier,
                    identifier_properties.shaderModuleIdentifierAlgorithmUUID[0],
                    identifier_properties.shaderModuleIdentifierAlgorithmUUID[1],
                    identifier_properties.shaderModuleIdentifierAlgorithmUUID[2],
                    identifier_properties.shaderModuleIdentifierAlgorithmUUID[3]);
        std::printf("[shim-test] Vulkan 1.3 features: synchronization2=%u pipelineCreationCacheControl=%u\n",
                    available_vulkan13_features.synchronization2,
                    available_vulkan13_features.pipelineCreationCacheControl);

        uint32_t family_count = 0;
        get_queue_families(devices[0], &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        get_queue_families(devices[0], &family_count, families.data());

        uint32_t graphics_family = UINT32_MAX;
        uint32_t timestamp_family = UINT32_MAX;
        for (uint32_t i = 0; i < family_count; ++i)
        {
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && graphics_family == UINT32_MAX)
            {
                graphics_family = i;
            }
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && families[i].timestampValidBits != 0 &&
                timestamp_family == UINT32_MAX)
            {
                timestamp_family = i;
            }
        }
        if (timestamp_family == UINT32_MAX)
        {
            // COPY and RESOLVE are transfer stages, so use only a command-capable family for the fallback.
            constexpr VkQueueFlags command_queue_flags =
                VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
            for (uint32_t i = 0; i < family_count; ++i)
            {
                if ((families[i].queueFlags & command_queue_flags) && families[i].timestampValidBits != 0)
                {
                    timestamp_family = i;
                    break;
                }
            }
        }
        const bool timestamp2_test_supported = synchronization2_supported && timestamp_family != UINT32_MAX;
        std::printf("[shim-test] queue families=%u, graphics family=%u, timestamp family=%u\n", family_count,
                    graphics_family, timestamp_family);

        if (graphics_family != UINT32_MAX)
        {
            const float priority = 1.0f;
            std::array<VkDeviceQueueCreateInfo, 2> queue_infos{};
            queue_infos[0].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queue_infos[0].queueFamilyIndex = graphics_family;
            queue_infos[0].queueCount = 1;
            queue_infos[0].pQueuePriorities = &priority;
            uint32_t queue_info_count = 1;
            if (timestamp2_test_supported && timestamp_family != graphics_family)
            {
                queue_infos[1].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
                queue_infos[1].queueFamilyIndex = timestamp_family;
                queue_infos[1].queueCount = 1;
                queue_infos[1].pQueuePriorities = &priority;
                queue_info_count = 2;
            }

            VkPhysicalDeviceShaderModuleIdentifierFeaturesEXT identifier_features{};
            identifier_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MODULE_IDENTIFIER_FEATURES_EXT;
            identifier_features.shaderModuleIdentifier = shader_identifier_supported ? VK_TRUE : VK_FALSE;
            const char* identifier_extension = VK_EXT_SHADER_MODULE_IDENTIFIER_EXTENSION_NAME;

            VkPhysicalDeviceVulkan13Features enabled_vulkan13_features{};
            enabled_vulkan13_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
            enabled_vulkan13_features.synchronization2 = timestamp2_test_supported ? VK_TRUE : VK_FALSE;
            enabled_vulkan13_features.pipelineCreationCacheControl =
                pipeline_creation_cache_control_enabled ? VK_TRUE : VK_FALSE;

            void* enabled_feature_chain = nullptr;
            if (shader_identifier_supported)
            {
                identifier_features.pNext = enabled_feature_chain;
                enabled_feature_chain = &identifier_features;
            }
            if (enabled_vulkan13_features.synchronization2 || enabled_vulkan13_features.pipelineCreationCacheControl)
            {
                enabled_vulkan13_features.pNext = enabled_feature_chain;
                enabled_feature_chain = &enabled_vulkan13_features;
            }

            VkDeviceCreateInfo device_info{};
            device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
            device_info.queueCreateInfoCount = queue_info_count;
            device_info.pQueueCreateInfos = queue_infos.data();
            device_info.pNext = enabled_feature_chain;
            if (shader_identifier_supported)
            {
                device_info.enabledExtensionCount = 1;
                device_info.ppEnabledExtensionNames = &identifier_extension;
            }

            VkDevice device = VK_NULL_HANDLE;
            const VkResult device_result = create_device(devices[0], &device_info, nullptr, &device);
            std::printf("[shim-test] vkCreateDevice -> %d, device=%p\n", device_result, static_cast<void*>(device));

            if (device_result == VK_SUCCESS && device != VK_NULL_HANDLE)
            {
                VkQueue queue = VK_NULL_HANDLE;
                get_device_queue(device, graphics_family, 0, &queue);
                std::printf("[shim-test] vkGetDeviceQueue -> queue=%p\n", static_cast<void*>(queue));

                if (timestamp2_test_supported)
                {
                    timestamp2_test_ok = test_write_timestamp2(get_instance_proc, instance, device, timestamp_family,
                                                               write_timestamp2, write_timestamp2_khr);
                }
                else if (!synchronization2_supported)
                {
                    std::printf("[shim-test] timestamp2 stage2-only recording -> SKIP (synchronization2 unavailable)\n");
                }
                else
                {
                    std::printf("[shim-test] timestamp2 stage2-only recording -> SKIP (no timestamp-capable queue family)\n");
                }
                submit_and_wait(get_instance_proc, instance, device, queue, graphics_family);
                fill_buffer_and_readback(get_instance_proc, instance, devices[0], device, queue, graphics_family);
                clear_image_and_readback(get_instance_proc, instance, devices[0], device, queue, graphics_family);
                if (shader_identifier_supported)
                {
                    shader_identifier_test_ok = test_shader_module_identifier(
                        get_instance_proc, instance, device, pipeline_creation_cache_control_enabled);
                }

                destroy_device(device, nullptr);
            }
        }
    }

    if (destroy_instance)
    {
        destroy_instance(instance, nullptr);
    }

    const bool all_ok = shader_identifier_test_ok && timestamp2_test_ok;
    std::printf("[shim-test] %s\n", all_ok ? "ok" : "FAILED");
    return all_ok ? 0 : 6;
}
