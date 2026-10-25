#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <vulkan/vulkan_core.h>

#include "check_samples.hpp"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

// Keeps GPU state alive across iterations and prints a checksum per iteration, so that a run suspended and resumed in
// the middle can be compared with an uninterrupted one. Between two iterations, where a suspend lands, GPU memory holds
// the previous iteration's results, a binary semaphore holds a signal the next iteration waits for, and a command buffer
// recorded once before the loop is waiting to be submitted again. A buffer in memory the host cannot map carries the
// results of the iteration before the previous one, and so does an image that rests in a shader-read layout. A
// timestamp query written once has to keep its result, and so does a multisampled image, checked through a resolve.
// Multisampled depth/stencil and integer images, which cannot be resolved, are read sample by sample by a compute shader
// of the test's own. An image is also passed between two queues in the opposite order of the submissions, through a timeline semaphore, and
// another one is handed between two queue families every iteration, starting on the one that is not the device's first.
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

    uint32_t find_other_family(const std::vector<VkQueueFamilyProperties>& families, const uint32_t family)
    {
        for (uint32_t i = 0; i < families.size(); ++i)
        {
            if (i != family && (families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT)) != 0)
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
    // Each line has to reach a redirected output right away, since a suspended run never flushes it.
    (void)std::setvbuf(stdout, nullptr, _IONBF, 0);

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
    app_info.apiVersion = VK_API_VERSION_1_2;

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
    LOAD(instance, vkCreateImageView);
    LOAD(instance, vkCreateShaderModule);
    LOAD(instance, vkCreateDescriptorSetLayout);
    LOAD(instance, vkCreatePipelineLayout);
    LOAD(instance, vkCreateComputePipelines);
    LOAD(instance, vkCreateDescriptorPool);
    LOAD(instance, vkAllocateDescriptorSets);
    LOAD(instance, vkUpdateDescriptorSets);
    LOAD(instance, vkCmdBindPipeline);
    LOAD(instance, vkCmdBindDescriptorSets);
    LOAD(instance, vkCmdPushConstants);
    LOAD(instance, vkCmdDispatch);
    LOAD(instance, vkCmdCopyImageToBuffer);
    LOAD(instance, vkCmdCopyBufferToImage);
    LOAD(instance, vkCreateQueryPool);
    LOAD(instance, vkCmdResetQueryPool);
    LOAD(instance, vkCmdWriteTimestamp);
    LOAD(instance, vkGetQueryPoolResults);
    LOAD(instance, vkCmdCopyQueryPoolResults);
    LOAD(instance, vkCmdClearColorImage);
    LOAD(instance, vkCmdResolveImage);
    LOAD(instance, vkCmdClearDepthStencilImage);
    LOAD(instance, vkCreateFence);
    LOAD(instance, vkCreateSemaphore);
    LOAD(instance, vkQueueSubmit);
    LOAD(instance, vkWaitForFences);
    LOAD(instance, vkResetFences);

    uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(instance, &device_count, nullptr);
    std::vector<VkPhysicalDevice> physical_devices(device_count);
    if (vkEnumeratePhysicalDevices(instance, &device_count, physical_devices.data()) < 0 || device_count == 0)
    {
        std::printf("[snapshot-test] no physical device\n");
        return 1;
    }

    VkPhysicalDevice physical_device{};
    std::vector<VkQueueFamilyProperties> families{};
    uint32_t family = UINT32_MAX;
    for (auto* const candidate : physical_devices)
    {
        uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> candidate_families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, candidate_families.data());

        const uint32_t candidate_family = find_queue_family(candidate_families);
        if (candidate_family != UINT32_MAX &&
            (family == UINT32_MAX || candidate_families[candidate_family].queueCount > families[family].queueCount))
        {
            physical_device = candidate;
            families = std::move(candidate_families);
            family = candidate_family;
        }
    }

    if (family == UINT32_MAX)
    {
        std::printf("[snapshot-test] no transfer queue family\n");
        return 1;
    }

    const std::array priorities{1.0F, 1.0F};
    const uint32_t queue_count = (std::min)(2U, families[family].queueCount);
    const uint32_t other_family = find_other_family(families, family);
    std::printf("[snapshot-test] %u devices, using %u queues and %u families\n", device_count, queue_count,
                other_family == UINT32_MAX ? 1 : 2);
    std::array<VkDeviceQueueCreateInfo, 2> queue_infos{};
    for (auto& queue_info : queue_infos)
    {
        queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = priorities.data();
    }
    queue_infos[0].queueFamilyIndex = family;
    queue_infos[0].queueCount = queue_count;
    queue_infos[1].queueFamilyIndex = other_family;

    VkPhysicalDeviceVulkan12Features timeline_features{};
    timeline_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    timeline_features.timelineSemaphore = VK_TRUE;

    VkDeviceCreateInfo device_info{};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.pNext = &timeline_features;
    device_info.queueCreateInfoCount = other_family == UINT32_MAX ? 1 : 2;
    device_info.pQueueCreateInfos = queue_infos.data();

    VkDevice device{};
    if (vkCreateDevice(physical_device, &device_info, nullptr, &device) != VK_SUCCESS)
    {
        std::printf("[snapshot-test] vkCreateDevice failed\n");
        return 1;
    }

    VkQueue queue{};
    VkQueue second_queue{};
    vkGetDeviceQueue(device, family, 0, &queue);
    vkGetDeviceQueue(device, family, queue_count - 1, &second_queue);
    VkQueue other_queue{};
    if (other_family != UINT32_MAX)
    {
        vkGetDeviceQueue(device, other_family, 0, &other_queue);
    }

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

    const VkImageCreateInfo image_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {.width = image_size, .height = image_size, .depth = 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    VkImageCreateInfo multisampled_info = image_info;
    multisampled_info.samples = VK_SAMPLE_COUNT_4_BIT;
    multisampled_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VkImageCreateInfo resolved_info = image_info;
    resolved_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    VkImage multisampled{};
    VkImage resolved{};
    VkBuffer multisampled_readback{};
    std::array<VkDeviceMemory, 3> multisampled_memory{};
    if (vkCreateImage(device, &multisampled_info, nullptr, &multisampled) != VK_SUCCESS ||
        vkCreateImage(device, &resolved_info, nullptr, &resolved) != VK_SUCCESS ||
        vkCreateBuffer(device, &buffer_info, nullptr, &multisampled_readback) != VK_SUCCESS)
    {
        std::printf("[snapshot-test] multisampled image setup failed\n");
        return 1;
    }

    const auto bind_new_memory = [&](VkDeviceMemory& memory_out, const VkMemoryRequirements& needs, const VkMemoryPropertyFlags wanted) {
        VkMemoryAllocateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        info.allocationSize = needs.size;
        info.memoryTypeIndex = find_memory_type(memory_properties, needs.memoryTypeBits, wanted, 0);
        return info.memoryTypeIndex != UINT32_MAX && vkAllocateMemory(device, &info, nullptr, &memory_out) == VK_SUCCESS;
    };

    VkMemoryRequirements needs{};
    vkGetImageMemoryRequirements(device, multisampled, &needs);
    bool bound = bind_new_memory(multisampled_memory[0], needs, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
                 vkBindImageMemory(device, multisampled, multisampled_memory[0], 0) == VK_SUCCESS;
    vkGetImageMemoryRequirements(device, resolved, &needs);
    bound = bound && bind_new_memory(multisampled_memory[1], needs, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
            vkBindImageMemory(device, resolved, multisampled_memory[1], 0) == VK_SUCCESS;
    vkGetBufferMemoryRequirements(device, multisampled_readback, &needs);
    bound = bound &&
            bind_new_memory(multisampled_memory[2], needs, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) &&
            vkBindBufferMemory(device, multisampled_readback, multisampled_memory[2], 0) == VK_SUCCESS;

    void* multisampled_mapped = nullptr;
    if (!bound || vkMapMemory(device, multisampled_memory[2], 0, VK_WHOLE_SIZE, 0, &multisampled_mapped) != VK_SUCCESS)
    {
        std::printf("[snapshot-test] multisampled memory setup failed\n");
        return 1;
    }
    const auto* resolved_data = static_cast<const uint32_t*>(multisampled_mapped);

    VkImageCreateInfo depth_info = multisampled_info;
    depth_info.format = VK_FORMAT_D32_SFLOAT_S8_UINT;
    depth_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VkImageCreateInfo integer_info = multisampled_info;
    integer_info.format = VK_FORMAT_R32_UINT;
    integer_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

    VkImage shared{};
    VkImage depth_stencil{};
    VkImage integer{};
    std::array<VkDeviceMemory, 3> attachment_memory{};
    if (vkCreateImage(device, &depth_info, nullptr, &depth_stencil) != VK_SUCCESS)
    {
        depth_info.format = VK_FORMAT_D24_UNORM_S8_UINT;
        if (vkCreateImage(device, &depth_info, nullptr, &depth_stencil) != VK_SUCCESS)
        {
            std::printf("[snapshot-test] no multisampled depth/stencil format\n");
            return 1;
        }
    }

    if (vkCreateImage(device, &integer_info, nullptr, &integer) != VK_SUCCESS ||
        vkCreateImage(device, &image_info, nullptr, &shared) != VK_SUCCESS)
    {
        std::printf("[snapshot-test] multisampled integer image setup failed\n");
        return 1;
    }

    vkGetImageMemoryRequirements(device, depth_stencil, &needs);
    bound = bind_new_memory(attachment_memory[0], needs, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
            vkBindImageMemory(device, depth_stencil, attachment_memory[0], 0) == VK_SUCCESS;
    vkGetImageMemoryRequirements(device, integer, &needs);
    bound = bound && bind_new_memory(attachment_memory[1], needs, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
            vkBindImageMemory(device, integer, attachment_memory[1], 0) == VK_SUCCESS;
    vkGetImageMemoryRequirements(device, shared, &needs);
    bound = bound && bind_new_memory(attachment_memory[2], needs, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
            vkBindImageMemory(device, shared, attachment_memory[2], 0) == VK_SUCCESS;
    if (!bound)
    {
        std::printf("[snapshot-test] multisampled attachment memory setup failed\n");
        return 1;
    }

    const auto create_view = [&](const VkImage target, const VkFormat format, const VkImageAspectFlags aspect, VkImageView& view) {
        const VkImageViewCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .image = target,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = format,
            .components = {},
            .subresourceRange = {.aspectMask = aspect, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1},
        };
        return vkCreateImageView(device, &info, nullptr, &view) == VK_SUCCESS;
    };

    std::array<VkImageView, 3> check_views{};
    VkBuffer check_buffer{};
    VkDeviceMemory check_memory{};
    void* check_mapped = nullptr;
    VkBufferCreateInfo check_buffer_info = buffer_info;
    check_buffer_info.size = 3 * sizeof(uint32_t);
    check_buffer_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bound = create_view(integer, integer_info.format, VK_IMAGE_ASPECT_COLOR_BIT, check_views[0]) &&
            create_view(depth_stencil, depth_info.format, VK_IMAGE_ASPECT_DEPTH_BIT, check_views[1]) &&
            create_view(depth_stencil, depth_info.format, VK_IMAGE_ASPECT_STENCIL_BIT, check_views[2]) &&
            vkCreateBuffer(device, &check_buffer_info, nullptr, &check_buffer) == VK_SUCCESS;
    if (bound)
    {
        vkGetBufferMemoryRequirements(device, check_buffer, &needs);
        bound = bind_new_memory(check_memory, needs, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) &&
                vkBindBufferMemory(device, check_buffer, check_memory, 0) == VK_SUCCESS &&
                vkMapMemory(device, check_memory, 0, VK_WHOLE_SIZE, 0, &check_mapped) == VK_SUCCESS;
    }

    struct expected_samples
    {
        uint32_t integer_value;
        float depth_value;
        uint32_t stencil_value;
        uint32_t size;
        int32_t samples;
    };

    constexpr std::array check_pool_sizes{VkDescriptorPoolSize{.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .descriptorCount = 3},
                                          VkDescriptorPoolSize{.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1}};
    const std::array check_bindings{
        VkDescriptorSetLayoutBinding{.binding = 0,
                                     .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                     .descriptorCount = 1,
                                     .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                                     .pImmutableSamplers = nullptr},
        VkDescriptorSetLayoutBinding{.binding = 1,
                                     .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                     .descriptorCount = 1,
                                     .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                                     .pImmutableSamplers = nullptr},
        VkDescriptorSetLayoutBinding{.binding = 2,
                                     .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                     .descriptorCount = 1,
                                     .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                                     .pImmutableSamplers = nullptr},
        VkDescriptorSetLayoutBinding{.binding = 3,
                                     .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                     .descriptorCount = 1,
                                     .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                                     .pImmutableSamplers = nullptr},
    };
    const VkDescriptorSetLayoutCreateInfo check_set_layout_info{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                                .pNext = nullptr,
                                                                .flags = 0,
                                                                .bindingCount = static_cast<uint32_t>(check_bindings.size()),
                                                                .pBindings = check_bindings.data()};
    VkDescriptorSetLayout check_set_layout{};
    const VkPushConstantRange check_push_range{.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = sizeof(expected_samples)};
    const VkPipelineLayoutCreateInfo check_layout_info{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                       .pNext = nullptr,
                                                       .flags = 0,
                                                       .setLayoutCount = 1,
                                                       .pSetLayouts = &check_set_layout,
                                                       .pushConstantRangeCount = 1,
                                                       .pPushConstantRanges = &check_push_range};
    VkPipelineLayout check_layout{};
    const VkShaderModuleCreateInfo check_module_info{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                                     .pNext = nullptr,
                                                     .flags = 0,
                                                     .codeSize = check_samples_shader.size() * sizeof(uint32_t),
                                                     .pCode = check_samples_shader.data()};
    VkShaderModule check_module{};
    VkPipeline check_pipeline{};
    const VkDescriptorPoolCreateInfo check_pool_info{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                                     .pNext = nullptr,
                                                     .flags = 0,
                                                     .maxSets = 1,
                                                     .poolSizeCount = static_cast<uint32_t>(check_pool_sizes.size()),
                                                     .pPoolSizes = check_pool_sizes.data()};
    VkDescriptorPool check_pool{};
    VkDescriptorSet check_set{};
    bound = bound && vkCreateDescriptorSetLayout(device, &check_set_layout_info, nullptr, &check_set_layout) == VK_SUCCESS &&
            vkCreatePipelineLayout(device, &check_layout_info, nullptr, &check_layout) == VK_SUCCESS &&
            vkCreateShaderModule(device, &check_module_info, nullptr, &check_module) == VK_SUCCESS &&
            vkCreateDescriptorPool(device, &check_pool_info, nullptr, &check_pool) == VK_SUCCESS;
    if (bound)
    {
        const VkComputePipelineCreateInfo pipeline_info{
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                      .pNext = nullptr,
                      .flags = 0,
                      .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                      .module = check_module,
                      .pName = "main",
                      .pSpecializationInfo = nullptr},
            .layout = check_layout,
            .basePipelineHandle = VK_NULL_HANDLE,
            .basePipelineIndex = -1,
        };
        const VkDescriptorSetAllocateInfo set_info{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                   .pNext = nullptr,
                                                   .descriptorPool = check_pool,
                                                   .descriptorSetCount = 1,
                                                   .pSetLayouts = &check_set_layout};
        bound = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &check_pipeline) == VK_SUCCESS &&
                vkAllocateDescriptorSets(device, &set_info, &check_set) == VK_SUCCESS;
    }
    if (!bound)
    {
        std::printf("[snapshot-test] multisampled check setup failed\n");
        return 1;
    }

    std::array<VkDescriptorImageInfo, 3> check_images{};
    std::array<VkWriteDescriptorSet, 4> check_writes{};
    for (uint32_t i = 0; i < check_writes.size(); ++i)
    {
        check_writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        check_writes[i].dstSet = check_set;
        check_writes[i].dstBinding = i;
        check_writes[i].descriptorCount = 1;
        check_writes[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        if (i < check_images.size())
        {
            check_images[i] = {.sampler = VK_NULL_HANDLE, .imageView = check_views[i], .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
            check_writes[i].pImageInfo = &check_images[i];
        }
    }
    const VkDescriptorBufferInfo check_buffer_descriptor{.buffer = check_buffer, .offset = 0, .range = VK_WHOLE_SIZE};
    check_writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    check_writes[3].pBufferInfo = &check_buffer_descriptor;
    vkUpdateDescriptorSets(device, static_cast<uint32_t>(check_writes.size()), check_writes.data(), 0, nullptr);
    const auto* check_mismatches = static_cast<const uint32_t*>(check_mapped);

    const std::array both_families{family, other_family};
    VkBufferCreateInfo travel_buffer_info = buffer_info;
    travel_buffer_info.size = half_size;
    travel_buffer_info.sharingMode = other_family == UINT32_MAX ? VK_SHARING_MODE_EXCLUSIVE : VK_SHARING_MODE_CONCURRENT;
    travel_buffer_info.queueFamilyIndexCount = other_family == UINT32_MAX ? 0 : 2;
    travel_buffer_info.pQueueFamilyIndices = both_families.data();

    VkImage travelling{};
    VkBuffer travel_source{};
    VkBuffer travel_readback{};
    std::array<VkDeviceMemory, 3> travel_memory{};
    void* travel_mapped = nullptr;
    if (vkCreateImage(device, &image_info, nullptr, &travelling) != VK_SUCCESS ||
        vkCreateBuffer(device, &travel_buffer_info, nullptr, &travel_source) != VK_SUCCESS ||
        vkCreateBuffer(device, &travel_buffer_info, nullptr, &travel_readback) != VK_SUCCESS)
    {
        std::printf("[snapshot-test] travelling image setup failed\n");
        return 1;
    }
    constexpr VkMemoryPropertyFlags host_coherent = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    vkGetImageMemoryRequirements(device, travelling, &needs);
    bound = bind_new_memory(travel_memory[0], needs, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
            vkBindImageMemory(device, travelling, travel_memory[0], 0) == VK_SUCCESS;
    vkGetBufferMemoryRequirements(device, travel_source, &needs);
    bound = bound && bind_new_memory(travel_memory[1], needs, host_coherent) &&
            vkBindBufferMemory(device, travel_source, travel_memory[1], 0) == VK_SUCCESS;
    vkGetBufferMemoryRequirements(device, travel_readback, &needs);
    bound = bound && bind_new_memory(travel_memory[2], needs, host_coherent) &&
            vkBindBufferMemory(device, travel_readback, travel_memory[2], 0) == VK_SUCCESS;
    void* travel_source_mapped = nullptr;
    if (!bound || vkMapMemory(device, travel_memory[1], 0, VK_WHOLE_SIZE, 0, &travel_source_mapped) != VK_SUCCESS ||
        vkMapMemory(device, travel_memory[2], 0, VK_WHOLE_SIZE, 0, &travel_mapped) != VK_SUCCESS)
    {
        std::printf("[snapshot-test] travelling memory setup failed\n");
        return 1;
    }
    auto* travel_source_data = static_cast<uint32_t*>(travel_source_mapped);
    const auto* travel_data = static_cast<const uint32_t*>(travel_mapped);

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
    VkCommandPool other_pool{};
    VkCommandPoolCreateInfo other_pool_info = pool_info;
    other_pool_info.queueFamilyIndex = other_family;
    if (vkCreateCommandPool(device, &pool_info, nullptr, &pool) != VK_SUCCESS ||
        (other_family != UINT32_MAX && vkCreateCommandPool(device, &other_pool_info, nullptr, &other_pool) != VK_SUCCESS))
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
    VkCommandBuffer query_copy_command_buffer{};
    VkCommandBuffer check_command_buffer{};
    VkCommandBuffer hand_back_command_buffer{};
    VkCommandBuffer hand_over_command_buffer{};
    std::array<VkCommandBuffer, 2> travel_command_buffers{};
    if (other_family != UINT32_MAX)
    {
        VkCommandBufferAllocateInfo other_info = command_buffer_info;
        other_info.commandPool = other_pool;
        if (vkAllocateCommandBuffers(device, &command_buffer_info, &travel_command_buffers[0]) != VK_SUCCESS ||
            vkAllocateCommandBuffers(device, &other_info, &travel_command_buffers[1]) != VK_SUCCESS)
        {
            std::printf("[snapshot-test] travelling command buffer setup failed\n");
            return 1;
        }
    }
    if (vkAllocateCommandBuffers(device, &command_buffer_info, &query_copy_command_buffer) != VK_SUCCESS ||
        vkAllocateCommandBuffers(device, &command_buffer_info, &check_command_buffer) != VK_SUCCESS ||
        vkAllocateCommandBuffers(device, &command_buffer_info, &hand_back_command_buffer) != VK_SUCCESS ||
        vkAllocateCommandBuffers(device, &command_buffer_info, &hand_over_command_buffer) != VK_SUCCESS ||
        vkAllocateCommandBuffers(device, &command_buffer_info, &image_command_buffer) != VK_SUCCESS ||
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

    VkSemaphoreTypeCreateInfo timeline_info{};
    timeline_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    timeline_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo timeline_semaphore_info{};
    timeline_semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    timeline_semaphore_info.pNext = &timeline_info;

    VkSemaphore handed_over{};
    if (vkCreateSemaphore(device, &timeline_semaphore_info, nullptr, &handed_over) != VK_SUCCESS)
    {
        std::printf("[snapshot-test] timeline semaphore setup failed\n");
        return 1;
    }

    VkFence fence{};
    VkSemaphore results_ready{};
    VkSemaphore travelled{};
    if (vkCreateFence(device, &fence_info, nullptr, &fence) != VK_SUCCESS ||
        vkCreateSemaphore(device, &semaphore_info, nullptr, &travelled) != VK_SUCCESS ||
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

    const auto transition_image = [&](const VkCommandBuffer recorded, const VkImage target, const VkImageLayout from,
                                      const VkImageLayout to) {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.oldLayout = from;
        barrier.newLayout = to;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = target;
        barrier.subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
        vkCmdPipelineBarrier(recorded, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &barrier);
    };
    const auto transition = [&](const VkCommandBuffer recorded, const VkImageLayout from, const VkImageLayout to) {
        transition_image(recorded, image, from, to);
    };

    VkBufferImageCopy image_region{};
    image_region.imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1};
    image_region.imageExtent = {.width = image_size, .height = image_size, .depth = 1};

    VkQueryPoolCreateInfo query_pool_info{};
    query_pool_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    query_pool_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    query_pool_info.queryCount = 2;

    VkQueryPool query_pool{};
    if (vkCreateQueryPool(device, &query_pool_info, nullptr, &query_pool) != VK_SUCCESS)
    {
        std::printf("[snapshot-test] vkCreateQueryPool failed\n");
        return 1;
    }

    vkBeginCommandBuffer(setup_command_buffer, &begin_info);
    transition(setup_command_buffer, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    transition_image(setup_command_buffer, multisampled, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    transition_image(setup_command_buffer, resolved, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    transition_image(setup_command_buffer, integer, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    transition_image(setup_command_buffer, shared, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = depth_stencil;
        barrier.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                                    .baseMipLevel = 0,
                                    .levelCount = 1,
                                    .baseArrayLayer = 0,
                                    .layerCount = 1};
        vkCmdPipelineBarrier(setup_command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &barrier);
    }
    vkCmdResetQueryPool(setup_command_buffer, query_pool, 0, 2);
    vkCmdWriteTimestamp(setup_command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, query_pool, 0);
    vkEndCommandBuffer(setup_command_buffer);

    vkBeginCommandBuffer(query_copy_command_buffer, &begin_info);
    vkCmdCopyQueryPoolResults(query_copy_command_buffer, query_pool, 0, 1, image_readback, half_size, sizeof(uint64_t),
                              VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    vkEndCommandBuffer(query_copy_command_buffer);
    const auto* copied_timestamp = reinterpret_cast<const uint64_t*>(image_data + half_count);

    const auto read_timestamp = [&](const uint32_t query) {
        uint64_t value = 0;
        return vkGetQueryPoolResults(device, query_pool, query, 1, sizeof(value), &value, sizeof(value),
                                     VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS
                   ? value
                   : 0;
    };

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

    const auto hand_travelling = [&](const VkCommandBuffer recorded, const uint32_t from_family, const uint32_t to_family,
                                     const bool release) {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = release ? VK_ACCESS_TRANSFER_WRITE_BIT : 0;
        barrier.dstAccessMask = release ? 0 : VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcQueueFamilyIndex = from_family;
        barrier.dstQueueFamilyIndex = to_family;
        barrier.image = travelling;
        barrier.subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
        vkCmdPipelineBarrier(recorded, release ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             release ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &barrier);
    };

    // The owning family reads back what the previous iteration wrote, writes this iteration's value and hands the image
    // to the other family.
    const auto travel = [&](const uint32_t iteration) {
        const uint32_t owner = iteration % 2 == 0 ? 1 : 0;
        const std::array travel_families{family, other_family};
        const std::array travel_queues{queue, other_queue};
        const VkCommandBuffer owning = travel_command_buffers[owner];
        const VkCommandBuffer receiving = travel_command_buffers[1 - owner];

        vkBeginCommandBuffer(owning, &begin_info);
        if (iteration == 0)
        {
            transition_image(owning, travelling, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        }
        else
        {
            vkCmdCopyImageToBuffer(owning, travelling, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, travel_readback, 1, &image_region);
            transition_image(owning, travelling, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        }
        vkCmdCopyBufferToImage(owning, travel_source, travelling, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &image_region);
        hand_travelling(owning, travel_families[owner], travel_families[1 - owner], true);
        vkEndCommandBuffer(owning);

        vkBeginCommandBuffer(receiving, &begin_info);
        hand_travelling(receiving, travel_families[owner], travel_families[1 - owner], false);
        vkEndCommandBuffer(receiving);

        for (size_t i = 0; i < half_count; ++i)
        {
            travel_source_data[i] = source_value(iteration);
        }

        constexpr VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo release_info{};
        release_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        release_info.commandBufferCount = 1;
        release_info.pCommandBuffers = &owning;
        release_info.signalSemaphoreCount = 1;
        release_info.pSignalSemaphores = &travelled;
        VkSubmitInfo acquire_info{};
        acquire_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        acquire_info.waitSemaphoreCount = 1;
        acquire_info.pWaitSemaphores = &travelled;
        acquire_info.pWaitDstStageMask = &wait_stage;
        acquire_info.commandBufferCount = 1;
        acquire_info.pCommandBuffers = &receiving;
        return vkQueueSubmit(travel_queues[owner], 1, &release_info, VK_NULL_HANDLE) == VK_SUCCESS &&
               vkQueueSubmit(travel_queues[1 - owner], 1, &acquire_info, fence) == VK_SUCCESS &&
               vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS && vkResetFences(device, 1, &fence) == VK_SUCCESS &&
               (iteration == 0 || std::ranges::all_of(std::span(travel_data, half_count),
                                                      [&](const uint32_t texel) { return texel == source_value(iteration - 1); }));
    };

    std::printf("[snapshot-test] mapped at %p\n", mapped);

    if (!submit_and_wait(setup_command_buffer, VK_NULL_HANDLE, VK_NULL_HANDLE))
    {
        std::printf("[snapshot-test] image setup failed\n");
        return 3;
    }

    const uint64_t first_timestamp = read_timestamp(0);

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

        if (read_timestamp(0) != first_timestamp || !submit_and_wait(query_copy_command_buffer, VK_NULL_HANDLE, VK_NULL_HANDLE) ||
            *copied_timestamp != first_timestamp)
        {
            std::printf("[snapshot-test] iteration %u: the timestamp query lost its result\n", iteration);
            return 4;
        }

        if (iteration > 0)
        {
            const uint32_t previous = fill_value(iteration - 1);
            const expected_samples expected{.integer_value = previous,
                                            .depth_value = static_cast<float>((iteration - 1) % 100) / 100.0F,
                                            .stencil_value = previous & 0xFF,
                                            .size = image_size,
                                            .samples = 4};

            vkBeginCommandBuffer(check_command_buffer, &begin_info);
            vkCmdFillBuffer(check_command_buffer, check_buffer, 0, VK_WHOLE_SIZE, 0);
            const VkMemoryBarrier to_shader{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                            .pNext = nullptr,
                                            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
            vkCmdPipelineBarrier(check_command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                                 &to_shader, 0, nullptr, 0, nullptr);
            vkCmdBindPipeline(check_command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, check_pipeline);
            vkCmdBindDescriptorSets(check_command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, check_layout, 0, 1, &check_set, 0, nullptr);
            vkCmdPushConstants(check_command_buffer, check_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(expected), &expected);
            vkCmdDispatch(check_command_buffer, (image_size * image_size + 63) / 64, 1, 1);
            const VkMemoryBarrier to_host{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                          .pNext = nullptr,
                                          .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                                          .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
            vkCmdPipelineBarrier(check_command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &to_host, 0,
                                 nullptr, 0, nullptr);
            vkEndCommandBuffer(check_command_buffer);

            if (!submit_and_wait(check_command_buffer, VK_NULL_HANDLE, VK_NULL_HANDLE))
            {
                std::printf("[snapshot-test] iteration %u: checking the multisampled images failed\n", iteration);
                return 3;
            }

            if (check_mismatches[0] != 0 || check_mismatches[1] != 0 || check_mismatches[2] != 0)
            {
                std::printf("[snapshot-test] iteration %u: multisampled samples differ from what iteration %u cleared: integer %u, depth "
                            "%u, stencil %u\n",
                            iteration, iteration - 1, check_mismatches[0], check_mismatches[1], check_mismatches[2]);
                return 5;
            }
        }

        vkBeginCommandBuffer(command_buffer, &begin_info);
        vkCmdResetQueryPool(command_buffer, query_pool, 1, 1);
        vkCmdWriteTimestamp(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, query_pool, 1);
        vkCmdFillBuffer(command_buffer, destination, 0, half_size, fill_value(iteration));
        const VkBufferCopy region{.srcOffset = 0, .dstOffset = half_size, .size = half_size};
        vkCmdCopyBuffer(command_buffer, source, destination, 1, &region);

        // The multisampled image still holds the previous iteration's clear, which the resolve reads.
        const VkImageSubresourceLayers color_layer{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1};
        const VkImageResolve resolve{.srcSubresource = color_layer,
                                     .srcOffset = {},
                                     .dstSubresource = color_layer,
                                     .dstOffset = {},
                                     .extent = {.width = image_size, .height = image_size, .depth = 1}};
        vkCmdResolveImage(command_buffer, multisampled, VK_IMAGE_LAYOUT_GENERAL, resolved, VK_IMAGE_LAYOUT_GENERAL, 1, &resolve);
        transition_image(command_buffer, resolved, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        vkCmdCopyImageToBuffer(command_buffer, resolved, VK_IMAGE_LAYOUT_GENERAL, multisampled_readback, 1, &image_region);
        transition_image(command_buffer, multisampled, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);

        const uint32_t value = fill_value(iteration);
        VkClearColorValue clear{};
        for (uint32_t channel = 0; channel < 4; ++channel)
        {
            clear.float32[channel] = static_cast<float>((value >> (channel * 8)) & 0xFF) / 255.0F;
        }
        const VkImageSubresourceRange color_range{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
        vkCmdClearColorImage(command_buffer, multisampled, VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &color_range);

        VkClearColorValue integer_clear{};
        integer_clear.uint32[0] = value;
        vkCmdClearColorImage(command_buffer, integer, VK_IMAGE_LAYOUT_GENERAL, &integer_clear, 1, &color_range);

        const VkClearDepthStencilValue depth_clear{.depth = static_cast<float>(iteration % 100) / 100.0F, .stencil = value & 0xFF};
        const VkImageSubresourceRange depth_range{.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                                                  .baseMipLevel = 0,
                                                  .levelCount = 1,
                                                  .baseArrayLayer = 0,
                                                  .layerCount = 1};
        vkCmdClearDepthStencilImage(command_buffer, depth_stencil, VK_IMAGE_LAYOUT_GENERAL, &depth_clear, 1, &depth_range);
        vkEndCommandBuffer(command_buffer);

        const VkImageSubresourceRange shared_range{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
        vkBeginCommandBuffer(hand_back_command_buffer, &begin_info);
        transition_image(hand_back_command_buffer, shared, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        vkEndCommandBuffer(hand_back_command_buffer);

        vkBeginCommandBuffer(hand_over_command_buffer, &begin_info);
        transition_image(hand_over_command_buffer, shared, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdClearColorImage(hand_over_command_buffer, shared, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &shared_range);
        vkEndCommandBuffer(hand_over_command_buffer);

        const uint64_t handed_over_value = iteration + 1;
        constexpr VkPipelineStageFlags all_commands = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkTimelineSemaphoreSubmitInfo hand_back_timeline{};
        hand_back_timeline.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        hand_back_timeline.waitSemaphoreValueCount = 1;
        hand_back_timeline.pWaitSemaphoreValues = &handed_over_value;
        VkSubmitInfo hand_back{};
        hand_back.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        hand_back.pNext = &hand_back_timeline;
        hand_back.waitSemaphoreCount = 1;
        hand_back.pWaitSemaphores = &handed_over;
        hand_back.pWaitDstStageMask = &all_commands;
        hand_back.commandBufferCount = 1;
        hand_back.pCommandBuffers = &hand_back_command_buffer;

        VkTimelineSemaphoreSubmitInfo hand_over_timeline{};
        hand_over_timeline.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        hand_over_timeline.signalSemaphoreValueCount = 1;
        hand_over_timeline.pSignalSemaphoreValues = &handed_over_value;
        VkSubmitInfo hand_over{};
        hand_over.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        hand_over.pNext = &hand_over_timeline;
        hand_over.commandBufferCount = 1;
        hand_over.pCommandBuffers = &hand_over_command_buffer;
        hand_over.signalSemaphoreCount = 1;
        hand_over.pSignalSemaphores = &handed_over;

        // With a single queue, a wait submitted before its signal would never run.
        const bool reversed = queue_count > 1;
        if (vkQueueSubmit(reversed ? queue : second_queue, 1, reversed ? &hand_back : &hand_over, reversed ? fence : VK_NULL_HANDLE) !=
                VK_SUCCESS ||
            vkQueueSubmit(reversed ? second_queue : queue, 1, reversed ? &hand_over : &hand_back, reversed ? VK_NULL_HANDLE : fence) !=
                VK_SUCCESS ||
            vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS || vkResetFences(device, 1, &fence) != VK_SUCCESS)
        {
            std::printf("[snapshot-test] iteration %u: handing the image between queues failed\n", iteration);
            return 3;
        }

        if (other_family != UINT32_MAX && !travel(iteration))
        {
            std::printf("[snapshot-test] iteration %u: the image handed between queue families lost what iteration %u wrote\n", iteration,
                        iteration - 1);
            return 6;
        }

        if (!submit_and_wait(command_buffer, VK_NULL_HANDLE, results_ready) || read_timestamp(1) == 0)
        {
            std::printf("[snapshot-test] iteration %u: submission failed\n", iteration);
            return 3;
        }

        if (iteration > 0 && !holds_fill(resolved_data, iteration - 1))
        {
            std::printf("[snapshot-test] iteration %u: the multisampled image lost what iteration %u cleared it to\n", iteration,
                        iteration - 1);
            return 5;
        }

        const auto count = static_cast<size_t>(buffer_size / sizeof(uint32_t));
        std::printf("[snapshot-test] iteration %u checksum %016" PRIx64 " %016" PRIx64 " %016" PRIx64 " mapped %p\n", iteration,
                    checksum(destination_data, count), checksum(mirror_data, count), checksum(image_data, half_count), mapped);
        Sleep(500);
    }

    std::printf("[snapshot-test] done\n");
    return 0;
}
