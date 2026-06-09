// A windowed Vulkan guest app that draws a real 3D cube with correct occlusion from a *depth buffer*
// -- no CPU face sorting (unlike vulkan-cube-sample, which predates depth support in the bridge). It
// exercises the bridge's depth plumbing: a render pass with a depth attachment, a depth image + view
// (DEPTH aspect), a pipeline with depth test/write enabled, and a depth clear. Geometry (per-vertex
// position + color) comes from a vertex + index buffer; a 64-byte mat4 MVP push constant computed on
// the CPU spins it. The same binary runs through the shim and natively against a real driver.

#include <windows.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan_win32.h>

#include "depth_cube_spirv.hxx"

namespace
{
    constexpr uint32_t window_width = 400;
    constexpr uint32_t window_height = 400;
    constexpr VkFormat swapchain_format = VK_FORMAT_B8G8R8A8_UNORM;
    constexpr VkFormat depth_format = VK_FORMAT_D32_SFLOAT;
    // Rotation is driven by real wall-clock time (radians/second), not the frame count, so the cube
    // spins at the same physical rate natively and emulated -- otherwise a faster frame rate would spin
    // it faster, making a host-vs-emulated comparison misleading.
    constexpr float rotation_speed = 0.6f;

    using mat4 = std::array<float, 16>; // column-major

    constexpr size_t mat_index(size_t row, size_t col)
    {
        return col * 4 + row;
    }

    mat4 mat_multiply(const mat4& a, const mat4& b)
    {
        mat4 out{};
        for (size_t col = 0; col < 4; ++col)
        {
            for (size_t row = 0; row < 4; ++row)
            {
                float sum = 0.0f;
                for (size_t k = 0; k < 4; ++k)
                {
                    sum += a[mat_index(row, k)] * b[mat_index(k, col)];
                }
                out[mat_index(row, col)] = sum;
            }
        }
        return out;
    }

    mat4 mat_translate(float x, float y, float z)
    {
        mat4 m{};
        m[mat_index(0, 0)] = 1.0f;
        m[mat_index(1, 1)] = 1.0f;
        m[mat_index(2, 2)] = 1.0f;
        m[mat_index(3, 3)] = 1.0f;
        m[mat_index(0, 3)] = x;
        m[mat_index(1, 3)] = y;
        m[mat_index(2, 3)] = z;
        return m;
    }

    mat4 mat_rotate_y(float a)
    {
        const float c = std::cos(a);
        const float s = std::sin(a);
        mat4 m{};
        m[mat_index(0, 0)] = c;
        m[mat_index(0, 2)] = s;
        m[mat_index(1, 1)] = 1.0f;
        m[mat_index(2, 0)] = -s;
        m[mat_index(2, 2)] = c;
        m[mat_index(3, 3)] = 1.0f;
        return m;
    }

    mat4 mat_rotate_x(float a)
    {
        const float c = std::cos(a);
        const float s = std::sin(a);
        mat4 m{};
        m[mat_index(0, 0)] = 1.0f;
        m[mat_index(1, 1)] = c;
        m[mat_index(1, 2)] = -s;
        m[mat_index(2, 1)] = s;
        m[mat_index(2, 2)] = c;
        m[mat_index(3, 3)] = 1.0f;
        return m;
    }

    // Right-handed perspective, clip-space depth in [0, 1], Y flipped for Vulkan.
    mat4 mat_perspective(float fovy, float aspect, float z_near, float z_far)
    {
        const float f = 1.0f / std::tan(fovy * 0.5f);
        mat4 m{};
        m[mat_index(0, 0)] = f / aspect;
        m[mat_index(1, 1)] = -f;
        m[mat_index(2, 2)] = z_far / (z_near - z_far);
        m[mat_index(3, 2)] = -1.0f;
        m[mat_index(2, 3)] = (z_far * z_near) / (z_near - z_far);
        return m;
    }

    struct Vertex
    {
        std::array<float, 3> pos;
        std::array<float, 3> color;
    };

    // 24 vertices (4 per face), one solid color per face. Faces: +X, -X, +Y, -Y, +Z, -Z.
    constexpr std::array<Vertex, 24> vertices = {{
        // +X (red)
        {.pos = {0.5f, -0.5f, -0.5f}, .color = {0.9f, 0.2f, 0.2f}},
        {.pos = {0.5f, -0.5f, 0.5f}, .color = {0.9f, 0.2f, 0.2f}},
        {.pos = {0.5f, 0.5f, 0.5f}, .color = {0.9f, 0.2f, 0.2f}},
        {.pos = {0.5f, 0.5f, -0.5f}, .color = {0.9f, 0.2f, 0.2f}},
        // -X (green)
        {.pos = {-0.5f, -0.5f, 0.5f}, .color = {0.2f, 0.8f, 0.2f}},
        {.pos = {-0.5f, -0.5f, -0.5f}, .color = {0.2f, 0.8f, 0.2f}},
        {.pos = {-0.5f, 0.5f, -0.5f}, .color = {0.2f, 0.8f, 0.2f}},
        {.pos = {-0.5f, 0.5f, 0.5f}, .color = {0.2f, 0.8f, 0.2f}},
        // +Y (blue)
        {.pos = {-0.5f, 0.5f, -0.5f}, .color = {0.3f, 0.4f, 0.9f}},
        {.pos = {0.5f, 0.5f, -0.5f}, .color = {0.3f, 0.4f, 0.9f}},
        {.pos = {0.5f, 0.5f, 0.5f}, .color = {0.3f, 0.4f, 0.9f}},
        {.pos = {-0.5f, 0.5f, 0.5f}, .color = {0.3f, 0.4f, 0.9f}},
        // -Y (yellow)
        {.pos = {-0.5f, -0.5f, 0.5f}, .color = {0.9f, 0.8f, 0.2f}},
        {.pos = {0.5f, -0.5f, 0.5f}, .color = {0.9f, 0.8f, 0.2f}},
        {.pos = {0.5f, -0.5f, -0.5f}, .color = {0.9f, 0.8f, 0.2f}},
        {.pos = {-0.5f, -0.5f, -0.5f}, .color = {0.9f, 0.8f, 0.2f}},
        // +Z (magenta)
        {.pos = {-0.5f, -0.5f, 0.5f}, .color = {0.9f, 0.3f, 0.8f}},
        {.pos = {0.5f, -0.5f, 0.5f}, .color = {0.9f, 0.3f, 0.8f}},
        {.pos = {0.5f, 0.5f, 0.5f}, .color = {0.9f, 0.3f, 0.8f}},
        {.pos = {-0.5f, 0.5f, 0.5f}, .color = {0.9f, 0.3f, 0.8f}},
        // -Z (cyan)
        {.pos = {0.5f, -0.5f, -0.5f}, .color = {0.2f, 0.8f, 0.85f}},
        {.pos = {-0.5f, -0.5f, -0.5f}, .color = {0.2f, 0.8f, 0.85f}},
        {.pos = {-0.5f, 0.5f, -0.5f}, .color = {0.2f, 0.8f, 0.85f}},
        {.pos = {0.5f, 0.5f, -0.5f}, .color = {0.2f, 0.8f, 0.85f}},
    }};

    // Two triangles per face: (0,1,2),(2,3,0) offset by face*4.
    constexpr std::array<uint16_t, 36> make_indices()
    {
        std::array<uint16_t, 36> idx{};
        for (uint16_t face = 0; face < 6; ++face)
        {
            const auto base = static_cast<uint16_t>(face * 4);
            const size_t o = static_cast<size_t>(face) * 6;
            idx[o + 0] = base + 0;
            idx[o + 1] = base + 1;
            idx[o + 2] = base + 2;
            idx[o + 3] = base + 2;
            idx[o + 4] = base + 3;
            idx[o + 5] = base + 0;
        }
        return idx;
    }
    constexpr std::array<uint16_t, 36> indices = make_indices();

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
    wc.lpszClassName = "SogenVulkanDepthCube";
    wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    if (!RegisterClassExA(&wc))
    {
        std::printf("[vk-depth] RegisterClassExA failed: %lu\n", GetLastError());
        return 1;
    }

    constexpr DWORD window_style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE;
    const HWND hwnd =
        CreateWindowExA(0, wc.lpszClassName, "Sogen Vulkan - Depth Cube", window_style, 200, 200, static_cast<int>(window_width),
                        static_cast<int>(window_height), nullptr, nullptr, hinstance, nullptr);
    if (!hwnd)
    {
        std::printf("[vk-depth] CreateWindowExA failed: %lu\n", GetLastError());
        return 2;
    }

    const HMODULE mod = LoadLibraryA(dll);
    if (!mod)
    {
        std::printf("[vk-depth] LoadLibrary(%s) failed: %lu\n", dll, GetLastError());
        return 3;
    }
    const auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(reinterpret_cast<void*>(GetProcAddress(mod, "vkGetInstanceProcAddr")));
    if (!gipa)
    {
        std::printf("[vk-depth] no vkGetInstanceProcAddr export\n");
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
        std::printf("[vk-depth] vkCreateInstance failed (no host Vulkan driver?)\n");
        return 5;
    }

    const auto enumerate = load<PFN_vkEnumeratePhysicalDevices>(gipa, instance, "vkEnumeratePhysicalDevices");
    const auto get_queue_families =
        load<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(gipa, instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    const auto get_memory_properties = load<PFN_vkGetPhysicalDeviceMemoryProperties>(gipa, instance, "vkGetPhysicalDeviceMemoryProperties");
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
    const auto get_buffer_memory_requirements = load<PFN_vkGetBufferMemoryRequirements>(gipa, instance, "vkGetBufferMemoryRequirements");
    const auto allocate_memory = load<PFN_vkAllocateMemory>(gipa, instance, "vkAllocateMemory");
    const auto bind_buffer_memory = load<PFN_vkBindBufferMemory>(gipa, instance, "vkBindBufferMemory");
    const auto map_memory = load<PFN_vkMapMemory>(gipa, instance, "vkMapMemory");
    const auto unmap_memory = load<PFN_vkUnmapMemory>(gipa, instance, "vkUnmapMemory");
    const auto destroy_buffer = load<PFN_vkDestroyBuffer>(gipa, instance, "vkDestroyBuffer");
    const auto free_memory = load<PFN_vkFreeMemory>(gipa, instance, "vkFreeMemory");
    const auto create_image = load<PFN_vkCreateImage>(gipa, instance, "vkCreateImage");
    const auto get_image_memory_requirements = load<PFN_vkGetImageMemoryRequirements>(gipa, instance, "vkGetImageMemoryRequirements");
    const auto bind_image_memory = load<PFN_vkBindImageMemory>(gipa, instance, "vkBindImageMemory");
    const auto destroy_image = load<PFN_vkDestroyImage>(gipa, instance, "vkDestroyImage");
    const auto create_image_view = load<PFN_vkCreateImageView>(gipa, instance, "vkCreateImageView");
    const auto destroy_image_view = load<PFN_vkDestroyImageView>(gipa, instance, "vkDestroyImageView");
    const auto create_render_pass = load<PFN_vkCreateRenderPass>(gipa, instance, "vkCreateRenderPass");
    const auto create_framebuffer = load<PFN_vkCreateFramebuffer>(gipa, instance, "vkCreateFramebuffer");
    const auto create_shader_module = load<PFN_vkCreateShaderModule>(gipa, instance, "vkCreateShaderModule");
    const auto create_pipeline_layout = load<PFN_vkCreatePipelineLayout>(gipa, instance, "vkCreatePipelineLayout");
    const auto create_graphics_pipelines = load<PFN_vkCreateGraphicsPipelines>(gipa, instance, "vkCreateGraphicsPipelines");
    const auto cmd_begin_render_pass = load<PFN_vkCmdBeginRenderPass>(gipa, instance, "vkCmdBeginRenderPass");
    const auto cmd_bind_pipeline = load<PFN_vkCmdBindPipeline>(gipa, instance, "vkCmdBindPipeline");
    const auto cmd_push_constants = load<PFN_vkCmdPushConstants>(gipa, instance, "vkCmdPushConstants");
    const auto cmd_bind_vertex_buffers = load<PFN_vkCmdBindVertexBuffers>(gipa, instance, "vkCmdBindVertexBuffers");
    const auto cmd_bind_index_buffer = load<PFN_vkCmdBindIndexBuffer>(gipa, instance, "vkCmdBindIndexBuffer");
    const auto cmd_draw_indexed = load<PFN_vkCmdDrawIndexed>(gipa, instance, "vkCmdDrawIndexed");
    const auto cmd_end_render_pass = load<PFN_vkCmdEndRenderPass>(gipa, instance, "vkCmdEndRenderPass");

    uint32_t device_count = 0;
    enumerate(instance, &device_count, nullptr);
    if (device_count == 0)
    {
        std::printf("[vk-depth] no physical devices\n");
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
        std::printf("[vk-depth] no graphics queue family\n");
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
        std::printf("[vk-depth] vkCreateDevice failed\n");
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
    const auto make_filled_buffer = [&](VkDeviceSize size, VkBufferUsageFlags usage, const void* src, VkBuffer& out_buffer,
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
        void* mapped = nullptr;
        if (map_memory(device, out_memory, 0, size, 0, &mapped) != VK_SUCCESS || !mapped)
        {
            return false;
        }
        std::memcpy(mapped, src, static_cast<size_t>(size));
        unmap_memory(device, out_memory);
        return true;
    };

    VkBuffer vertex_buffer = VK_NULL_HANDLE;
    VkDeviceMemory vertex_memory = VK_NULL_HANDLE;
    VkBuffer index_buffer = VK_NULL_HANDLE;
    VkDeviceMemory index_memory = VK_NULL_HANDLE;
    if (!make_filled_buffer(sizeof(vertices), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertices.data(), vertex_buffer, vertex_memory) ||
        !make_filled_buffer(sizeof(indices), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, indices.data(), index_buffer, index_memory))
    {
        std::printf("[vk-depth] failed to create vertex/index buffers\n");
        return 9;
    }

    VkWin32SurfaceCreateInfoKHR sci{};
    sci.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    sci.hinstance = hinstance;
    sci.hwnd = hwnd;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (create_win32_surface(instance, &sci, nullptr, &surface) != VK_SUCCESS)
    {
        std::printf("[vk-depth] vkCreateWin32SurfaceKHR failed\n");
        return 10;
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
        std::printf("[vk-depth] vkCreateSwapchainKHR failed\n");
        return 11;
    }

    uint32_t image_count = 0;
    get_swapchain_images(device, swapchain, &image_count, nullptr);
    std::vector<VkImage> images(image_count);
    get_swapchain_images(device, swapchain, &image_count, images.data());
    std::printf("[vk-depth] swapchain ready: %u images, %ux%u\n", image_count, swap_extent.width, swap_extent.height);

    // --- depth image + view (shared across frames; the loop waits per frame) ---
    VkImageCreateInfo dici2{};
    dici2.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    dici2.imageType = VK_IMAGE_TYPE_2D;
    dici2.format = depth_format;
    dici2.extent = {.width = swap_extent.width, .height = swap_extent.height, .depth = 1};
    dici2.mipLevels = 1;
    dici2.arrayLayers = 1;
    dici2.samples = VK_SAMPLE_COUNT_1_BIT;
    dici2.tiling = VK_IMAGE_TILING_OPTIMAL;
    dici2.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    dici2.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    dici2.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage depth_image = VK_NULL_HANDLE;
    if (create_image(device, &dici2, nullptr, &depth_image) != VK_SUCCESS)
    {
        std::printf("[vk-depth] depth vkCreateImage failed\n");
        return 12;
    }
    VkMemoryRequirements depth_reqs{};
    get_image_memory_requirements(device, depth_image, &depth_reqs);
    uint32_t depth_type = find_mem(depth_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (depth_type == UINT32_MAX)
    {
        depth_type = find_mem(depth_reqs.memoryTypeBits, 0);
    }
    VkMemoryAllocateInfo depth_alloc{};
    depth_alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    depth_alloc.allocationSize = depth_reqs.size;
    depth_alloc.memoryTypeIndex = depth_type;
    VkDeviceMemory depth_memory = VK_NULL_HANDLE;
    if (allocate_memory(device, &depth_alloc, nullptr, &depth_memory) != VK_SUCCESS)
    {
        std::printf("[vk-depth] depth vkAllocateMemory failed\n");
        return 13;
    }
    bind_image_memory(device, depth_image, depth_memory, 0);

    VkImageViewCreateInfo dvci{};
    dvci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    dvci.image = depth_image;
    dvci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    dvci.format = depth_format;
    dvci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    dvci.subresourceRange.levelCount = 1;
    dvci.subresourceRange.layerCount = 1;
    VkImageView depth_view = VK_NULL_HANDLE;
    create_image_view(device, &dvci, nullptr, &depth_view);

    // --- render pass: color + depth ---
    std::array<VkAttachmentDescription, 2> attachments{};
    attachments[0].format = swapchain_format;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    attachments[1].format = depth_format;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference color_ref{};
    color_ref.attachment = 0;
    color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference depth_ref{};
    depth_ref.attachment = 1;
    depth_ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;
    subpass.pDepthStencilAttachment = &depth_ref;
    VkRenderPassCreateInfo rpci{};
    rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = 2;
    rpci.pAttachments = attachments.data();
    rpci.subpassCount = 1;
    rpci.pSubpasses = &subpass;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    if (create_render_pass(device, &rpci, nullptr, &render_pass) != VK_SUCCESS)
    {
        std::printf("[vk-depth] vkCreateRenderPass failed\n");
        return 14;
    }

    // --- shaders ---
    VkShaderModuleCreateInfo vert_ci{};
    vert_ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    vert_ci.codeSize = depth_cube_vert_spv.size() * sizeof(uint32_t);
    vert_ci.pCode = depth_cube_vert_spv.data();
    VkShaderModule vert_module = VK_NULL_HANDLE;
    create_shader_module(device, &vert_ci, nullptr, &vert_module);

    VkShaderModuleCreateInfo frag_ci{};
    frag_ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    frag_ci.codeSize = depth_cube_frag_spv.size() * sizeof(uint32_t);
    frag_ci.pCode = depth_cube_frag_spv.data();
    VkShaderModule frag_module = VK_NULL_HANDLE;
    create_shader_module(device, &frag_ci, nullptr, &frag_module);

    // --- pipeline layout with a push-constant mat4 (the MVP) ---
    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push_range.offset = 0;
    push_range.size = sizeof(mat4);
    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
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
    std::array<VkVertexInputAttributeDescription, 2> attribs{};
    attribs[0].location = 0;
    attribs[0].binding = 0;
    attribs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attribs[0].offset = offsetof(Vertex, pos);
    attribs[1].location = 1;
    attribs[1].binding = 0;
    attribs[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attribs[1].offset = offsetof(Vertex, color);

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &binding;
    vertex_input.vertexAttributeDescriptionCount = static_cast<uint32_t>(attribs.size());
    vertex_input.pVertexAttributeDescriptions = attribs.data();

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
    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable = VK_TRUE;
    depth_stencil.depthWriteEnable = VK_TRUE;
    depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS;
    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
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
    gpci.pDepthStencilState = &depth_stencil;
    gpci.pColorBlendState = &color_blend;
    gpci.layout = pipeline_layout;
    gpci.renderPass = render_pass;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (create_graphics_pipelines(device, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipeline) != VK_SUCCESS || !pipeline)
    {
        std::printf("[vk-depth] vkCreateGraphicsPipelines failed\n");
        return 15;
    }

    // --- per-image color views + framebuffers (each with the shared depth view) ---
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

        const std::array<VkImageView, 2> fb_attachments = {image_views[i], depth_view};
        VkFramebufferCreateInfo fbci{};
        fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbci.renderPass = render_pass;
        fbci.attachmentCount = static_cast<uint32_t>(fb_attachments.size());
        fbci.pAttachments = fb_attachments.data();
        fbci.width = swap_extent.width;
        fbci.height = swap_extent.height;
        fbci.layers = 1;
        create_framebuffer(device, &fbci, nullptr, &framebuffers[i]);
    }

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

    const float aspect = static_cast<float>(swap_extent.width) / static_cast<float>(swap_extent.height);
    const mat4 projection = mat_perspective(1.0472f /* 60 deg */, aspect, 0.1f, 10.0f);
    const mat4 view = mat_translate(0.0f, 0.0f, -3.0f);

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
            std::printf("[vk-depth] vkAcquireNextImageKHR -> %d\n", acquired);
            break;
        }

        const uint64_t now = now_ms();
        const float angle = static_cast<float>(now - start_ms) / 1000.0f * rotation_speed;
        const mat4 model = mat_multiply(mat_rotate_y(angle), mat_rotate_x(angle * 0.6f));
        const mat4 mvp = mat_multiply(projection, mat_multiply(view, model));

        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        begin_command_buffer(cmd, &begin);

        std::array<VkClearValue, 2> clears{};
        clears[0].color.float32[0] = 0.05f;
        clears[0].color.float32[1] = 0.05f;
        clears[0].color.float32[2] = 0.08f;
        clears[0].color.float32[3] = 1.0f;
        clears[1].depthStencil = {.depth = 1.0f, .stencil = 0};

        VkRenderPassBeginInfo rpbi{};
        rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpbi.renderPass = render_pass;
        rpbi.framebuffer = framebuffers[image_index];
        rpbi.renderArea.extent = swap_extent;
        rpbi.clearValueCount = static_cast<uint32_t>(clears.size());
        rpbi.pClearValues = clears.data();
        cmd_begin_render_pass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
        cmd_bind_pipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        cmd_push_constants(cmd, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(mat4), mvp.data());
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
            std::printf("[vk-depth] vkQueuePresentKHR -> %d\n", presented);
            break;
        }

        ++fps_window_frames;
        if (fps_window_frames >= fps_window_size)
        {
            const ULONGLONG fps_elapsed = (now > fps_window_start) ? (now - fps_window_start) : 1;
            const double fps = static_cast<double>(fps_window_frames) * 1000.0 / static_cast<double>(fps_elapsed);
            const double ms_per_frame = static_cast<double>(fps_elapsed) / static_cast<double>(fps_window_frames);
            std::array<char, 128> title{};
            std::snprintf(title.data(), title.size(), "Sogen Vulkan - Depth Cube - %.2f FPS", fps);
            SetWindowTextA(hwnd, title.data());
            std::printf("[vk-depth] %.2f FPS (%.0f ms/frame, frame %u)\n", fps, ms_per_frame, frame);
            fps_window_frames = 0;
            fps_window_start = now;
        }

        ++frame;
    }

    std::printf("[vk-depth] done after %u frames\n", frame);

    const auto destroy_framebuffer = load<PFN_vkDestroyFramebuffer>(gipa, instance, "vkDestroyFramebuffer");
    const auto destroy_pipeline = load<PFN_vkDestroyPipeline>(gipa, instance, "vkDestroyPipeline");
    const auto destroy_pipeline_layout = load<PFN_vkDestroyPipelineLayout>(gipa, instance, "vkDestroyPipelineLayout");
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
        destroy_image_view(device, image_views[i], nullptr);
    }
    destroy_pipeline(device, pipeline, nullptr);
    destroy_pipeline_layout(device, pipeline_layout, nullptr);
    destroy_shader_module(device, frag_module, nullptr);
    destroy_shader_module(device, vert_module, nullptr);
    destroy_render_pass(device, render_pass, nullptr);
    destroy_image_view(device, depth_view, nullptr);
    destroy_image(device, depth_image, nullptr);
    free_memory(device, depth_memory, nullptr);
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
