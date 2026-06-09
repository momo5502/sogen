// A windowed Vulkan guest app that draws a spinning, solid-shaded 3D cube and continuously reports
// its frame rate. It builds on vulkan-spinning-triangle-sample (Win32 window + swapchain + render-pass
// draw through the Sogen GPU bridge) and is deliberately a notch more complex:
//   - 3D geometry: a full cube (6 faces, 36 baked-in vertices) transformed by a model-view-projection
//     matrix supplied as a 64-byte push constant (mat4). The matrix math runs on the CPU.
//   - correct occlusion without a depth buffer: the bridge has no depth attachment and does not cull,
//     so we draw the cube with the painter's algorithm -- the six faces are sorted back-to-front each
//     frame (by their rotated center's view-space depth) and drawn far-to-near, each with its own
//     vkCmdDraw whose firstVertex selects that face's six vertices.
//   - benchmark: the loop runs until the window is closed and prints the measured FPS to stdout once per
//     real second. Run the same binary against the shim (in the emulator) and against a real driver
//     (vulkan-1.dll) to compare emulated vs host throughput directly. An optional run duration in
//     seconds (argv[2]) stops the loop after that many wall-clock seconds (default 0 = run until closed),
//     which is handy for a fixed-length, scripted comparison.
//
// Usage: vulkan-cube-sample.exe [vulkan dll = vulkan-shim.dll] [max seconds = 0 (indefinite)]
//
// Because the analyzer advances the guest's system time at real wall-clock rate (the default, non
// -reproducible clock), GetSystemTimeAsFileTime gives real elapsed time in both modes, so the FPS
// numbers are directly comparable. The rotation is driven by that real wall-clock time (not the frame
// count), so the cube spins at the same physical rate whether it renders at thousands of FPS natively
// or a handful in the emulator -- only the smoothness differs.

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan_win32.h>

#include "cube_spirv.hxx"

namespace
{
    constexpr uint32_t window_width = 480;
    constexpr uint32_t window_height = 480; // square so the cube isn't stretched
    constexpr VkFormat swapchain_format = VK_FORMAT_B8G8R8A8_UNORM;
    constexpr uint32_t face_count = 6;
    constexpr uint32_t verts_per_face = 6;
    // Rotation rate in radians per second. Driven by real wall-clock time (see now_ms), so the cube
    // spins at this rate regardless of the achieved frame rate -- a frame-based increment would blur at
    // native's thousands of FPS yet crawl in the emulator. ~0.6 rad/s is one revolution every ~10 s.
    constexpr float rotation_speed = 0.6f;

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
            ValidateRect(hwnd, nullptr); // we present via Vulkan, not GDI
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

    // Wall-clock milliseconds from the system time. We use this rather than GetTickCount64 because the
    // emulator advances the tick counter far faster than real time, which would make a measured FPS
    // meaningless.
    uint64_t now_ms()
    {
        FILETIME ft{};
        GetSystemTimeAsFileTime(&ft);
        ULARGE_INTEGER u{};
        u.LowPart = ft.dwLowDateTime;
        u.HighPart = ft.dwHighDateTime;
        return u.QuadPart / 10000ULL; // 100 ns units -> ms
    }

    // High-resolution counter for per-phase profiling. NOTE: under the emulator QueryPerformanceCounter
    // is itself a syscall (NtQueryPerformanceCounter), i.e. a boundary crossing, so each phase boundary
    // adds one such crossing -- we calibrate that cost separately so it can be reasoned about.
    int64_t qpc_now()
    {
        LARGE_INTEGER t{};
        QueryPerformanceCounter(&t);
        return t.QuadPart;
    }

    // Column-major 4x4 matrix, matching GLSL/Vulkan layout (element (row, col) at col * 4 + row).
    using mat4 = std::array<float, 16>;

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

    // Right-handed perspective with clip-space depth in [0, 1] and the Y axis flipped for Vulkan.
    mat4 mat_perspective(float fovy, float aspect, float z_near, float z_far)
    {
        const float f = 1.0f / std::tan(fovy * 0.5f);
        mat4 m{};
        m[mat_index(0, 0)] = f / aspect;
        m[mat_index(1, 1)] = -f; // flip Y for Vulkan's clip space
        m[mat_index(2, 2)] = z_far / (z_near - z_far);
        m[mat_index(3, 2)] = -1.0f;
        m[mat_index(2, 3)] = (z_far * z_near) / (z_near - z_far);
        return m;
    }

    struct vec3
    {
        float x, y, z;
    };

    // Transform a point by a matrix and return the resulting view-space Z (used to depth-sort faces).
    float transform_z(const mat4& m, const vec3& p)
    {
        return m[mat_index(2, 0)] * p.x + m[mat_index(2, 1)] * p.y + m[mat_index(2, 2)] * p.z + m[mat_index(2, 3)];
    }

    // Centers of the six cube faces, in the same face order as cube.vert (+X, -X, +Y, -Y, +Z, -Z).
    constexpr std::array<vec3, face_count> face_centers = {{
        {.x = 0.5f, .y = 0.0f, .z = 0.0f},
        {.x = -0.5f, .y = 0.0f, .z = 0.0f},
        {.x = 0.0f, .y = 0.5f, .z = 0.0f},
        {.x = 0.0f, .y = -0.5f, .z = 0.0f},
        {.x = 0.0f, .y = 0.0f, .z = 0.5f},
        {.x = 0.0f, .y = 0.0f, .z = -0.5f},
    }};
}

int main(int argc, char** argv)
{
    const char* dll = (argc > 1) ? argv[1] : "vulkan-shim.dll";
    const uint64_t max_ms = (argc > 2) ? static_cast<uint64_t>(std::atoi(argv[2])) * 1000ULL : 0ULL;

    const HINSTANCE hinstance = GetModuleHandleA(nullptr);
    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &window_proc;
    wc.hInstance = hinstance;
    wc.lpszClassName = "SogenVulkanCube";
    wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    if (!RegisterClassExA(&wc))
    {
        std::printf("[vk-cube] RegisterClassExA failed: %lu\n", GetLastError());
        return 1;
    }

    // Fixed-size window (no thick frame / maximize) so the surface extent never changes under us -- this
    // sample does not recreate the swapchain on resize.
    constexpr DWORD window_style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE;
    const HWND hwnd =
        CreateWindowExA(0, wc.lpszClassName, "Sogen Vulkan - Spinning Cube", window_style, 200, 200, static_cast<int>(window_width),
                        static_cast<int>(window_height), nullptr, nullptr, hinstance, nullptr);
    if (!hwnd)
    {
        std::printf("[vk-cube] CreateWindowExA failed: %lu\n", GetLastError());
        return 2;
    }

    const HMODULE mod = LoadLibraryA(dll);
    if (!mod)
    {
        std::printf("[vk-cube] LoadLibrary(%s) failed: %lu\n", dll, GetLastError());
        return 3;
    }
    const auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(reinterpret_cast<void*>(GetProcAddress(mod, "vkGetInstanceProcAddr")));
    if (!gipa)
    {
        std::printf("[vk-cube] no vkGetInstanceProcAddr export\n");
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
        std::printf("[vk-cube] vkCreateInstance failed (no host Vulkan driver?)\n");
        return 5;
    }

    const auto enumerate = load<PFN_vkEnumeratePhysicalDevices>(gipa, instance, "vkEnumeratePhysicalDevices");
    const auto get_queue_families =
        load<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(gipa, instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    const auto create_device = load<PFN_vkCreateDevice>(gipa, instance, "vkCreateDevice");
    const auto get_device_queue = load<PFN_vkGetDeviceQueue>(gipa, instance, "vkGetDeviceQueue");
    const auto create_win32_surface = load<PFN_vkCreateWin32SurfaceKHR>(gipa, instance, "vkCreateWin32SurfaceKHR");
    const auto get_surface_caps =
        load<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(gipa, instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
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
    const auto create_shader_module = load<PFN_vkCreateShaderModule>(gipa, instance, "vkCreateShaderModule");
    const auto create_image_view = load<PFN_vkCreateImageView>(gipa, instance, "vkCreateImageView");
    const auto create_render_pass = load<PFN_vkCreateRenderPass>(gipa, instance, "vkCreateRenderPass");
    const auto create_framebuffer = load<PFN_vkCreateFramebuffer>(gipa, instance, "vkCreateFramebuffer");
    const auto create_pipeline_layout = load<PFN_vkCreatePipelineLayout>(gipa, instance, "vkCreatePipelineLayout");
    const auto create_graphics_pipelines = load<PFN_vkCreateGraphicsPipelines>(gipa, instance, "vkCreateGraphicsPipelines");
    const auto cmd_begin_render_pass = load<PFN_vkCmdBeginRenderPass>(gipa, instance, "vkCmdBeginRenderPass");
    const auto cmd_bind_pipeline = load<PFN_vkCmdBindPipeline>(gipa, instance, "vkCmdBindPipeline");
    const auto cmd_push_constants = load<PFN_vkCmdPushConstants>(gipa, instance, "vkCmdPushConstants");
    const auto cmd_draw = load<PFN_vkCmdDraw>(gipa, instance, "vkCmdDraw");
    const auto cmd_end_render_pass = load<PFN_vkCmdEndRenderPass>(gipa, instance, "vkCmdEndRenderPass");

    uint32_t device_count = 0;
    enumerate(instance, &device_count, nullptr);
    if (device_count == 0)
    {
        std::printf("[vk-cube] no physical devices\n");
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
        std::printf("[vk-cube] no graphics queue family\n");
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
        std::printf("[vk-cube] vkCreateDevice failed\n");
        return 8;
    }

    VkQueue queue = VK_NULL_HANDLE;
    get_device_queue(device, graphics_family, 0, &queue);

    VkWin32SurfaceCreateInfoKHR sci{};
    sci.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    sci.hinstance = hinstance;
    sci.hwnd = hwnd;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (create_win32_surface(instance, &sci, nullptr, &surface) != VK_SUCCESS)
    {
        std::printf("[vk-cube] vkCreateWin32SurfaceKHR failed\n");
        return 9;
    }

    // Size the swapchain to the surface's real extent (the client area in physical pixels). Hardcoding
    // the window size breaks on a real driver -- the client area is smaller than the window and may be
    // DPI-scaled, so the present is rejected with VK_ERROR_OUT_OF_DATE_KHR. The bridge reports an
    // undefined extent (0xFFFFFFFF), in which case we fall back to the window client rect.
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
    // IMMEDIATE (no vsync) so the FPS readout reflects real throughput rather than the refresh rate.
    // The bridge ignores the present mode; on a real driver IMMEDIATE is near-universally supported.
    swci.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    swci.clipped = VK_TRUE;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    if (create_swapchain(device, &swci, nullptr, &swapchain) != VK_SUCCESS || !swapchain)
    {
        std::printf("[vk-cube] vkCreateSwapchainKHR failed\n");
        return 10;
    }

    uint32_t image_count = 0;
    get_swapchain_images(device, swapchain, &image_count, nullptr);
    std::vector<VkImage> images(image_count);
    get_swapchain_images(device, swapchain, &image_count, images.data());
    std::printf("[vk-cube] swapchain ready: %u images, %ux%u\n", image_count, swap_extent.width, swap_extent.height);

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
        std::printf("[vk-cube] vkCreateRenderPass failed\n");
        return 11;
    }

    // --- shader modules ---
    VkShaderModuleCreateInfo vert_ci{};
    vert_ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    vert_ci.codeSize = cube_vert_spv.size() * sizeof(uint32_t);
    vert_ci.pCode = cube_vert_spv.data();
    VkShaderModule vert_module = VK_NULL_HANDLE;
    create_shader_module(device, &vert_ci, nullptr, &vert_module);

    VkShaderModuleCreateInfo frag_ci{};
    frag_ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    frag_ci.codeSize = cube_frag_spv.size() * sizeof(uint32_t);
    frag_ci.pCode = cube_frag_spv.data();
    VkShaderModule frag_module = VK_NULL_HANDLE;
    create_shader_module(device, &frag_ci, nullptr, &frag_module);

    // --- pipeline layout with a push-constant mat4 (the model-view-projection matrix) ---
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

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
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
    gpci.pColorBlendState = &color_blend;
    gpci.layout = pipeline_layout;
    gpci.renderPass = render_pass;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (create_graphics_pipelines(device, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipeline) != VK_SUCCESS || !pipeline)
    {
        std::printf("[vk-cube] vkCreateGraphicsPipelines failed\n");
        return 12;
    }

    // --- per-image views + framebuffers ---
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

    // Per-frame fence so we wait for the GPU to finish a frame before reusing the command buffer and
    // presenting (on a real driver there is no implicit serialization; through the bridge it is benign).
    VkFenceCreateInfo fence_ci{};
    fence_ci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence frame_fence = VK_NULL_HANDLE;
    create_fence(device, &fence_ci, nullptr, &frame_fence);

    const float aspect = static_cast<float>(swap_extent.width) / static_cast<float>(swap_extent.height);
    const mat4 projection = mat_perspective(1.0472f /* 60 deg */, aspect, 0.1f, 10.0f);
    const mat4 view = mat_translate(0.0f, 0.0f, -3.0f);

    // --- render loop: spin the cube (wall-clock angle) and print FPS once per real second ---
    std::printf("[vk-cube] rendering through '%s' (close the window to stop)\n", dll);
    std::fflush(stdout);
    const uint64_t start_ms = now_ms();
    uint64_t fps_window_start = start_ms;
    uint32_t fps_window_frames = 0;
    uint32_t frame = 0;

    // Per-phase profiling, accumulated in QPC ticks over each one-second reporting window.
    LARGE_INTEGER qpc_freq{};
    QueryPerformanceFrequency(&qpc_freq);
    const double qpc_to_us = 1.0e6 / static_cast<double>(qpc_freq.QuadPart);
    int64_t acc_acquire = 0;
    int64_t acc_cpu = 0;
    int64_t acc_record = 0;
    int64_t acc_sync = 0;
    int64_t acc_present = 0;

    while (!g_quit)
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
        if (max_ms != 0 && (now_ms() - start_ms) >= max_ms)
        {
            break;
        }

        const int64_t t_acquire_begin = qpc_now();
        uint32_t image_index = 0;
        const VkResult acquired = acquire_next(device, swapchain, UINT64_MAX, VK_NULL_HANDLE, VK_NULL_HANDLE, &image_index);
        const int64_t t_acquire_end = qpc_now();
        // VK_SUBOPTIMAL_KHR is a success code (the swapchain still works); only a real error is fatal.
        if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR)
        {
            std::printf("[vk-cube] vkAcquireNextImageKHR -> %d\n", acquired);
            break;
        }

        const float angle = static_cast<float>(now_ms() - start_ms) / 1000.0f * rotation_speed;
        const mat4 model = mat_multiply(mat_rotate_y(angle), mat_rotate_x(angle * 0.6f));
        const mat4 model_view = mat_multiply(view, model);
        const mat4 mvp = mat_multiply(projection, model_view);

        // Painter's algorithm: with no depth buffer and no culling, draw the faces back-to-front so the
        // near faces correctly overdraw the far ones. Sort face indices by their center's view-space Z
        // (more negative = farther from the camera, which looks down -Z), farthest first.
        std::array<uint32_t, face_count> order = {0, 1, 2, 3, 4, 5};
        std::array<float, face_count> depth{};
        for (uint32_t f = 0; f < face_count; ++f)
        {
            depth[f] = transform_z(model_view, face_centers[f]);
        }
        std::ranges::sort(order, [&](uint32_t a, uint32_t b) { return depth[a] < depth[b]; });
        const int64_t t_cpu_end = qpc_now();

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
        cmd_push_constants(cmd, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(mat4), mvp.data());
        for (const uint32_t f : order)
        {
            cmd_draw(cmd, verts_per_face, 1, f * verts_per_face, 0);
        }
        cmd_end_render_pass(cmd);

        end_command_buffer(cmd);
        const int64_t t_record_end = qpc_now();

        reset_fences(device, 1, &frame_fence);
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        queue_submit(queue, 1, &submit, frame_fence);
        // Wait for rendering to finish before presenting (no render-finished semaphore in this sample).
        wait_for_fences(device, 1, &frame_fence, VK_TRUE, UINT64_MAX);
        const int64_t t_sync_end = qpc_now();

        VkPresentInfoKHR present{};
        present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain;
        present.pImageIndices = &image_index;
        const VkResult presented = queue_present(queue, &present);
        const int64_t t_present_end = qpc_now();
        if (presented != VK_SUCCESS && presented != VK_SUBOPTIMAL_KHR)
        {
            std::printf("[vk-cube] vkQueuePresentKHR -> %d\n", presented);
            break;
        }

        acc_acquire += t_acquire_end - t_acquire_begin;
        acc_cpu += t_cpu_end - t_acquire_end;
        acc_record += t_record_end - t_cpu_end;
        acc_sync += t_sync_end - t_record_end;
        acc_present += t_present_end - t_sync_end;

        // FPS: once per real second, report frames/elapsed against the (real-time) system clock. Running
        // the same binary through the shim and against vulkan-1.dll lets the two rates be compared.
        ++fps_window_frames;
        const uint64_t now = now_ms();
        if (now - fps_window_start >= 1000)
        {
            const auto elapsed = static_cast<double>(now - fps_window_start);
            const double fps = static_cast<double>(fps_window_frames) * 1000.0 / elapsed;
            std::array<char, 128> title{};
            std::snprintf(title.data(), title.size(), "Sogen Vulkan - Spinning Cube - %.1f FPS", fps);
            SetWindowTextA(hwnd, title.data());
            std::printf("[vk-cube] %.1f FPS (%.2f ms/frame, frame %u)\n", fps, elapsed / static_cast<double>(fps_window_frames), frame);

            // Calibrate one boundary crossing: NtQueryPerformanceCounter is a trivial syscall, so timing
            // back-to-back calls isolates the VM-exit + host-dispatch round trip that every remoted
            // Vulkan command also pays.
            constexpr int calib_iters = 200;
            const int64_t cal0 = qpc_now();
            for (int k = 0; k < calib_iters; ++k)
            {
                (void)qpc_now();
            }
            const int64_t cal1 = qpc_now();
            const double syscall_us = static_cast<double>(cal1 - cal0) * qpc_to_us / calib_iters;

            const auto nf = static_cast<double>(fps_window_frames);
            std::printf("[vk-cube]   per-frame us: acquire %.0f | cpu %.0f | record %.0f | submit+wait %.0f | "
                        "present %.0f  (1 syscall ~%.2f us)\n",
                        static_cast<double>(acc_acquire) * qpc_to_us / nf, static_cast<double>(acc_cpu) * qpc_to_us / nf,
                        static_cast<double>(acc_record) * qpc_to_us / nf, static_cast<double>(acc_sync) * qpc_to_us / nf,
                        static_cast<double>(acc_present) * qpc_to_us / nf, syscall_us);
            std::fflush(stdout);

            acc_acquire = acc_cpu = acc_record = acc_sync = acc_present = 0;
            fps_window_frames = 0;
            fps_window_start = now;
        }

        ++frame;
    }

    std::printf("[vk-cube] done after %u frames\n", frame);

    const auto destroy_framebuffer = load<PFN_vkDestroyFramebuffer>(gipa, instance, "vkDestroyFramebuffer");
    const auto destroy_image_view = load<PFN_vkDestroyImageView>(gipa, instance, "vkDestroyImageView");
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
    destroy_fence(device, frame_fence, nullptr);
    destroy_command_pool(device, pool, nullptr);
    destroy_swapchain(device, swapchain, nullptr);
    destroy_surface(instance, surface, nullptr);
    destroy_device(device, nullptr);
    destroy_instance(instance, nullptr);
    return 0;
}
