#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <vulkan/vulkan_core.h>

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <vector>

// Keeps GPU state alive across iterations and prints a checksum per iteration, so that a run suspended and resumed in
// the middle can be compared with an uninterrupted one. Between two iterations, where a suspend lands, GPU memory holds
// the previous iteration's results, a binary semaphore holds a signal the next iteration waits for, and a command buffer
// recorded once before the loop is waiting to be submitted again. A buffer in memory the host cannot map carries the
// results of the iteration before the previous one, and so does an image that rests in a shader-read layout.
namespace
{
    constexpr VkDeviceSize half_size = 16 * 1024;
    constexpr VkDeviceSize buffer_size = 2 * half_size;
    constexpr size_t half_count = half_size / sizeof(uint32_t);
    constexpr uint32_t iterations = 40;
    constexpr uint32_t image_size = 64;
    static_assert(image_size * image_size * 4 == half_size);

    uint32_t source_value(const uint32_t iteration)
    {
        return 0x1000193u * (iteration + 1);
    }

    uint32_t fill_value(const uint32_t iteration)
    {
        return source_value(iteration) ^ 0x5A5A5A5Au;
    }

    uint64_t checksum(const uint32_t* data, const size_t count)
    {
        uint64_t hash = 14695981039346656037ULL;
        for (size_t i = 0; i < count; ++i)
        {
            hash = (hash ^ data[i]) * 1099511628211ULL;
        }
        return hash;
    }

    bool holds_fill(const uint32_t* data, const uint32_t iteration)
    {
        for (size_t i = 0; i < half_count; ++i)
        {
            if (data[i] != fill_value(iteration))
            {
                return false;
            }
        }
        return true;
    }

    bool holds_iteration(const uint32_t* data, const uint32_t iteration)
    {
        for (size_t i = 0; i < half_count; ++i)
        {
            if (data[i] != fill_value(iteration) || data[half_count + i] != source_value(iteration))
            {
                return false;
            }
        }
        return true;
    }

    uint32_t find_queue_family(const std::vector<VkQueueFamilyProperties>& families)
    {
        for (uint32_t i = 0; i < families.size(); ++i)
        {
            if ((families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT)) != 0)
            {
                return i;
            }
        }
        return UINT32_MAX;
    }

    uint32_t find_memory_type(const VkPhysicalDeviceMemoryProperties& properties, const uint32_t type_bits,
                              const VkMemoryPropertyFlags wanted, const VkMemoryPropertyFlags unwanted)
    {
        for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
        {
            const auto flags = properties.memoryTypes[i].propertyFlags;
            if ((type_bits & (1U << i)) != 0 && (flags & wanted) == wanted && (flags & unwanted) == 0)
            {
                return i;
            }
        }
        return UINT32_MAX;
    }
}

int main(const int argc, char** argv)
{
    const char* dll = argc > 1 ? argv[1] : "vulkan-shim.dll";
    const HMODULE module = LoadLibraryA(dll);
    if (!module)
    {
        std::printf("[snapshot-test] LoadLibrary(%s) failed: %lu\n", dll, GetLastError());
        return 1;
    }

    const auto get_instance_proc =
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(reinterpret_cast<void*>(GetProcAddress(module, "vkGetInstanceProcAddr")));
    if (!get_instance_proc)
    {
        std::printf("[snapshot-test] no vkGetInstanceProcAddr\n");
        return 1;
    }

#define LOAD(instance, name) const auto name = reinterpret_cast<PFN_##name>(get_instance_proc(instance, #name))

    LOAD(nullptr, vkCreateInstance);

    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo instance_info{};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &app_info;

    VkInstance instance{};
    if (vkCreateInstance(&instance_info, nullptr, &instance) != VK_SUCCESS)
    {
        std::printf("[snapshot-test] vkCreateInstance failed\n");
        return 1;
    }

    LOAD(instance, vkEnumeratePhysicalDevices);
    LOAD(instance, vkGetPhysicalDeviceQueueFamilyProperties);
    LOAD(instance, vkGetPhysicalDeviceMemoryProperties);
    LOAD(instance, vkCreateDevice);
    LOAD(instance, vkGetDeviceQueue);
    LOAD(instance, vkCreateBuffer);
    LOAD(instance, vkGetBufferMemoryRequirements);
    LOAD(instance, vkAllocateMemory);
    LOAD(instance, vkBindBufferMemory);
    LOAD(instance, vkMapMemory);
    LOAD(instance, vkCreateCommandPool);
    LOAD(instance, vkAllocateCommandBuffers);
    LOAD(instance, vkBeginCommandBuffer);
    LOAD(instance, vkEndCommandBuffer);
    LOAD(instance, vkCmdFillBuffer);
    LOAD(instance, vkCmdCopyBuffer);
    LOAD(instance, vkCreateImage);
    LOAD(instance, vkGetImageMemoryRequirements);
    LOAD(instance, vkBindImageMemory);
    LOAD(instance, vkCmdPipelineBarrier);
    LOAD(instance, vkCmdCopyImageToBuffer);
    LOAD(instance, vkCmdCopyBufferToImage);
    LOAD(instance, vkCreateFence);
    LOAD(instance, vkCreateSemaphore);
    LOAD(instance, vkQueueSubmit);
    LOAD(instance, vkWaitForFences);
    LOAD(instance, vkResetFences);

    uint32_t device_count = 1;
    VkPhysicalDevice physical_device{};
    if (vkEnumeratePhysicalDevices(instance, &device_count, &physical_device) < 0 || device_count == 0)
    {
        std::printf("[snapshot-test] no physical device\n");
        return 1;
    }

    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &family_count, families.data());

    const uint32_t family = find_queue_family(families);
    if (family == UINT32_MAX)
    {
        std::printf("[snapshot-test] no transfer queue family\n");
        return 1;
    }

    const float priority = 1.0F;
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;

    VkDeviceCreateInfo device_info{};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;

    VkDevice device{};
    if (vkCreateDevice(physical_device, &device_info, nullptr, &device) != VK_SUCCESS)
    {
        std::printf("[snapshot-test] vkCreateDevice failed\n");
        return 1;
    }

    VkQueue queue{};
    vkGetDeviceQueue(device, family, 0, &queue);

    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = buffer_size;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer source{};
    VkBuffer destination{};
    VkBuffer mirror{};
    VkBuffer device_local{};
    VkBuffer image_readback{};
    if (vkCreateBuffer(device, &buffer_info, nullptr, &image_readback) != VK_SUCCESS ||
        vkCreateBuffer(device, &buffer_info, nullptr, &source) != VK_SUCCESS ||
        vkCreateBuffer(device, &buffer_info, nullptr, &destination) != VK_SUCCESS ||
        vkCreateBuffer(device, &buffer_info, nullptr, &mirror) != VK_SUCCESS ||
        vkCreateBuffer(device, &buffer_info, nullptr, &device_local) != VK_SUCCESS)
    {
        std::printf("[snapshot-test] vkCreateBuffer failed\n");
        return 1;
    }

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, source, &requirements);
    const VkDeviceSize stride = (requirements.size + requirements.alignment - 1) / requirements.alignment * requirements.alignment;

    VkPhysicalDeviceMemoryProperties memory_properties{};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);

    VkMemoryAllocateInfo allocate_info{};
    allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate_info.allocationSize = 4 * stride;
    allocate_info.memoryTypeIndex = find_memory_type(memory_properties, requirements.memoryTypeBits,
                                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);

    VkMemoryAllocateInfo device_local_info{};
    device_local_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    device_local_info.allocationSize = stride;
    device_local_info.memoryTypeIndex = find_memory_type(memory_properties, requirements.memoryTypeBits,
                                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (allocate_info.memoryTypeIndex == UINT32_MAX || device_local_info.memoryTypeIndex == UINT32_MAX)
    {
        std::printf("[snapshot-test] missing a host-visible coherent or a device-local memory type\n");
        return 1;
    }

    VkDeviceMemory memory{};
    VkDeviceMemory device_local_memory{};
    if (vkAllocateMemory(device, &allocate_info, nullptr, &memory) != VK_SUCCESS ||
        vkAllocateMemory(device, &device_local_info, nullptr, &device_local_memory) != VK_SUCCESS ||
        vkBindBufferMemory(device, device_local, device_local_memory, 0) != VK_SUCCESS ||
        vkBindBufferMemory(device, source, memory, 0) != VK_SUCCESS ||
        vkBindBufferMemory(device, destination, memory, stride) != VK_SUCCESS ||
        vkBindBufferMemory(device, mirror, memory, 2 * stride) != VK_SUCCESS ||
        vkBindBufferMemory(device, image_readback, memory, 3 * stride) != VK_SUCCESS)
    {
        std::printf("[snapshot-test] memory setup failed\n");
        return 1;
    }

    void* mapped = nullptr;
    if (vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS || !mapped)
    {
        std::printf("[snapshot-test] vkMapMemory failed\n");
        return 1;
    }

    auto* source_data = static_cast<uint32_t*>(mapped);
    const auto* destination_data = reinterpret_cast<const uint32_t*>(static_cast<uint8_t*>(mapped) + stride);
    const auto* mirror_data = reinterpret_cast<const uint32_t*>(static_cast<uint8_t*>(mapped) + (2 * stride));
    const auto* image_data = reinterpret_cast<const uint32_t*>(static_cast<uint8_t*>(mapped) + (3 * stride));

    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_info.extent = {.width = image_size, .height = image_size, .depth = 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage image{};
    VkMemoryRequirements image_requirements{};
    VkDeviceMemory image_memory{};
    if (vkCreateImage(device, &image_info, nullptr, &image) != VK_SUCCESS)
    {
        std::printf("[snapshot-test] vkCreateImage failed\n");
        return 1;
    }

    vkGetImageMemoryRequirements(device, image, &image_requirements);
    VkMemoryAllocateInfo image_allocate_info{};
    image_allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    image_allocate_info.allocationSize = image_requirements.size;
    image_allocate_info.memoryTypeIndex =
        find_memory_type(memory_properties, image_requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    if (image_allocate_info.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory(device, &image_allocate_info, nullptr, &image_memory) != VK_SUCCESS ||
        vkBindImageMemory(device, image, image_memory, 0) != VK_SUCCESS)
    {
        std::printf("[snapshot-test] image memory setup failed\n");
        return 1;
    }

    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = family;

    VkCommandPool pool{};
    if (vkCreateCommandPool(device, &pool_info, nullptr, &pool) != VK_SUCCESS)
    {
        std::printf("[snapshot-test] vkCreateCommandPool failed\n");
        return 1;
    }

    VkCommandBufferAllocateInfo command_buffer_info{};
    command_buffer_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    command_buffer_info.commandPool = pool;
    command_buffer_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_buffer_info.commandBufferCount = 1;

    VkCommandBuffer command_buffer{};
    VkCommandBuffer mirror_command_buffer{};
    VkCommandBuffer keep_command_buffer{};
    VkCommandBuffer image_command_buffer{};
    VkCommandBuffer setup_command_buffer{};
    if (vkAllocateCommandBuffers(device, &command_buffer_info, &image_command_buffer) != VK_SUCCESS ||
        vkAllocateCommandBuffers(device, &command_buffer_info, &setup_command_buffer) != VK_SUCCESS ||
        vkAllocateCommandBuffers(device, &command_buffer_info, &command_buffer) != VK_SUCCESS ||
        vkAllocateCommandBuffers(device, &command_buffer_info, &mirror_command_buffer) != VK_SUCCESS ||
        vkAllocateCommandBuffers(device, &command_buffer_info, &keep_command_buffer) != VK_SUCCESS)
    {
        std::printf("[snapshot-test] vkAllocateCommandBuffers failed\n");
        return 1;
    }

    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkSemaphoreCreateInfo semaphore_info{};
    semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFence fence{};
    VkSemaphore results_ready{};
    if (vkCreateFence(device, &fence_info, nullptr, &fence) != VK_SUCCESS ||
        vkCreateSemaphore(device, &semaphore_info, nullptr, &results_ready) != VK_SUCCESS)
    {
        std::printf("[snapshot-test] synchronization setup failed\n");
        return 1;
    }

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    const VkBufferCopy whole_buffer{.srcOffset = 0, .dstOffset = 0, .size = buffer_size};
    vkBeginCommandBuffer(mirror_command_buffer, &begin_info);
    vkCmdCopyBuffer(mirror_command_buffer, device_local, mirror, 1, &whole_buffer);
    vkEndCommandBuffer(mirror_command_buffer);

    vkBeginCommandBuffer(keep_command_buffer, &begin_info);
    vkCmdCopyBuffer(keep_command_buffer, destination, device_local, 1, &whole_buffer);
    vkEndCommandBuffer(keep_command_buffer);

    const auto transition = [&](const VkCommandBuffer recorded, const VkImageLayout from, const VkImageLayout to) {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.oldLayout = from;
        barrier.newLayout = to;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
        vkCmdPipelineBarrier(recorded, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &barrier);
    };

    VkBufferImageCopy image_region{};
    image_region.imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1};
    image_region.imageExtent = {.width = image_size, .height = image_size, .depth = 1};

    vkBeginCommandBuffer(setup_command_buffer, &begin_info);
    transition(setup_command_buffer, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vkEndCommandBuffer(setup_command_buffer);

    vkBeginCommandBuffer(image_command_buffer, &begin_info);
    transition(image_command_buffer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vkCmdCopyImageToBuffer(image_command_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image_readback, 1, &image_region);
    transition(image_command_buffer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyBufferToImage(image_command_buffer, destination, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &image_region);
    transition(image_command_buffer, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vkEndCommandBuffer(image_command_buffer);

    const auto submit_and_wait = [&](const VkCommandBuffer submitted, const VkSemaphore wait, const VkSemaphore signal) {
        constexpr VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submit_info{};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.waitSemaphoreCount = wait ? 1 : 0;
        submit_info.pWaitSemaphores = &wait;
        submit_info.pWaitDstStageMask = &wait_stage;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &submitted;
        submit_info.signalSemaphoreCount = signal ? 1 : 0;
        submit_info.pSignalSemaphores = &signal;
        return vkQueueSubmit(queue, 1, &submit_info, fence) == VK_SUCCESS &&
               vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS && vkResetFences(device, 1, &fence) == VK_SUCCESS;
    };

    std::printf("[snapshot-test] mapped at %p\n", mapped);

    if (!submit_and_wait(setup_command_buffer, VK_NULL_HANDLE, VK_NULL_HANDLE))
    {
        std::printf("[snapshot-test] image setup failed\n");
        return 3;
    }

    for (uint32_t iteration = 0; iteration < iterations; ++iteration)
    {
        if (iteration > 0)
        {
            if (!submit_and_wait(mirror_command_buffer, results_ready, VK_NULL_HANDLE) ||
                !submit_and_wait(keep_command_buffer, VK_NULL_HANDLE, VK_NULL_HANDLE) ||
                !submit_and_wait(image_command_buffer, VK_NULL_HANDLE, VK_NULL_HANDLE))
            {
                std::printf("[snapshot-test] iteration %u: mirror submission failed\n", iteration);
                return 3;
            }

            if (!holds_iteration(destination_data, iteration - 1) ||
                (iteration > 1 && (!holds_iteration(mirror_data, iteration - 2) || !holds_fill(image_data, iteration - 2))))
            {
                std::printf("[snapshot-test] iteration %u: GPU memory lost what iteration %u wrote\n", iteration, iteration - 1);
                return 2;
            }
        }

        for (size_t i = 0; i < half_count; ++i)
        {
            source_data[i] = source_value(iteration);
        }

        vkBeginCommandBuffer(command_buffer, &begin_info);
        vkCmdFillBuffer(command_buffer, destination, 0, half_size, fill_value(iteration));
        const VkBufferCopy region{.srcOffset = 0, .dstOffset = half_size, .size = half_size};
        vkCmdCopyBuffer(command_buffer, source, destination, 1, &region);
        vkEndCommandBuffer(command_buffer);

        if (!submit_and_wait(command_buffer, VK_NULL_HANDLE, results_ready))
        {
            std::printf("[snapshot-test] iteration %u: submission failed\n", iteration);
            return 3;
        }

        const auto count = static_cast<size_t>(buffer_size / sizeof(uint32_t));
        std::printf("[snapshot-test] iteration %u checksum %016" PRIx64 " %016" PRIx64 " %016" PRIx64 " mapped %p\n", iteration,
                    checksum(destination_data, count), checksum(mirror_data, count), checksum(image_data, half_count), mapped);
        Sleep(500);
    }

    std::printf("[snapshot-test] done\n");
    return 0;
}
