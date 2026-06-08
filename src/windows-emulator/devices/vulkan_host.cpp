#include "vulkan_host.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <unordered_map>
#include <vector>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <dlfcn.h>
#endif

namespace sogen
{
    namespace
    {
#ifdef _WIN32
        using library_handle = HMODULE;

        library_handle load_library(const char* name)
        {
            return ::LoadLibraryA(name);
        }

        void* get_symbol(library_handle handle, const char* name)
        {
            return reinterpret_cast<void*>(::GetProcAddress(handle, name));
        }

        void free_library(library_handle handle)
        {
            ::FreeLibrary(handle);
        }

        constexpr std::array<const char*, 1> vulkan_loader_names{"vulkan-1.dll"};
#else
        using library_handle = void*;

        library_handle load_library(const char* name)
        {
            return ::dlopen(name, RTLD_NOW | RTLD_LOCAL);
        }

        void* get_symbol(library_handle handle, const char* name)
        {
            return ::dlsym(handle, name);
        }

        void free_library(library_handle handle)
        {
            ::dlclose(handle);
        }

        constexpr std::array<const char*, 2> vulkan_loader_names{"libvulkan.so.1", "libvulkan.so"};
#endif
    }

    struct vulkan_host::impl
    {
        library_handle loader{};
        PFN_vkGetInstanceProcAddr get_instance_proc_addr{};
        PFN_vkCreateInstance create_instance{};

        struct instance_data
        {
            VkInstance handle{};
            PFN_vkDestroyInstance destroy_instance{};
            PFN_vkEnumeratePhysicalDevices enumerate_physical_devices{};
            PFN_vkGetPhysicalDeviceProperties get_physical_device_properties{};
            PFN_vkGetPhysicalDeviceQueueFamilyProperties get_queue_family_properties{};
            PFN_vkCreateDevice create_device{};
            PFN_vkGetDeviceProcAddr get_device_proc_addr{};
        };

        struct physical_device_data
        {
            VkPhysicalDevice handle{};
            uint64_t instance_id{};
        };

        struct device_data
        {
            VkDevice handle{};
            uint64_t instance_id{};
            PFN_vkDestroyDevice destroy_device{};
            PFN_vkGetDeviceQueue get_device_queue{};
        };

        std::unordered_map<uint64_t, instance_data> instances;
        std::unordered_map<uint64_t, physical_device_data> physical_devices;
        std::unordered_map<VkPhysicalDevice, uint64_t> physical_device_ids;
        struct queue_data
        {
            VkQueue handle{};
            uint64_t device_id{};
        };

        std::unordered_map<uint64_t, device_data> devices;
        std::unordered_map<uint64_t, queue_data> queues;
        uint64_t next_id{1};

        void erase_device(uint64_t device_id)
        {
            const auto it = this->devices.find(device_id);
            if (it == this->devices.end())
            {
                return;
            }

            if (it->second.handle && it->second.destroy_device)
            {
                it->second.destroy_device(it->second.handle, nullptr);
            }

            for (auto q = this->queues.begin(); q != this->queues.end();)
            {
                q = (q->second.device_id == device_id) ? this->queues.erase(q) : std::next(q);
            }

            this->devices.erase(it);
        }

        impl()
        {
            for (const auto* name : vulkan_loader_names)
            {
                this->loader = load_library(name);
                if (this->loader)
                {
                    break;
                }
            }

            if (!this->loader)
            {
                return;
            }

            this->get_instance_proc_addr =
                reinterpret_cast<PFN_vkGetInstanceProcAddr>(get_symbol(this->loader, "vkGetInstanceProcAddr"));
            if (!this->get_instance_proc_addr)
            {
                return;
            }

            this->create_instance =
                reinterpret_cast<PFN_vkCreateInstance>(this->get_instance_proc_addr(nullptr, "vkCreateInstance"));
        }

        ~impl()
        {
            for (auto& [id, device] : this->devices)
            {
                if (device.handle && device.destroy_device)
                {
                    device.destroy_device(device.handle, nullptr);
                }
            }

            for (auto& [id, instance] : this->instances)
            {
                if (instance.handle && instance.destroy_instance)
                {
                    instance.destroy_instance(instance.handle, nullptr);
                }
            }

            if (this->loader)
            {
                free_library(this->loader);
            }
        }

        template <typename Fn>
        Fn load_instance_proc(VkInstance instance, const char* name) const
        {
            return reinterpret_cast<Fn>(this->get_instance_proc_addr(instance, name));
        }
    };

    vulkan_host::vulkan_host() : impl_(std::make_unique<impl>())
    {
    }

    vulkan_host::~vulkan_host() = default;

    bool vulkan_host::available() const
    {
        return this->impl_->create_instance != nullptr;
    }

    int32_t vulkan_host::create_instance(uint64_t& out_instance)
    {
        out_instance = 0;

        if (!this->available())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkApplicationInfo app_info{};
        app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app_info.apiVersion = VK_API_VERSION_1_0;

        VkInstanceCreateInfo create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        create_info.pApplicationInfo = &app_info;

        VkInstance instance{};
        const VkResult result = this->impl_->create_instance(&create_info, nullptr, &instance);
        if (result != VK_SUCCESS)
        {
            return result;
        }

        impl::instance_data data{};
        data.handle = instance;
        data.destroy_instance = this->impl_->load_instance_proc<PFN_vkDestroyInstance>(instance, "vkDestroyInstance");
        data.enumerate_physical_devices =
            this->impl_->load_instance_proc<PFN_vkEnumeratePhysicalDevices>(instance, "vkEnumeratePhysicalDevices");
        data.get_physical_device_properties =
            this->impl_->load_instance_proc<PFN_vkGetPhysicalDeviceProperties>(instance, "vkGetPhysicalDeviceProperties");
        data.get_queue_family_properties = this->impl_->load_instance_proc<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
            instance, "vkGetPhysicalDeviceQueueFamilyProperties");
        data.create_device = this->impl_->load_instance_proc<PFN_vkCreateDevice>(instance, "vkCreateDevice");
        data.get_device_proc_addr = this->impl_->load_instance_proc<PFN_vkGetDeviceProcAddr>(instance, "vkGetDeviceProcAddr");

        const uint64_t id = this->impl_->next_id++;
        this->impl_->instances.emplace(id, data);
        out_instance = id;
        return VK_SUCCESS;
    }

    void vulkan_host::destroy_instance(uint64_t instance)
    {
        const auto it = this->impl_->instances.find(instance);
        if (it == this->impl_->instances.end())
        {
            return;
        }

        // Tear down logical devices (and their queues) created from this instance first.
        std::vector<uint64_t> owned_devices;
        for (const auto& [device_id, device] : this->impl_->devices)
        {
            if (device.instance_id == instance)
            {
                owned_devices.push_back(device_id);
            }
        }
        for (const uint64_t device_id : owned_devices)
        {
            this->impl_->erase_device(device_id);
        }

        if (it->second.handle && it->second.destroy_instance)
        {
            it->second.destroy_instance(it->second.handle, nullptr);
        }

        // Drop physical devices that belonged to this instance.
        for (auto pd = this->impl_->physical_devices.begin(); pd != this->impl_->physical_devices.end();)
        {
            if (pd->second.instance_id == instance)
            {
                this->impl_->physical_device_ids.erase(pd->second.handle);
                pd = this->impl_->physical_devices.erase(pd);
            }
            else
            {
                ++pd;
            }
        }

        this->impl_->instances.erase(it);
    }

    int32_t vulkan_host::enumerate_physical_devices(uint64_t instance, std::span<uint64_t> out_devices, uint32_t& out_count)
    {
        out_count = 0;

        const auto it = this->impl_->instances.find(instance);
        if (it == this->impl_->instances.end() || !it->second.enumerate_physical_devices)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        uint32_t count = 0;
        VkResult result = it->second.enumerate_physical_devices(it->second.handle, &count, nullptr);
        if (result != VK_SUCCESS && result != VK_INCOMPLETE)
        {
            return result;
        }

        std::vector<VkPhysicalDevice> devices(count);
        if (count > 0)
        {
            result = it->second.enumerate_physical_devices(it->second.handle, &count, devices.data());
            if (result != VK_SUCCESS && result != VK_INCOMPLETE)
            {
                return result;
            }
        }

        out_count = count;

        for (uint32_t i = 0; i < count && i < out_devices.size(); ++i)
        {
            const VkPhysicalDevice device = devices[i];

            auto id_it = this->impl_->physical_device_ids.find(device);
            if (id_it == this->impl_->physical_device_ids.end())
            {
                const uint64_t id = this->impl_->next_id++;
                this->impl_->physical_devices.emplace(id, impl::physical_device_data{device, instance});
                id_it = this->impl_->physical_device_ids.emplace(device, id).first;
            }

            out_devices[i] = id_it->second;
        }

        return VK_SUCCESS;
    }

    int32_t vulkan_host::get_physical_device_properties(uint64_t physical_device, void* out, size_t out_size)
    {
        const auto pd = this->impl_->physical_devices.find(physical_device);
        if (pd == this->impl_->physical_devices.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto instance = this->impl_->instances.find(pd->second.instance_id);
        if (instance == this->impl_->instances.end() || !instance->second.get_physical_device_properties)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkPhysicalDeviceProperties properties{};
        instance->second.get_physical_device_properties(pd->second.handle, &properties);

        std::memcpy(out, &properties, std::min(out_size, sizeof(properties)));
        return VK_SUCCESS;
    }

    int32_t vulkan_host::get_queue_family_properties(uint64_t physical_device, void* out, size_t out_size, uint32_t& out_count)
    {
        out_count = 0;

        const auto pd = this->impl_->physical_devices.find(physical_device);
        if (pd == this->impl_->physical_devices.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto instance = this->impl_->instances.find(pd->second.instance_id);
        if (instance == this->impl_->instances.end() || !instance->second.get_queue_family_properties)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        uint32_t count = 0;
        instance->second.get_queue_family_properties(pd->second.handle, &count, nullptr);

        std::vector<VkQueueFamilyProperties> families(count);
        if (count > 0)
        {
            instance->second.get_queue_family_properties(pd->second.handle, &count, families.data());
        }

        out_count = count;

        const size_t copy_bytes = std::min(out_size, families.size() * sizeof(VkQueueFamilyProperties));
        if (copy_bytes > 0)
        {
            std::memcpy(out, families.data(), copy_bytes);
        }

        return VK_SUCCESS;
    }

    int32_t vulkan_host::create_device(uint64_t physical_device, uint32_t queue_family_index, uint32_t queue_count,
                                       uint64_t& out_device)
    {
        out_device = 0;

        const auto pd = this->impl_->physical_devices.find(physical_device);
        if (pd == this->impl_->physical_devices.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto instance = this->impl_->instances.find(pd->second.instance_id);
        if (instance == this->impl_->instances.end() || !instance->second.create_device)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const uint32_t queues = (queue_count == 0) ? 1 : queue_count;
        const std::vector<float> priorities(queues, 1.0f);

        VkDeviceQueueCreateInfo queue_info{};
        queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info.queueFamilyIndex = queue_family_index;
        queue_info.queueCount = queues;
        queue_info.pQueuePriorities = priorities.data();

        VkDeviceCreateInfo create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        create_info.queueCreateInfoCount = 1;
        create_info.pQueueCreateInfos = &queue_info;

        VkDevice device{};
        const VkResult result = instance->second.create_device(pd->second.handle, &create_info, nullptr, &device);
        if (result != VK_SUCCESS)
        {
            return result;
        }

        impl::device_data data{};
        data.handle = device;
        data.instance_id = pd->second.instance_id;

        if (instance->second.get_device_proc_addr)
        {
            data.destroy_device =
                reinterpret_cast<PFN_vkDestroyDevice>(instance->second.get_device_proc_addr(device, "vkDestroyDevice"));
            data.get_device_queue =
                reinterpret_cast<PFN_vkGetDeviceQueue>(instance->second.get_device_proc_addr(device, "vkGetDeviceQueue"));
        }

        const uint64_t id = this->impl_->next_id++;
        this->impl_->devices.emplace(id, data);
        out_device = id;
        return VK_SUCCESS;
    }

    void vulkan_host::destroy_device(uint64_t device)
    {
        this->impl_->erase_device(device);
    }

    int32_t vulkan_host::get_device_queue(uint64_t device, uint32_t queue_family_index, uint32_t queue_index, uint64_t& out_queue)
    {
        out_queue = 0;

        const auto dev = this->impl_->devices.find(device);
        if (dev == this->impl_->devices.end() || !dev->second.get_device_queue)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkQueue queue{};
        dev->second.get_device_queue(dev->second.handle, queue_family_index, queue_index, &queue);
        if (!queue)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const uint64_t id = this->impl_->next_id++;
        this->impl_->queues.emplace(id, impl::queue_data{queue, device});
        out_queue = id;
        return VK_SUCCESS;
    }
}
