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
            PFN_vkCreateCommandPool create_command_pool{};
            PFN_vkDestroyCommandPool destroy_command_pool{};
            PFN_vkAllocateCommandBuffers allocate_command_buffers{};
            PFN_vkFreeCommandBuffers free_command_buffers{};
            PFN_vkBeginCommandBuffer begin_command_buffer{};
            PFN_vkEndCommandBuffer end_command_buffer{};
            PFN_vkCreateFence create_fence{};
            PFN_vkDestroyFence destroy_fence{};
            PFN_vkResetFences reset_fences{};
            PFN_vkGetFenceStatus get_fence_status{};
            PFN_vkQueueSubmit queue_submit{};
        };

        std::unordered_map<uint64_t, instance_data> instances;
        std::unordered_map<uint64_t, physical_device_data> physical_devices;
        std::unordered_map<VkPhysicalDevice, uint64_t> physical_device_ids;
        struct queue_data
        {
            VkQueue handle{};
            uint64_t device_id{};
        };

        struct command_pool_data
        {
            VkCommandPool handle{};
            uint64_t device_id{};
        };

        struct command_buffer_data
        {
            VkCommandBuffer handle{};
            uint64_t device_id{};
            uint64_t pool_id{};
        };

        struct fence_data
        {
            VkFence handle{};
            uint64_t device_id{};
        };

        std::unordered_map<uint64_t, device_data> devices;
        std::unordered_map<uint64_t, queue_data> queues;
        std::unordered_map<uint64_t, command_pool_data> command_pools;
        std::unordered_map<uint64_t, command_buffer_data> command_buffers;
        std::unordered_map<uint64_t, fence_data> fences;
        uint64_t next_id{1};

        template <typename Map, typename Pred>
        void erase_owned(Map& map, Pred pred)
        {
            for (auto it = map.begin(); it != map.end();)
            {
                it = pred(it->second) ? map.erase(it) : std::next(it);
            }
        }

        void erase_device(uint64_t device_id)
        {
            const auto it = this->devices.find(device_id);
            if (it == this->devices.end())
            {
                return;
            }

            // Destroy GPU-owned children (pools free their command buffers; fences are freed) before
            // the device itself.
            for (auto& [id, pool] : this->command_pools)
            {
                if (pool.device_id == device_id && pool.handle && it->second.destroy_command_pool)
                {
                    it->second.destroy_command_pool(it->second.handle, pool.handle, nullptr);
                }
            }
            for (auto& [id, fence] : this->fences)
            {
                if (fence.device_id == device_id && fence.handle && it->second.destroy_fence)
                {
                    it->second.destroy_fence(it->second.handle, fence.handle, nullptr);
                }
            }

            this->erase_owned(this->command_buffers, [&](const command_buffer_data& d) { return d.device_id == device_id; });
            this->erase_owned(this->command_pools, [&](const command_pool_data& d) { return d.device_id == device_id; });
            this->erase_owned(this->fences, [&](const fence_data& d) { return d.device_id == device_id; });
            this->erase_owned(this->queues, [&](const queue_data& d) { return d.device_id == device_id; });

            if (it->second.handle && it->second.destroy_device)
            {
                it->second.destroy_device(it->second.handle, nullptr);
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
                this->impl_->physical_devices.emplace(id, impl::physical_device_data{.handle = device, .instance_id = instance});
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

        if (const auto gdpa = instance->second.get_device_proc_addr)
        {
            const auto resolve = [&](const char* name) { return gdpa(device, name); };

            data.destroy_device = reinterpret_cast<PFN_vkDestroyDevice>(resolve("vkDestroyDevice"));
            data.get_device_queue = reinterpret_cast<PFN_vkGetDeviceQueue>(resolve("vkGetDeviceQueue"));
            data.create_command_pool = reinterpret_cast<PFN_vkCreateCommandPool>(resolve("vkCreateCommandPool"));
            data.destroy_command_pool = reinterpret_cast<PFN_vkDestroyCommandPool>(resolve("vkDestroyCommandPool"));
            data.allocate_command_buffers = reinterpret_cast<PFN_vkAllocateCommandBuffers>(resolve("vkAllocateCommandBuffers"));
            data.free_command_buffers = reinterpret_cast<PFN_vkFreeCommandBuffers>(resolve("vkFreeCommandBuffers"));
            data.begin_command_buffer = reinterpret_cast<PFN_vkBeginCommandBuffer>(resolve("vkBeginCommandBuffer"));
            data.end_command_buffer = reinterpret_cast<PFN_vkEndCommandBuffer>(resolve("vkEndCommandBuffer"));
            data.create_fence = reinterpret_cast<PFN_vkCreateFence>(resolve("vkCreateFence"));
            data.destroy_fence = reinterpret_cast<PFN_vkDestroyFence>(resolve("vkDestroyFence"));
            data.reset_fences = reinterpret_cast<PFN_vkResetFences>(resolve("vkResetFences"));
            data.get_fence_status = reinterpret_cast<PFN_vkGetFenceStatus>(resolve("vkGetFenceStatus"));
            data.queue_submit = reinterpret_cast<PFN_vkQueueSubmit>(resolve("vkQueueSubmit"));
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
        this->impl_->queues.emplace(id, impl::queue_data{.handle = queue, .device_id = device});
        out_queue = id;
        return VK_SUCCESS;
    }

    int32_t vulkan_host::create_command_pool(uint64_t device, uint32_t queue_family_index, uint32_t flags, uint64_t& out_pool)
    {
        out_pool = 0;

        const auto dev = this->impl_->devices.find(device);
        if (dev == this->impl_->devices.end() || !dev->second.create_command_pool)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkCommandPoolCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        info.flags = flags;
        info.queueFamilyIndex = queue_family_index;

        VkCommandPool pool{};
        const VkResult result = dev->second.create_command_pool(dev->second.handle, &info, nullptr, &pool);
        if (result != VK_SUCCESS)
        {
            return result;
        }

        const uint64_t id = this->impl_->next_id++;
        this->impl_->command_pools.emplace(id, impl::command_pool_data{.handle = pool, .device_id = device});
        out_pool = id;
        return VK_SUCCESS;
    }

    void vulkan_host::destroy_command_pool(uint64_t device, uint64_t pool)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto it = this->impl_->command_pools.find(pool);
        if (it == this->impl_->command_pools.end())
        {
            return;
        }

        if (dev != this->impl_->devices.end() && it->second.handle && dev->second.destroy_command_pool)
        {
            dev->second.destroy_command_pool(dev->second.handle, it->second.handle, nullptr);
        }

        // Command buffers from this pool are freed implicitly by the driver; drop their ids.
        this->impl_->erase_owned(this->impl_->command_buffers,
                                 [&](const impl::command_buffer_data& d) { return d.pool_id == pool; });
        this->impl_->command_pools.erase(it);
    }

    int32_t vulkan_host::allocate_command_buffer(uint64_t device, uint64_t pool, uint64_t& out_command_buffer)
    {
        out_command_buffer = 0;

        const auto dev = this->impl_->devices.find(device);
        const auto pool_it = this->impl_->command_pools.find(pool);
        if (dev == this->impl_->devices.end() || pool_it == this->impl_->command_pools.end() ||
            !dev->second.allocate_command_buffers)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkCommandBufferAllocateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        info.commandPool = pool_it->second.handle;
        info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        info.commandBufferCount = 1;

        VkCommandBuffer command_buffer{};
        const VkResult result = dev->second.allocate_command_buffers(dev->second.handle, &info, &command_buffer);
        if (result != VK_SUCCESS)
        {
            return result;
        }

        const uint64_t id = this->impl_->next_id++;
        this->impl_->command_buffers.emplace(
            id, impl::command_buffer_data{.handle = command_buffer, .device_id = device, .pool_id = pool});
        out_command_buffer = id;
        return VK_SUCCESS;
    }

    void vulkan_host::free_command_buffer(uint64_t device, uint64_t pool, uint64_t command_buffer)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto pool_it = this->impl_->command_pools.find(pool);
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        if (cb == this->impl_->command_buffers.end())
        {
            return;
        }

        if (dev != this->impl_->devices.end() && pool_it != this->impl_->command_pools.end() && cb->second.handle &&
            dev->second.free_command_buffers)
        {
            dev->second.free_command_buffers(dev->second.handle, pool_it->second.handle, 1, &cb->second.handle);
        }

        this->impl_->command_buffers.erase(cb);
    }

    int32_t vulkan_host::begin_command_buffer(uint64_t command_buffer, uint32_t flags)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        if (cb == this->impl_->command_buffers.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.begin_command_buffer)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkCommandBufferBeginInfo info{};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        info.flags = flags;

        return dev->second.begin_command_buffer(cb->second.handle, &info);
    }

    int32_t vulkan_host::end_command_buffer(uint64_t command_buffer)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        if (cb == this->impl_->command_buffers.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.end_command_buffer)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        return dev->second.end_command_buffer(cb->second.handle);
    }

    int32_t vulkan_host::create_fence(uint64_t device, uint32_t flags, uint64_t& out_fence)
    {
        out_fence = 0;

        const auto dev = this->impl_->devices.find(device);
        if (dev == this->impl_->devices.end() || !dev->second.create_fence)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkFenceCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        info.flags = flags;

        VkFence fence{};
        const VkResult result = dev->second.create_fence(dev->second.handle, &info, nullptr, &fence);
        if (result != VK_SUCCESS)
        {
            return result;
        }

        const uint64_t id = this->impl_->next_id++;
        this->impl_->fences.emplace(id, impl::fence_data{.handle = fence, .device_id = device});
        out_fence = id;
        return VK_SUCCESS;
    }

    void vulkan_host::destroy_fence(uint64_t device, uint64_t fence)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto it = this->impl_->fences.find(fence);
        if (it == this->impl_->fences.end())
        {
            return;
        }

        if (dev != this->impl_->devices.end() && it->second.handle && dev->second.destroy_fence)
        {
            dev->second.destroy_fence(dev->second.handle, it->second.handle, nullptr);
        }

        this->impl_->fences.erase(it);
    }

    int32_t vulkan_host::reset_fence(uint64_t device, uint64_t fence)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto it = this->impl_->fences.find(fence);
        if (dev == this->impl_->devices.end() || it == this->impl_->fences.end() || !dev->second.reset_fences)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        return dev->second.reset_fences(dev->second.handle, 1, &it->second.handle);
    }

    int32_t vulkan_host::get_fence_status(uint64_t fence)
    {
        const auto it = this->impl_->fences.find(fence);
        if (it == this->impl_->fences.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto dev = this->impl_->devices.find(it->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.get_fence_status)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        return dev->second.get_fence_status(dev->second.handle, it->second.handle);
    }

    int32_t vulkan_host::queue_submit(uint64_t queue, uint64_t command_buffer, uint64_t fence)
    {
        const auto queue_it = this->impl_->queues.find(queue);
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        if (queue_it == this->impl_->queues.end() || cb == this->impl_->command_buffers.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto dev = this->impl_->devices.find(queue_it->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.queue_submit)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkFence fence_handle = VK_NULL_HANDLE;
        if (fence != 0)
        {
            const auto fence_it = this->impl_->fences.find(fence);
            if (fence_it == this->impl_->fences.end())
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            fence_handle = fence_it->second.handle;
        }

        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cb->second.handle;

        return dev->second.queue_submit(queue_it->second.handle, 1, &submit, fence_handle);
    }
}
