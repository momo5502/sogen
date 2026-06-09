// A windowed Vulkan guest app that draws a *textured* quad, exercising the GPU bridge's texture
// plumbing. It builds on vulkan-uniform-buffer-sample and adds:
//   - a texture image (R8G8B8A8) uploaded from a host-visible staging buffer via a one-time
//     vkCmdCopyBufferToImage, with the layout transitions UNDEFINED -> TRANSFER_DST -> SHADER_READ_ONLY;
//   - a sampler (vkCreateSampler) and an image view;
//   - a combined-image-sampler descriptor (set 0, binding 0, fragment stage) the shader samples.
// The quad spins via a push-constant angle (so this one sample touches vertex buffers, index buffers,
// push constants, descriptor sets, AND textures). The same binary runs through the shim and natively
// against a real driver (pass vulkan-1.dll).

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan_win32.h>

#include "texture_spirv.hpp"

namespace
{
    constexpr uint32_t window_width = 400;
    constexpr uint32_t window_height = 400;
    constexpr VkFormat swapchain_format = VK_FORMAT_B8G8R8A8_UNORM;
    // Wall-clock-time-based rotation (radians/second) so the quad spins at the same physical rate
    // natively and emulated, independent of frame rate.
    constexpr float rotation_speed = 0.9f;
    constexpr uint32_t tex_size = 64; // 64x64 RGBA checkerboard

    struct Vertex
    {
        std::array<float, 2> pos;
        std::array<float, 2> uv;
    };

    constexpr std::array<Vertex, 4> vertices = {{
        {.pos = {-0.5f, -0.5f}, .uv = {0.0f, 0.0f}},
        {.pos = {0.5f, -0.5f}, .uv = {1.0f, 0.0f}},
        {.pos = {0.5f, 0.5f}, .uv = {1.0f, 1.0f}},
        {.pos = {-0.5f, 0.5f}, .uv = {0.0f, 1.0f}},
    }};
    constexpr std::array<uint16_t, 6> indices = {0, 1, 2, 2, 3, 0};

    bool g_quit = false;

    LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
    {
        switch (msg)
        {
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            g_quit = true;
            PostQuitMessage(0);
            return 0;
        case WM_PAINT:
            ValidateRect(hwnd, nullptr);
            return 0;
        default:
            return DefWindowProcA(hwnd, msg, wparam, lparam);
        }
    }

    template <typename Fn>
    Fn load(PFN_vkGetInstanceProcAddr gipa, VkInstance instance, const char* name)
    {
        return reinterpret_cast<Fn>(gipa(instance, name));
    }

    uint64_t now_ms()
    {
        FILETIME ft{};
        GetSystemTimeAsFileTime(&ft);
        ULARGE_INTEGER u{};
        u.LowPart = ft.dwLowDateTime;
        u.HighPart = ft.dwHighDateTime;
        return u.QuadPart / 10000ULL;
    }

    // A 64x64 RGBA checkerboard of two colors (8px cells), generated on the CPU.
    std::vector<uint8_t> make_checkerboard()
    {
        std::vector<uint8_t> pixels(static_cast<size_t>(tex_size) * tex_size * 4);
        for (uint32_t y = 0; y < tex_size; ++y)
        {
            for (uint32_t x = 0; x < tex_size; ++x)
            {
                const bool cell = ((x / 8) + (y / 8)) % 2 == 0;
                uint8_t* p = pixels.data() + (static_cast<size_t>(y) * tex_size + x) * 4;
                p[0] = cell ? 240 : 30;  // R
                p[1] = cell ? 140 : 160; // G
                p[2] = cell ? 40 : 200;  // B
                p[3] = 255;              // A
            }
        }
        return pixels;
    }
}

int main(int argc, char** argv)
{
    const char* dll = (argc > 1) ? argv[1] : "vulkan-shim.dll";
    const uint32_t max_frames = (argc > 2) ? static_cast<uint32_t>(std::atoi(argv[2])) : 100000;

    const HINSTANCE hinstance = GetModuleHandleA(nullptr);
    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &window_proc;
    wc.hInstance = hinstance;
    wc.lpszClassName = "SogenVulkanTexture";
    wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    if (!RegisterClassExA(&wc))
    {
        std::printf("[vk-tex] RegisterClassExA failed: %lu\n", GetLastError());
        return 1;
    }

    constexpr DWORD window_style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE;
    const HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "Sogen Vulkan - Texture", window_style, 200, 200,
                                      static_cast<int>(window_width), static_cast<int>(window_height), nullptr, nullptr,
                                      hinstance, nullptr);
    if (!hwnd)
    {
        std::printf("[vk-tex] CreateWindowExA failed: %lu\n", GetLastError());
        return 2;
    }

    const HMODULE mod = LoadLibraryA(dll);
    if (!mod)
    {
        std::printf("[vk-tex] LoadLibrary(%s) failed: %lu\n", dll, GetLastError());
        return 3;
    }
    const auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        reinterpret_cast<void*>(GetProcAddress(mod, "vkGetInstanceProcAddr")));
    if (!gipa)
    {
        std::printf("[vk-tex] no vkGetInstanceProcAddr export\n");
        return 4;
    }

    const auto create_instance = load<PFN_vkCreateInstance>(gipa, nullptr, "vkCreateInstance");

    const std::array<const char*, 2> instance_exts = {"VK_KHR_surface", "VK_KHR_win32_surface"};
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = static_cast<uint32_t>(instance_exts.size());
    ici.ppEnabledExtensionNames = instance_exts.data();

    VkInstance instance = VK_NULL_HANDLE;
    if (create_instance(&ici, nullptr, &instance) != VK_SUCCESS || !instance)
    {
        std::printf("[vk-tex] vkCreateInstance failed (no host Vulkan driver?)\n");
        return 5;
    }

    const auto enumerate = load<PFN_vkEnumeratePhysicalDevices>(gipa, instance, "vkEnumeratePhysicalDevices");
    const auto get_queue_families =
        load<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(gipa, instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    const auto get_memory_properties =
        load<PFN_vkGetPhysicalDeviceMemoryProperties>(gipa, instance, "vkGetPhysicalDeviceMemoryProperties");
    const auto create_device = load<PFN_vkCreateDevice>(gipa, instance, "vkCreateDevice");
    const auto get_device_queue = load<PFN_vkGetDeviceQueue>(gipa, instance, "vkGetDeviceQueue");
    const auto create_win32_surface = load<PFN_vkCreateWin32SurfaceKHR>(gipa, instance, "vkCreateWin32SurfaceKHR");
    const auto create_swapchain = load<PFN_vkCreateSwapchainKHR>(gipa, instance, "vkCreateSwapchainKHR");
    const auto get_swapchain_images = load<PFN_vkGetSwapchainImagesKHR>(gipa, instance, "vkGetSwapchainImagesKHR");
    const auto acquire_next = load<PFN_vkAcquireNextImageKHR>(gipa, instance, "vkAcquireNextImageKHR");
    const auto queue_present = load<PFN_vkQueuePresentKHR>(gipa, instance, "vkQueuePresentKHR");
    const auto create_command_pool = load<PFN_vkCreateCommandPool>(gipa, instance, "vkCreateCommandPool");
    const auto allocate_command_buffers = load<PFN_vkAllocateCommandBuffers>(gipa, instance, "vkAllocateCommandBuffers");
    const auto begin_command_buffer = load<PFN_vkBeginCommandBuffer>(gipa, instance, "vkBeginCommandBuffer");
    const auto end_command_buffer = load<PFN_vkEndCommandBuffer>(gipa, instance, "vkEndCommandBuffer");
    const auto queue_submit = load<PFN_vkQueueSubmit>(gipa, instance, "vkQueueSubmit");
    const auto create_fence = load<PFN_vkCreateFence>(gipa, instance, "vkCreateFence");
    const auto reset_fences = load<PFN_vkResetFences>(gipa, instance, "vkResetFences");
    const auto wait_for_fences = load<PFN_vkWaitForFences>(gipa, instance, "vkWaitForFences");
    const auto destroy_fence = load<PFN_vkDestroyFence>(gipa, instance, "vkDestroyFence");
    const auto create_buffer = load<PFN_vkCreateBuffer>(gipa, instance, "vkCreateBuffer");
    const auto get_buffer_memory_requirements =
        load<PFN_vkGetBufferMemoryRequirements>(gipa, instance, "vkGetBufferMemoryRequirements");
    const auto allocate_memory = load<PFN_vkAllocateMemory>(gipa, instance, "vkAllocateMemory");
    const auto bind_buffer_memory = load<PFN_vkBindBufferMemory>(gipa, instance, "vkBindBufferMemory");
    const auto map_memory = load<PFN_vkMapMemory>(gipa, instance, "vkMapMemory");
    const auto unmap_memory = load<PFN_vkUnmapMemory>(gipa, instance, "vkUnmapMemory");
    const auto destroy_buffer = load<PFN_vkDestroyBuffer>(gipa, instance, "vkDestroyBuffer");
    const auto free_memory = load<PFN_vkFreeMemory>(gipa, instance, "vkFreeMemory");
    const auto create_image = load<PFN_vkCreateImage>(gipa, instance, "vkCreateImage");
    const auto get_image_memory_requirements =
        load<PFN_vkGetImageMemoryRequirements>(gipa, instance, "vkGetImageMemoryRequirements");
    const auto bind_image_memory = load<PFN_vkBindImageMemory>(gipa, instance, "vkBindImageMemory");
    const auto destroy_image = load<PFN_vkDestroyImage>(gipa, instance, "vkDestroyImage");
    const auto create_image_view = load<PFN_vkCreateImageView>(gipa, instance, "vkCreateImageView");
    const auto destroy_image_view_fn = load<PFN_vkDestroyImageView>(gipa, instance, "vkDestroyImageView");
    const auto create_sampler = load<PFN_vkCreateSampler>(gipa, instance, "vkCreateSampler");
    const auto destroy_sampler = load<PFN_vkDestroySampler>(gipa, instance, "vkDestroySampler");
    const auto create_render_pass = load<PFN_vkCreateRenderPass>(gipa, instance, "vkCreateRenderPass");
    const auto create_framebuffer = load<PFN_vkCreateFramebuffer>(gipa, instance, "vkCreateFramebuffer");
    const auto create_shader_module = load<PFN_vkCreateShaderModule>(gipa, instance, "vkCreateShaderModule");
    const auto create_pipeline_layout = load<PFN_vkCreatePipelineLayout>(gipa, instance, "vkCreatePipelineLayout");
    const auto create_graphics_pipelines = load<PFN_vkCreateGraphicsPipelines>(gipa, instance, "vkCreateGraphicsPipelines");
    const auto create_descriptor_set_layout =
        load<PFN_vkCreateDescriptorSetLayout>(gipa, instance, "vkCreateDescriptorSetLayout");
    const auto create_descriptor_pool = load<PFN_vkCreateDescriptorPool>(gipa, instance, "vkCreateDescriptorPool");
    const auto allocate_descriptor_sets = load<PFN_vkAllocateDescriptorSets>(gipa, instance, "vkAllocateDescriptorSets");
    const auto update_descriptor_sets = load<PFN_vkUpdateDescriptorSets>(gipa, instance, "vkUpdateDescriptorSets");
    const auto cmd_pipeline_barrier = load<PFN_vkCmdPipelineBarrier>(gipa, instance, "vkCmdPipelineBarrier");
    const auto cmd_copy_buffer_to_image = load<PFN_vkCmdCopyBufferToImage>(gipa, instance, "vkCmdCopyBufferToImage");
    const auto cmd_begin_render_pass = load<PFN_vkCmdBeginRenderPass>(gipa, instance, "vkCmdBeginRenderPass");
    const auto cmd_bind_pipeline = load<PFN_vkCmdBindPipeline>(gipa, instance, "vkCmdBindPipeline");
    const auto cmd_bind_descriptor_sets = load<PFN_vkCmdBindDescriptorSets>(gipa, instance, "vkCmdBindDescriptorSets");
    const auto cmd_push_constants = load<PFN_vkCmdPushConstants>(gipa, instance, "vkCmdPushConstants");
    const auto cmd_bind_vertex_buffers = load<PFN_vkCmdBindVertexBuffers>(gipa, instance, "vkCmdBindVertexBuffers");
    const auto cmd_bind_index_buffer = load<PFN_vkCmdBindIndexBuffer>(gipa, instance, "vkCmdBindIndexBuffer");
    const auto cmd_draw_indexed = load<PFN_vkCmdDrawIndexed>(gipa, instance, "vkCmdDrawIndexed");
    const auto cmd_end_render_pass = load<PFN_vkCmdEndRenderPass>(gipa, instance, "vkCmdEndRenderPass");

    uint32_t device_count = 0;
    enumerate(instance, &device_count, nullptr);
    if (device_count == 0)
    {
        std::printf("[vk-tex] no physical devices\n");
        return 6;
    }
    std::vector<VkPhysicalDevice> physical_devices(device_count);
    enumerate(instance, &device_count, physical_devices.data());
    const VkPhysicalDevice physical_device = physical_devices[0];

    uint32_t family_count = 0;
    get_queue_families(physical_device, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    get_queue_families(physical_device, &family_count, families.data());
    uint32_t graphics_family = UINT32_MAX;
    for (uint32_t i = 0; i < family_count; ++i)
    {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
        {
            graphics_family = i;
            break;
        }
    }
    if (graphics_family == UINT32_MAX)
    {
        std::printf("[vk-tex] no graphics queue family\n");
        return 7;
    }

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = graphics_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    const std::array<const char*, 1> device_exts = {"VK_KHR_swapchain"};
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = static_cast<uint32_t>(device_exts.size());
    dci.ppEnabledExtensionNames = device_exts.data();

    VkDevice device = VK_NULL_HANDLE;
    if (create_device(physical_device, &dci, nullptr, &device) != VK_SUCCESS || !device)
    {
        std::printf("[vk-tex] vkCreateDevice failed\n");
        return 8;
    }

    VkQueue queue = VK_NULL_HANDLE;
    get_device_queue(device, graphics_family, 0, &queue);

    VkPhysicalDeviceMemoryProperties mem_props{};
    get_memory_properties(physical_device, &mem_props);
    const auto find_mem = [&](uint32_t type_bits, VkMemoryPropertyFlags want) -> uint32_t {
        for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i)
        {
            if ((type_bits & (1u << i)) && (mem_props.memoryTypes[i].propertyFlags & want) == want)
            {
                return i;
            }
        }
        return UINT32_MAX;
    };
    constexpr VkMemoryPropertyFlags host_visible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    const auto make_buffer = [&](VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& out_buffer,
                                 VkDeviceMemory& out_memory) -> bool {
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = size;
        bci.usage = usage;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (create_buffer(device, &bci, nullptr, &out_buffer) != VK_SUCCESS)
        {
            return false;
        }
        VkMemoryRequirements reqs{};
        get_buffer_memory_requirements(device, out_buffer, &reqs);
        const uint32_t type_index = find_mem(reqs.memoryTypeBits, host_visible);
        if (type_index == UINT32_MAX)
        {
            return false;
        }
        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = reqs.size;
        mai.memoryTypeIndex = type_index;
        if (allocate_memory(device, &mai, nullptr, &out_memory) != VK_SUCCESS)
        {
            return false;
        }
        bind_buffer_memory(device, out_buffer, out_memory, 0);
        return true;
    };
    const auto fill_buffer = [&](VkDeviceMemory memory, VkDeviceSize size, const void* src) {
        void* mapped = nullptr;
        if (map_memory(device, memory, 0, size, 0, &mapped) == VK_SUCCESS && mapped)
        {
            std::memcpy(mapped, src, static_cast<size_t>(size));
            unmap_memory(device, memory);
        }
    };

    // --- vertex + index buffers ---
    VkBuffer vertex_buffer = VK_NULL_HANDLE;
    VkDeviceMemory vertex_memory = VK_NULL_HANDLE;
    VkBuffer index_buffer = VK_NULL_HANDLE;
    VkDeviceMemory index_memory = VK_NULL_HANDLE;
    if (!make_buffer(sizeof(vertices), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertex_buffer, vertex_memory) ||
        !make_buffer(sizeof(indices), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, index_buffer, index_memory))
    {
        std::printf("[vk-tex] failed to create vertex/index buffers\n");
        return 9;
    }
    fill_buffer(vertex_memory, sizeof(vertices), vertices.data());
    fill_buffer(index_memory, sizeof(indices), indices.data());

    // --- texture image + staging upload ---
    const std::vector<uint8_t> pixels = make_checkerboard();
    const auto image_bytes = static_cast<VkDeviceSize>(pixels.size());

    VkBuffer staging_buffer = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    if (!make_buffer(image_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging_buffer, staging_memory))
    {
        std::printf("[vk-tex] failed to create staging buffer\n");
        return 10;
    }
    fill_buffer(staging_memory, image_bytes, pixels.data());

    VkImageCreateInfo tici{};
    tici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    tici.imageType = VK_IMAGE_TYPE_2D;
    tici.format = VK_FORMAT_R8G8B8A8_UNORM;
    tici.extent = {.width = tex_size, .height = tex_size, .depth = 1};
    tici.mipLevels = 1;
    tici.arrayLayers = 1;
    tici.samples = VK_SAMPLE_COUNT_1_BIT;
    tici.tiling = VK_IMAGE_TILING_OPTIMAL;
    tici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    tici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    tici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage texture = VK_NULL_HANDLE;
    if (create_image(device, &tici, nullptr, &texture) != VK_SUCCESS)
    {
        std::printf("[vk-tex] vkCreateImage failed\n");
        return 11;
    }
    VkMemoryRequirements tex_reqs{};
    get_image_memory_requirements(device, texture, &tex_reqs);
    uint32_t tex_type = find_mem(tex_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (tex_type == UINT32_MAX)
    {
        tex_type = find_mem(tex_reqs.memoryTypeBits, 0);
    }
    VkMemoryAllocateInfo tex_alloc{};
    tex_alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    tex_alloc.allocationSize = tex_reqs.size;
    tex_alloc.memoryTypeIndex = tex_type;
    VkDeviceMemory texture_memory = VK_NULL_HANDLE;
    if (allocate_memory(device, &tex_alloc, nullptr, &texture_memory) != VK_SUCCESS)
    {
        std::printf("[vk-tex] texture vkAllocateMemory failed\n");
        return 12;
    }
    bind_image_memory(device, texture, texture_memory, 0);

    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = graphics_family;
    VkCommandPool pool = VK_NULL_HANDLE;
    create_command_pool(device, &pci, nullptr, &pool);

    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    allocate_command_buffers(device, &cbai, &cmd);

    VkFenceCreateInfo fence_ci{};
    fence_ci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence frame_fence = VK_NULL_HANDLE;
    create_fence(device, &fence_ci, nullptr, &frame_fence);

    // One-time upload: UNDEFINED -> TRANSFER_DST, copy, TRANSFER_DST -> SHADER_READ_ONLY.
    {
        const auto image_barrier = [&](VkImageLayout oldL, VkImageLayout newL, VkAccessFlags srcA, VkAccessFlags dstA,
                                       VkPipelineStageFlags srcS, VkPipelineStageFlags dstS) {
            VkImageMemoryBarrier b{};
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.srcAccessMask = srcA;
            b.dstAccessMask = dstA;
            b.oldLayout = oldL;
            b.newLayout = newL;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = texture;
            b.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                  .baseMipLevel = 0,
                                  .levelCount = 1,
                                  .baseArrayLayer = 0,
                                  .layerCount = 1};
            cmd_pipeline_barrier(cmd, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b);
        };

        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        begin_command_buffer(cmd, &begin);

        image_barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {.x = 0, .y = 0, .z = 0};
        region.imageExtent = {.width = tex_size, .height = tex_size, .depth = 1};
        cmd_copy_buffer_to_image(cmd, staging_buffer, texture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        image_barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

        end_command_buffer(cmd);

        reset_fences(device, 1, &frame_fence);
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        queue_submit(queue, 1, &submit, frame_fence);
        wait_for_fences(device, 1, &frame_fence, VK_TRUE, UINT64_MAX);
    }

    // --- image view + sampler ---
    VkImageViewCreateInfo tivci{};
    tivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    tivci.image = texture;
    tivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    tivci.format = VK_FORMAT_R8G8B8A8_UNORM;
    tivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    tivci.subresourceRange.levelCount = 1;
    tivci.subresourceRange.layerCount = 1;
    VkImageView texture_view = VK_NULL_HANDLE;
    create_image_view(device, &tivci, nullptr, &texture_view);

    VkSamplerCreateInfo smci{};
    smci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    smci.magFilter = VK_FILTER_LINEAR;
    smci.minFilter = VK_FILTER_LINEAR;
    smci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    smci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    smci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    VkSampler sampler = VK_NULL_HANDLE;
    if (create_sampler(device, &smci, nullptr, &sampler) != VK_SUCCESS)
    {
        std::printf("[vk-tex] vkCreateSampler failed\n");
        return 13;
    }

    // --- descriptor set layout / pool / set (combined image sampler) ---
    VkDescriptorSetLayoutBinding tex_binding{};
    tex_binding.binding = 0;
    tex_binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    tex_binding.descriptorCount = 1;
    tex_binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dslci{};
    dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslci.bindingCount = 1;
    dslci.pBindings = &tex_binding;
    VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
    if (create_descriptor_set_layout(device, &dslci, nullptr, &descriptor_set_layout) != VK_SUCCESS)
    {
        std::printf("[vk-tex] vkCreateDescriptorSetLayout failed\n");
        return 14;
    }

    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_size.descriptorCount = 1;
    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 1;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &pool_size;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    if (create_descriptor_pool(device, &dpci, nullptr, &descriptor_pool) != VK_SUCCESS)
    {
        std::printf("[vk-tex] vkCreateDescriptorPool failed\n");
        return 15;
    }

    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = descriptor_pool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &descriptor_set_layout;
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    if (allocate_descriptor_sets(device, &dsai, &descriptor_set) != VK_SUCCESS)
    {
        std::printf("[vk-tex] vkAllocateDescriptorSets failed\n");
        return 16;
    }

    VkDescriptorImageInfo image_info{};
    image_info.sampler = sampler;
    image_info.imageView = texture_view;
    image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = descriptor_set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image_info;
    update_descriptor_sets(device, 1, &write, 0, nullptr);

    // --- surface + swapchain ---
    VkWin32SurfaceCreateInfoKHR sci{};
    sci.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    sci.hinstance = hinstance;
    sci.hwnd = hwnd;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (create_win32_surface(instance, &sci, nullptr, &surface) != VK_SUCCESS)
    {
        std::printf("[vk-tex] vkCreateWin32SurfaceKHR failed\n");
        return 17;
    }

    const auto get_surface_caps =
        load<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(gipa, instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    VkSurfaceCapabilitiesKHR caps{};
    get_surface_caps(physical_device, surface, &caps);

    VkExtent2D swap_extent = caps.currentExtent;
    if (swap_extent.width == 0xFFFFFFFFu)
    {
        RECT client{};
        GetClientRect(hwnd, &client);
        swap_extent.width = static_cast<uint32_t>(client.right - client.left);
        swap_extent.height = static_cast<uint32_t>(client.bottom - client.top);
        if (swap_extent.width == 0)
        {
            swap_extent.width = window_width;
        }
        if (swap_extent.height == 0)
        {
            swap_extent.height = window_height;
        }
    }

    uint32_t min_image_count = (caps.minImageCount < 2) ? 2 : caps.minImageCount;
    if (caps.maxImageCount != 0 && min_image_count > caps.maxImageCount)
    {
        min_image_count = caps.maxImageCount;
    }

    VkSwapchainCreateInfoKHR swci{};
    swci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swci.surface = surface;
    swci.minImageCount = min_image_count;
    swci.imageFormat = swapchain_format;
    swci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    swci.imageExtent = swap_extent;
    swci.imageArrayLayers = 1;
    swci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    swci.preTransform = caps.currentTransform;
    swci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swci.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    swci.clipped = VK_TRUE;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    if (create_swapchain(device, &swci, nullptr, &swapchain) != VK_SUCCESS || !swapchain)
    {
        std::printf("[vk-tex] vkCreateSwapchainKHR failed\n");
        return 18;
    }

    uint32_t image_count = 0;
    get_swapchain_images(device, swapchain, &image_count, nullptr);
    std::vector<VkImage> images(image_count);
    get_swapchain_images(device, swapchain, &image_count, images.data());
    std::printf("[vk-tex] swapchain ready: %u images, %ux%u\n", image_count, swap_extent.width, swap_extent.height);

    // --- render pass ---
    VkAttachmentDescription color_attachment{};
    color_attachment.format = swapchain_format;
    color_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color_attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference color_ref{};
    color_ref.attachment = 0;
    color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;
    VkRenderPassCreateInfo rpci{};
    rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = 1;
    rpci.pAttachments = &color_attachment;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &subpass;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    if (create_render_pass(device, &rpci, nullptr, &render_pass) != VK_SUCCESS)
    {
        std::printf("[vk-tex] vkCreateRenderPass failed\n");
        return 19;
    }

    // --- shader modules ---
    VkShaderModuleCreateInfo vert_ci{};
    vert_ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    vert_ci.codeSize = texture_vert_spv.size() * sizeof(uint32_t);
    vert_ci.pCode = texture_vert_spv.data();
    VkShaderModule vert_module = VK_NULL_HANDLE;
    create_shader_module(device, &vert_ci, nullptr, &vert_module);

    VkShaderModuleCreateInfo frag_ci{};
    frag_ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    frag_ci.codeSize = texture_frag_spv.size() * sizeof(uint32_t);
    frag_ci.pCode = texture_frag_spv.data();
    VkShaderModule frag_module = VK_NULL_HANDLE;
    create_shader_module(device, &frag_ci, nullptr, &frag_module);

    // --- pipeline layout: the texture descriptor set + a push-constant float (rotation angle) ---
    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push_range.offset = 0;
    push_range.size = sizeof(float);
    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &descriptor_set_layout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &push_range;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    create_pipeline_layout(device, &plci, nullptr, &pipeline_layout);

    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert_module;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag_module;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(Vertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    std::array<VkVertexInputAttributeDescription, 2> attributes{};
    attributes[0].location = 0;
    attributes[0].binding = 0;
    attributes[0].format = VK_FORMAT_R32G32_SFLOAT;
    attributes[0].offset = offsetof(Vertex, pos);
    attributes[1].location = 1;
    attributes[1].binding = 0;
    attributes[1].format = VK_FORMAT_R32G32_SFLOAT;
    attributes[1].offset = offsetof(Vertex, uv);

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &binding;
    vertex_input.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
    vertex_input.pVertexAttributeDescriptions = attributes.data();

    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport viewport{};
    viewport.width = static_cast<float>(swap_extent.width);
    viewport.height = static_cast<float>(swap_extent.height);
    viewport.maxDepth = 1.0f;
    VkRect2D scissor{};
    scissor.extent = swap_extent;
    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.pViewports = &viewport;
    viewport_state.scissorCount = 1;
    viewport_state.pScissors = &scissor;
    VkPipelineRasterizationStateCreateInfo rasterization{};
    rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                                      VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo color_blend{};
    color_blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blend.attachmentCount = 1;
    color_blend.pAttachments = &blend_attachment;

    VkGraphicsPipelineCreateInfo gpci{};
    gpci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpci.stageCount = static_cast<uint32_t>(stages.size());
    gpci.pStages = stages.data();
    gpci.pVertexInputState = &vertex_input;
    gpci.pInputAssemblyState = &input_assembly;
    gpci.pViewportState = &viewport_state;
    gpci.pRasterizationState = &rasterization;
    gpci.pMultisampleState = &multisample;
    gpci.pColorBlendState = &color_blend;
    gpci.layout = pipeline_layout;
    gpci.renderPass = render_pass;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (create_graphics_pipelines(device, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipeline) != VK_SUCCESS || !pipeline)
    {
        std::printf("[vk-tex] vkCreateGraphicsPipelines failed\n");
        return 20;
    }

    std::vector<VkImageView> image_views(image_count);
    std::vector<VkFramebuffer> framebuffers(image_count);
    for (uint32_t i = 0; i < image_count; ++i)
    {
        VkImageViewCreateInfo ivci{};
        ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ivci.image = images[i];
        ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivci.format = swapchain_format;
        ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ivci.subresourceRange.levelCount = 1;
        ivci.subresourceRange.layerCount = 1;
        create_image_view(device, &ivci, nullptr, &image_views[i]);

        VkFramebufferCreateInfo fbci{};
        fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbci.renderPass = render_pass;
        fbci.attachmentCount = 1;
        fbci.pAttachments = &image_views[i];
        fbci.width = swap_extent.width;
        fbci.height = swap_extent.height;
        fbci.layers = 1;
        create_framebuffer(device, &fbci, nullptr, &framebuffers[i]);
    }

    constexpr uint32_t fps_window_size = 30;
    const uint64_t start_ms = now_ms();
    uint64_t fps_window_start = start_ms;
    uint32_t fps_window_frames = 0;
    uint32_t frame = 0;
    while (!g_quit && frame < max_frames)
    {
        MSG msg{};
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                g_quit = true;
            }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (g_quit)
        {
            break;
        }

        uint32_t image_index = 0;
        const VkResult acquired = acquire_next(device, swapchain, UINT64_MAX, VK_NULL_HANDLE, VK_NULL_HANDLE, &image_index);
        if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR)
        {
            std::printf("[vk-tex] vkAcquireNextImageKHR -> %d\n", acquired);
            break;
        }

        const uint64_t now = now_ms();
        const float angle = static_cast<float>(now - start_ms) / 1000.0f * rotation_speed;

        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        begin_command_buffer(cmd, &begin);

        VkClearValue clear{};
        clear.color.float32[0] = 0.08f;
        clear.color.float32[1] = 0.08f;
        clear.color.float32[2] = 0.12f;
        clear.color.float32[3] = 1.0f;

        VkRenderPassBeginInfo rpbi{};
        rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpbi.renderPass = render_pass;
        rpbi.framebuffer = framebuffers[image_index];
        rpbi.renderArea.extent = swap_extent;
        rpbi.clearValueCount = 1;
        rpbi.pClearValues = &clear;
        cmd_begin_render_pass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
        cmd_bind_pipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        cmd_bind_descriptor_sets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1, &descriptor_set, 0, nullptr);
        cmd_push_constants(cmd, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float), &angle);
        const VkDeviceSize vertex_offset = 0;
        cmd_bind_vertex_buffers(cmd, 0, 1, &vertex_buffer, &vertex_offset);
        cmd_bind_index_buffer(cmd, index_buffer, 0, VK_INDEX_TYPE_UINT16);
        cmd_draw_indexed(cmd, static_cast<uint32_t>(indices.size()), 1, 0, 0, 0);
        cmd_end_render_pass(cmd);

        end_command_buffer(cmd);

        reset_fences(device, 1, &frame_fence);
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        queue_submit(queue, 1, &submit, frame_fence);
        wait_for_fences(device, 1, &frame_fence, VK_TRUE, UINT64_MAX);

        VkPresentInfoKHR present{};
        present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain;
        present.pImageIndices = &image_index;
        const VkResult presented = queue_present(queue, &present);
        if (presented != VK_SUCCESS && presented != VK_SUBOPTIMAL_KHR)
        {
            std::printf("[vk-tex] vkQueuePresentKHR -> %d\n", presented);
            break;
        }

        ++fps_window_frames;
        if (fps_window_frames >= fps_window_size)
        {
            const ULONGLONG fps_elapsed = (now > fps_window_start) ? (now - fps_window_start) : 1;
            const double fps = static_cast<double>(fps_window_frames) * 1000.0 / static_cast<double>(fps_elapsed);
            const double ms_per_frame = static_cast<double>(fps_elapsed) / static_cast<double>(fps_window_frames);
            std::array<char, 128> title{};
            std::snprintf(title.data(), title.size(), "Sogen Vulkan - Texture - %.2f FPS", fps);
            SetWindowTextA(hwnd, title.data());
            std::printf("[vk-tex] %.2f FPS (%.0f ms/frame, frame %u)\n", fps, ms_per_frame, frame);
            fps_window_frames = 0;
            fps_window_start = now;
        }

        ++frame;
    }

    std::printf("[vk-tex] done after %u frames\n", frame);

    const auto destroy_framebuffer = load<PFN_vkDestroyFramebuffer>(gipa, instance, "vkDestroyFramebuffer");
    const auto destroy_pipeline = load<PFN_vkDestroyPipeline>(gipa, instance, "vkDestroyPipeline");
    const auto destroy_pipeline_layout = load<PFN_vkDestroyPipelineLayout>(gipa, instance, "vkDestroyPipelineLayout");
    const auto destroy_descriptor_pool = load<PFN_vkDestroyDescriptorPool>(gipa, instance, "vkDestroyDescriptorPool");
    const auto destroy_descriptor_set_layout =
        load<PFN_vkDestroyDescriptorSetLayout>(gipa, instance, "vkDestroyDescriptorSetLayout");
    const auto destroy_shader_module = load<PFN_vkDestroyShaderModule>(gipa, instance, "vkDestroyShaderModule");
    const auto destroy_render_pass = load<PFN_vkDestroyRenderPass>(gipa, instance, "vkDestroyRenderPass");
    const auto destroy_swapchain = load<PFN_vkDestroySwapchainKHR>(gipa, instance, "vkDestroySwapchainKHR");
    const auto destroy_surface = load<PFN_vkDestroySurfaceKHR>(gipa, instance, "vkDestroySurfaceKHR");
    const auto destroy_command_pool = load<PFN_vkDestroyCommandPool>(gipa, instance, "vkDestroyCommandPool");
    const auto destroy_device = load<PFN_vkDestroyDevice>(gipa, instance, "vkDestroyDevice");
    const auto destroy_instance = load<PFN_vkDestroyInstance>(gipa, instance, "vkDestroyInstance");

    for (uint32_t i = 0; i < image_count; ++i)
    {
        destroy_framebuffer(device, framebuffers[i], nullptr);
        destroy_image_view_fn(device, image_views[i], nullptr);
    }
    destroy_pipeline(device, pipeline, nullptr);
    destroy_pipeline_layout(device, pipeline_layout, nullptr);
    destroy_descriptor_pool(device, descriptor_pool, nullptr);
    destroy_descriptor_set_layout(device, descriptor_set_layout, nullptr);
    destroy_shader_module(device, frag_module, nullptr);
    destroy_shader_module(device, vert_module, nullptr);
    destroy_render_pass(device, render_pass, nullptr);
    destroy_sampler(device, sampler, nullptr);
    destroy_image_view_fn(device, texture_view, nullptr);
    destroy_image(device, texture, nullptr);
    free_memory(device, texture_memory, nullptr);
    destroy_buffer(device, staging_buffer, nullptr);
    free_memory(device, staging_memory, nullptr);
    destroy_fence(device, frame_fence, nullptr);
    destroy_command_pool(device, pool, nullptr);
    destroy_buffer(device, index_buffer, nullptr);
    free_memory(device, index_memory, nullptr);
    destroy_buffer(device, vertex_buffer, nullptr);
    free_memory(device, vertex_memory, nullptr);
    destroy_swapchain(device, swapchain, nullptr);
    destroy_surface(instance, surface, nullptr);
    destroy_device(device, nullptr);
    destroy_instance(instance, nullptr);
    return 0;
}
