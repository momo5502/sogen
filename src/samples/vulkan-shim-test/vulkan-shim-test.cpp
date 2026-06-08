// Exercises the guest Vulkan shim (vulkan-shim.dll) through the genuine Vulkan loading dance --
// LoadLibrary + vkGetInstanceProcAddr -- exactly as a real application or the Khronos loader would.
// The shim forwards each call across the Sogen GPU bridge to the host driver, so this enumerates
// the host's physical devices end-to-end: guest app -> shim -> bridge -> host Vulkan -> GPU.

#include <windows.h>

#include <cstdio>
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
        const auto create_command_pool =
            reinterpret_cast<PFN_vkCreateCommandPool>(get_instance_proc(instance, "vkCreateCommandPool"));
        const auto destroy_command_pool =
            reinterpret_cast<PFN_vkDestroyCommandPool>(get_instance_proc(instance, "vkDestroyCommandPool"));
        const auto allocate_command_buffers =
            reinterpret_cast<PFN_vkAllocateCommandBuffers>(get_instance_proc(instance, "vkAllocateCommandBuffers"));
        const auto begin_command_buffer =
            reinterpret_cast<PFN_vkBeginCommandBuffer>(get_instance_proc(instance, "vkBeginCommandBuffer"));
        const auto end_command_buffer =
            reinterpret_cast<PFN_vkEndCommandBuffer>(get_instance_proc(instance, "vkEndCommandBuffer"));
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

    // Picks the first memory type that is set in `type_bits` and carries all of `required` property
    // flags. Returns UINT32_MAX if none qualifies.
    uint32_t find_memory_type(const VkPhysicalDeviceMemoryProperties& props, uint32_t type_bits,
                              VkMemoryPropertyFlags required)
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
    bool fill_buffer_and_readback(PFN_vkGetInstanceProcAddr get_instance_proc, VkInstance instance,
                                  VkPhysicalDevice physical_device, VkDevice device, VkQueue queue, uint32_t queue_family)
    {
        const auto get_memory_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
            get_instance_proc(instance, "vkGetPhysicalDeviceMemoryProperties"));
        const auto create_buffer = reinterpret_cast<PFN_vkCreateBuffer>(get_instance_proc(instance, "vkCreateBuffer"));
        const auto destroy_buffer = reinterpret_cast<PFN_vkDestroyBuffer>(get_instance_proc(instance, "vkDestroyBuffer"));
        const auto get_buffer_reqs = reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(
            get_instance_proc(instance, "vkGetBufferMemoryRequirements"));
        const auto allocate_memory = reinterpret_cast<PFN_vkAllocateMemory>(get_instance_proc(instance, "vkAllocateMemory"));
        const auto free_memory = reinterpret_cast<PFN_vkFreeMemory>(get_instance_proc(instance, "vkFreeMemory"));
        const auto bind_buffer_memory =
            reinterpret_cast<PFN_vkBindBufferMemory>(get_instance_proc(instance, "vkBindBufferMemory"));
        const auto map_memory = reinterpret_cast<PFN_vkMapMemory>(get_instance_proc(instance, "vkMapMemory"));
        const auto unmap_memory = reinterpret_cast<PFN_vkUnmapMemory>(get_instance_proc(instance, "vkUnmapMemory"));
        const auto cmd_fill_buffer = reinterpret_cast<PFN_vkCmdFillBuffer>(get_instance_proc(instance, "vkCmdFillBuffer"));

        const auto create_command_pool =
            reinterpret_cast<PFN_vkCreateCommandPool>(get_instance_proc(instance, "vkCreateCommandPool"));
        const auto destroy_command_pool =
            reinterpret_cast<PFN_vkDestroyCommandPool>(get_instance_proc(instance, "vkDestroyCommandPool"));
        const auto allocate_command_buffers =
            reinterpret_cast<PFN_vkAllocateCommandBuffers>(get_instance_proc(instance, "vkAllocateCommandBuffers"));
        const auto begin_command_buffer =
            reinterpret_cast<PFN_vkBeginCommandBuffer>(get_instance_proc(instance, "vkBeginCommandBuffer"));
        const auto end_command_buffer =
            reinterpret_cast<PFN_vkEndCommandBuffer>(get_instance_proc(instance, "vkEndCommandBuffer"));
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
        const uint32_t type_index = find_memory_type(
            mem_props, reqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
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

        std::printf("[shim-test] fill+readback (0x%08X x%u) -> %s\n", fill_value,
                    static_cast<uint32_t>(buffer_size / sizeof(uint32_t)), ok ? "PASS" : "FAIL");

        destroy_fence(device, fence, nullptr);
        destroy_command_pool(device, pool, nullptr);
        free_memory(device, memory, nullptr);
        destroy_buffer(device, buffer, nullptr);
        return ok;
    }

    // Renders into an image the simplest possible way -- clears it to a known color on the GPU -- then
    // copies it into a host-visible buffer and reads the pixels back. This is the offscreen render
    // target + readback path that windowed present will reuse (present = clear/draw -> copy -> readback).
    bool clear_image_and_readback(PFN_vkGetInstanceProcAddr get_instance_proc, VkInstance instance,
                                  VkPhysicalDevice physical_device, VkDevice device, VkQueue queue, uint32_t queue_family)
    {
        const auto get_memory_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
            get_instance_proc(instance, "vkGetPhysicalDeviceMemoryProperties"));
        const auto create_image = reinterpret_cast<PFN_vkCreateImage>(get_instance_proc(instance, "vkCreateImage"));
        const auto destroy_image = reinterpret_cast<PFN_vkDestroyImage>(get_instance_proc(instance, "vkDestroyImage"));
        const auto get_image_reqs = reinterpret_cast<PFN_vkGetImageMemoryRequirements>(
            get_instance_proc(instance, "vkGetImageMemoryRequirements"));
        const auto bind_image_memory =
            reinterpret_cast<PFN_vkBindImageMemory>(get_instance_proc(instance, "vkBindImageMemory"));
        const auto create_buffer = reinterpret_cast<PFN_vkCreateBuffer>(get_instance_proc(instance, "vkCreateBuffer"));
        const auto destroy_buffer = reinterpret_cast<PFN_vkDestroyBuffer>(get_instance_proc(instance, "vkDestroyBuffer"));
        const auto get_buffer_reqs = reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(
            get_instance_proc(instance, "vkGetBufferMemoryRequirements"));
        const auto allocate_memory = reinterpret_cast<PFN_vkAllocateMemory>(get_instance_proc(instance, "vkAllocateMemory"));
        const auto free_memory = reinterpret_cast<PFN_vkFreeMemory>(get_instance_proc(instance, "vkFreeMemory"));
        const auto bind_buffer_memory =
            reinterpret_cast<PFN_vkBindBufferMemory>(get_instance_proc(instance, "vkBindBufferMemory"));
        const auto map_memory = reinterpret_cast<PFN_vkMapMemory>(get_instance_proc(instance, "vkMapMemory"));
        const auto unmap_memory = reinterpret_cast<PFN_vkUnmapMemory>(get_instance_proc(instance, "vkUnmapMemory"));
        const auto cmd_pipeline_barrier =
            reinterpret_cast<PFN_vkCmdPipelineBarrier>(get_instance_proc(instance, "vkCmdPipelineBarrier"));
        const auto cmd_clear_color_image =
            reinterpret_cast<PFN_vkCmdClearColorImage>(get_instance_proc(instance, "vkCmdClearColorImage"));
        const auto cmd_copy_image_to_buffer =
            reinterpret_cast<PFN_vkCmdCopyImageToBuffer>(get_instance_proc(instance, "vkCmdCopyImageToBuffer"));

        const auto create_command_pool =
            reinterpret_cast<PFN_vkCreateCommandPool>(get_instance_proc(instance, "vkCreateCommandPool"));
        const auto destroy_command_pool =
            reinterpret_cast<PFN_vkDestroyCommandPool>(get_instance_proc(instance, "vkDestroyCommandPool"));
        const auto allocate_command_buffers =
            reinterpret_cast<PFN_vkAllocateCommandBuffers>(get_instance_proc(instance, "vkAllocateCommandBuffers"));
        const auto begin_command_buffer =
            reinterpret_cast<PFN_vkBeginCommandBuffer>(get_instance_proc(instance, "vkBeginCommandBuffer"));
        const auto end_command_buffer =
            reinterpret_cast<PFN_vkEndCommandBuffer>(get_instance_proc(instance, "vkEndCommandBuffer"));
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
        const uint32_t buffer_type = find_memory_type(
            mem_props, buffer_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

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

        const auto transition = [&](VkImageLayout old_layout, VkImageLayout new_layout, VkAccessFlags src_access,
                                    VkAccessFlags dst_access, VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
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
                        std::printf("[shim-test] clear readback mismatch at pixel %u: 0x%08X != 0x%08X\n", i, pixels[i],
                                    expected);
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

    const auto enumerate =
        reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(get_instance_proc(instance, "vkEnumeratePhysicalDevices"));
    const auto get_properties =
        reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(get_instance_proc(instance, "vkGetPhysicalDeviceProperties"));
    const auto destroy_instance = reinterpret_cast<PFN_vkDestroyInstance>(get_instance_proc(instance, "vkDestroyInstance"));

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

        uint32_t family_count = 0;
        get_queue_families(devices[0], &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        get_queue_families(devices[0], &family_count, families.data());

        uint32_t graphics_family = UINT32_MAX;
        for (uint32_t i = 0; i < family_count; ++i)
        {
            if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
            {
                graphics_family = i;
                break;
            }
        }
        std::printf("[shim-test] queue families=%u, graphics family=%u\n", family_count, graphics_family);

        if (graphics_family != UINT32_MAX)
        {
            const float priority = 1.0f;
            VkDeviceQueueCreateInfo queue_info{};
            queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queue_info.queueFamilyIndex = graphics_family;
            queue_info.queueCount = 1;
            queue_info.pQueuePriorities = &priority;

            VkDeviceCreateInfo device_info{};
            device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
            device_info.queueCreateInfoCount = 1;
            device_info.pQueueCreateInfos = &queue_info;

            VkDevice device = VK_NULL_HANDLE;
            const VkResult device_result = create_device(devices[0], &device_info, nullptr, &device);
            std::printf("[shim-test] vkCreateDevice -> %d, device=%p\n", device_result, static_cast<void*>(device));

            if (device_result == VK_SUCCESS && device != VK_NULL_HANDLE)
            {
                VkQueue queue = VK_NULL_HANDLE;
                get_device_queue(device, graphics_family, 0, &queue);
                std::printf("[shim-test] vkGetDeviceQueue -> queue=%p\n", static_cast<void*>(queue));

                submit_and_wait(get_instance_proc, instance, device, queue, graphics_family);
                fill_buffer_and_readback(get_instance_proc, instance, devices[0], device, queue, graphics_family);
                clear_image_and_readback(get_instance_proc, instance, devices[0], device, queue, graphics_family);

                destroy_device(device, nullptr);
            }
        }
    }

    if (destroy_instance)
    {
        destroy_instance(instance, nullptr);
    }

    std::printf("[shim-test] ok\n");
    return 0;
}
