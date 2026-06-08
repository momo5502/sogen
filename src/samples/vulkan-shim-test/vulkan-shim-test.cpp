// Exercises the guest Vulkan shim (vulkan-shim.dll) through the genuine Vulkan loading dance --
// LoadLibrary + vkGetInstanceProcAddr -- exactly as a real application or the Khronos loader would.
// The shim forwards each call across the Sogen GPU bridge to the host driver, so this enumerates
// the host's physical devices end-to-end: guest app -> shim -> bridge -> host Vulkan -> GPU.

#include <windows.h>

#include <cstdio>
#include <vector>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

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
