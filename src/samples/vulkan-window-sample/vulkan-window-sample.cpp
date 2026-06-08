// A minimal *windowed* Vulkan guest app for the Sogen GPU bridge. It creates a real Win32 window,
// builds a Vulkan swapchain on that window's HWND through the guest shim (vulkan-shim.dll), and each
// frame clears the swapchain image to an animated color and presents it. On the host the bridge
// renders into an offscreen image, reads it back, and hands the pixels to the window via the
// emulator's UI backend -- so the window shows live GPU-rendered content.
//
// This is the WSI counterpart to vulkan-shim-test (which is headless): same bridge, but the pixels
// end up on screen instead of in a readback assertion.

#include <windows.h>

#include <array>
#include <cstdio>
#include <vector>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan_win32.h>

#include "triangle_spirv.hpp"

namespace
{
    constexpr uint32_t window_width = 320;
    constexpr uint32_t window_height = 240;
    constexpr VkFormat swapchain_format = VK_FORMAT_B8G8R8A8_UNORM; // matches the UI backend's bgra8

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
            // We present via Vulkan, not GDI; just validate so we don't spin on WM_PAINT and don't let
            // an empty GDI BeginPaint/EndPaint surface overwrite our rendered content.
            ValidateRect(hwnd, nullptr);
            return 0;
        default:
            return DefWindowProcA(hwnd, msg, wparam, lparam);
        }
    }

    // Resolves a Vulkan entry point through the shim's vkGetInstanceProcAddr.
    template <typename Fn>
    Fn load(PFN_vkGetInstanceProcAddr gipa, VkInstance instance, const char* name)
    {
        return reinterpret_cast<Fn>(gipa(instance, name));
    }
}

int main(int argc, char** argv)
{
    const char* dll = (argc > 1) ? argv[1] : "vulkan-shim.dll";
    const uint32_t max_frames = (argc > 2) ? static_cast<uint32_t>(std::atoi(argv[2])) : 180;

    // --- host window ---
    const HINSTANCE hinstance = GetModuleHandleA(nullptr);
    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &window_proc;
    wc.hInstance = hinstance;
    wc.lpszClassName = "SogenVulkanWindow";
    wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    if (!RegisterClassExA(&wc))
    {
        std::printf("[vk-window] RegisterClassExA failed: %lu\n", GetLastError());
        return 1;
    }

    // Fixed-size window (no resize) -- this sample does not recreate the swapchain on resize.
    constexpr DWORD window_style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE;
    const HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "Sogen Vulkan Window", window_style, 200, 200,
                                      static_cast<int>(window_width), static_cast<int>(window_height), nullptr, nullptr,
                                      hinstance, nullptr);
    if (!hwnd)
    {
        std::printf("[vk-window] CreateWindowExA failed: %lu\n", GetLastError());
        return 2;
    }

    // --- load the shim and bootstrap Vulkan ---
    const HMODULE mod = LoadLibraryA(dll);
    if (!mod)
    {
        std::printf("[vk-window] LoadLibrary(%s) failed: %lu\n", dll, GetLastError());
        return 3;
    }
    const auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        reinterpret_cast<void*>(GetProcAddress(mod, "vkGetInstanceProcAddr")));
    if (!gipa)
    {
        std::printf("[vk-window] no vkGetInstanceProcAddr export\n");
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
        std::printf("[vk-window] vkCreateInstance failed (no host Vulkan driver?)\n");
        return 5;
    }

    const auto enumerate = load<PFN_vkEnumeratePhysicalDevices>(gipa, instance, "vkEnumeratePhysicalDevices");
    const auto get_queue_families =
        load<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(gipa, instance, "vkGetPhysicalDeviceQueueFamilyProperties");
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
    const auto create_shader_module = load<PFN_vkCreateShaderModule>(gipa, instance, "vkCreateShaderModule");
    const auto create_image_view = load<PFN_vkCreateImageView>(gipa, instance, "vkCreateImageView");
    const auto create_render_pass = load<PFN_vkCreateRenderPass>(gipa, instance, "vkCreateRenderPass");
    const auto create_framebuffer = load<PFN_vkCreateFramebuffer>(gipa, instance, "vkCreateFramebuffer");
    const auto create_pipeline_layout = load<PFN_vkCreatePipelineLayout>(gipa, instance, "vkCreatePipelineLayout");
    const auto create_graphics_pipelines = load<PFN_vkCreateGraphicsPipelines>(gipa, instance, "vkCreateGraphicsPipelines");
    const auto cmd_begin_render_pass = load<PFN_vkCmdBeginRenderPass>(gipa, instance, "vkCmdBeginRenderPass");
    const auto cmd_bind_pipeline = load<PFN_vkCmdBindPipeline>(gipa, instance, "vkCmdBindPipeline");
    const auto cmd_draw = load<PFN_vkCmdDraw>(gipa, instance, "vkCmdDraw");
    const auto cmd_end_render_pass = load<PFN_vkCmdEndRenderPass>(gipa, instance, "vkCmdEndRenderPass");

    uint32_t device_count = 0;
    enumerate(instance, &device_count, nullptr);
    if (device_count == 0)
    {
        std::printf("[vk-window] no physical devices\n");
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
        std::printf("[vk-window] no graphics queue family\n");
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
        std::printf("[vk-window] vkCreateDevice failed\n");
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
        std::printf("[vk-window] vkCreateWin32SurfaceKHR failed\n");
        return 9;
    }

    // Size the swapchain to the surface's real extent (the client area in physical pixels). Hardcoding
    // the window size breaks on a real driver (client < window, possibly DPI-scaled) with
    // VK_ERROR_OUT_OF_DATE_KHR. The bridge reports an undefined extent (0xFFFFFFFF) -> fall back to the
    // window client rect.
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
    swci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; // we render into them via a render pass
    swci.preTransform = caps.currentTransform;
    swci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swci.clipped = VK_TRUE;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    if (create_swapchain(device, &swci, nullptr, &swapchain) != VK_SUCCESS || !swapchain)
    {
        std::printf("[vk-window] vkCreateSwapchainKHR failed\n");
        return 10;
    }

    uint32_t image_count = 0;
    get_swapchain_images(device, swapchain, &image_count, nullptr);
    std::vector<VkImage> images(image_count);
    get_swapchain_images(device, swapchain, &image_count, images.data());
    std::printf("[vk-window] swapchain ready: %u images, %ux%u\n", image_count, swap_extent.width, swap_extent.height);

    // --- render pass (clear -> draw -> present) ---
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
        std::printf("[vk-window] vkCreateRenderPass failed\n");
        return 11;
    }

    // --- shader modules (embedded SPIR-V) ---
    VkShaderModuleCreateInfo vert_ci{};
    vert_ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    vert_ci.codeSize = triangle_vert_spv.size() * sizeof(uint32_t);
    vert_ci.pCode = triangle_vert_spv.data();
    VkShaderModule vert_module = VK_NULL_HANDLE;
    create_shader_module(device, &vert_ci, nullptr, &vert_module);

    VkShaderModuleCreateInfo frag_ci{};
    frag_ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    frag_ci.codeSize = triangle_frag_spv.size() * sizeof(uint32_t);
    frag_ci.pCode = triangle_frag_spv.data();
    VkShaderModule frag_module = VK_NULL_HANDLE;
    create_shader_module(device, &frag_ci, nullptr, &frag_module);

    // --- pipeline layout + graphics pipeline ---
    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
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
        std::printf("[vk-window] vkCreateGraphicsPipelines failed\n");
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

    // Per-frame fence: wait for the GPU to finish a frame before reusing the command buffer and
    // presenting (a real driver does not serialize this for us; through the bridge it is benign).
    VkFenceCreateInfo fence_ci{};
    fence_ci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence frame_fence = VK_NULL_HANDLE;
    create_fence(device, &fence_ci, nullptr, &frame_fence);

    // --- render loop: clear to a background color, draw the triangle, present each frame ---
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
        // VK_SUBOPTIMAL_KHR is a success code (the swapchain still works); only a real error is fatal.
        if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR)
        {
            std::printf("[vk-window] vkAcquireNextImageKHR -> %d\n", acquired);
            break;
        }

        const float t = static_cast<float>(frame % 180) / 180.0f;

        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        begin_command_buffer(cmd, &begin);

        VkClearValue clear{};
        clear.color.float32[0] = 0.10f;
        clear.color.float32[1] = 0.10f;
        clear.color.float32[2] = 0.15f + 0.25f * t; // subtle animated background so motion is visible
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
        cmd_draw(cmd, 3, 1, 0, 0);
        cmd_end_render_pass(cmd);

        end_command_buffer(cmd);

        reset_fences(device, 1, &frame_fence);
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        queue_submit(queue, 1, &submit, frame_fence);
        // Wait for rendering to finish before presenting (no render-finished semaphore in this sample).
        wait_for_fences(device, 1, &frame_fence, VK_TRUE, UINT64_MAX);

        VkPresentInfoKHR present{};
        present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain;
        present.pImageIndices = &image_index;
        const VkResult presented = queue_present(queue, &present);
        if (frame < 3 || (frame % 60) == 0)
        {
            std::printf("[vk-window] frame %u: image %u, draw triangle, present -> %d\n", frame, image_index, presented);
        }
        if (presented != VK_SUCCESS && presented != VK_SUBOPTIMAL_KHR)
        {
            break;
        }

        ++frame;
        Sleep(16); // ~60 fps; keeps the emulator responsive
    }

    std::printf("[vk-window] done after %u frames\n", frame);

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
