#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <vulkan/vulkan_core.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

// Keeps GPU state alive across iterations and prints a checksum per iteration, so that a run suspended and resumed in
// the middle can be compared with an uninterrupted one. Each iteration first checks what the previous one left in GPU
// memory, which is what a snapshot has to carry over.
namespace
{
    constexpr VkDeviceSize half_size = 16 * 1024;
    constexpr VkDeviceSize buffer_size = 2 * half_size;
    constexpr uint32_t iterations = 40;

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
        uint64_t hash = 14695981039346656037ull;
        for (size_t i = 0; i < count; ++i)
        {
            hash = (hash ^ data[i]) * 1099511628211ull;
        }
        return hash;
    }

    bool check_destination(const uint32_t* destination, const uint32_t iteration)
    {
        constexpr auto half_count = static_cast<size_t>(half_size / sizeof(uint32_t));
        for (size_t i = 0; i < half_count; ++i)
        {
            if (destination[i] != fill_value(iteration) || destination[half_count + i] != source_value(iteration))
            {
                return false;
            }
        }
        return true;
    }
}

int main(int argc, char** argv)
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
    LOAD(instance, vkCreateFence);
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

    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < family_count; ++i)
    {
        if (families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT))
        {
            family = i;
            break;
        }
    }

    if (family == UINT32_MAX)
    {
        std::printf("[snapshot-test] no transfer queue family\n");
        return 1;
    }

    const float priority = 1.0f;
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
    if (vkCreateBuffer(device, &buffer_info, nullptr, &source) != VK_SUCCESS ||
        vkCreateBuffer(device, &buffer_info, nullptr, &destination) != VK_SUCCESS)
    {
        std::printf("[snapshot-test] vkCreateBuffer failed\n");
        return 1;
    }

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, source, &requirements);
    const VkDeviceSize stride = (requirements.size + requirements.alignment - 1) / requirements.alignment * requirements.alignment;

    VkPhysicalDeviceMemoryProperties memory_properties{};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);

    constexpr VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t memory_type = UINT32_MAX;
    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i)
    {
        if ((requirements.memoryTypeBits & (1u << i)) && (memory_properties.memoryTypes[i].propertyFlags & wanted) == wanted)
        {
            memory_type = i;
            break;
        }
    }

    if (memory_type == UINT32_MAX)
    {
        std::printf("[snapshot-test] no host-visible coherent memory type\n");
        return 1;
    }

    VkMemoryAllocateInfo allocate_info{};
    allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate_info.allocationSize = 2 * stride;
    allocate_info.memoryTypeIndex = memory_type;

    VkDeviceMemory memory{};
    if (vkAllocateMemory(device, &allocate_info, nullptr, &memory) != VK_SUCCESS ||
        vkBindBufferMemory(device, source, memory, 0) != VK_SUCCESS ||
        vkBindBufferMemory(device, destination, memory, stride) != VK_SUCCESS)
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

    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = family;

    VkCommandPool pool{};
    VkCommandBuffer command_buffer{};
    VkCommandBufferAllocateInfo command_buffer_info{};
    command_buffer_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    command_buffer_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_buffer_info.commandBufferCount = 1;
    if (vkCreateCommandPool(device, &pool_info, nullptr, &pool) != VK_SUCCESS ||
        (command_buffer_info.commandPool = pool, vkAllocateCommandBuffers(device, &command_buffer_info, &command_buffer)) != VK_SUCCESS)
    {
        std::printf("[snapshot-test] command buffer setup failed\n");
        return 1;
    }

    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence{};
    if (vkCreateFence(device, &fence_info, nullptr, &fence) != VK_SUCCESS)
    {
        std::printf("[snapshot-test] vkCreateFence failed\n");
        return 1;
    }

    std::printf("[snapshot-test] mapped at %p\n", mapped);

    for (uint32_t iteration = 0; iteration < iterations; ++iteration)
    {
        if (iteration > 0 && !check_destination(destination_data, iteration - 1))
        {
            std::printf("[snapshot-test] iteration %u: GPU memory lost what iteration %u wrote\n", iteration, iteration - 1);
            return 2;
        }

        constexpr auto half_count = static_cast<size_t>(half_size / sizeof(uint32_t));
        for (size_t i = 0; i < half_count; ++i)
        {
            source_data[i] = source_value(iteration);
        }

        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(command_buffer, &begin_info);
        vkCmdFillBuffer(command_buffer, destination, 0, half_size, fill_value(iteration));

        const VkBufferCopy region{.srcOffset = 0, .dstOffset = half_size, .size = half_size};
        vkCmdCopyBuffer(command_buffer, source, destination, 1, &region);

        vkEndCommandBuffer(command_buffer);

        VkSubmitInfo submit_info{};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &command_buffer;
        if (vkQueueSubmit(queue, 1, &submit_info, fence) != VK_SUCCESS ||
            vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS || vkResetFences(device, 1, &fence) != VK_SUCCESS)
        {
            std::printf("[snapshot-test] iteration %u: submission failed\n", iteration);
            return 3;
        }

        std::printf("[snapshot-test] iteration %u checksum %016llx mapped %p\n", iteration,
                    static_cast<unsigned long long>(checksum(destination_data, static_cast<size_t>(buffer_size / sizeof(uint32_t)))),
                    mapped);
        Sleep(500);
    }

    std::printf("[snapshot-test] done\n");
    return 0;
}
