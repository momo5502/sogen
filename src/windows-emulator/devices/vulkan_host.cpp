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
            PFN_vkGetPhysicalDeviceMemoryProperties get_physical_device_memory_properties{};
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
            VkPhysicalDevice physical_device{}; // the device this was created from (for memory queries)
            uint32_t queue_family_index{};      // the single family this device was created with
            PFN_vkDestroyDevice destroy_device{};
            PFN_vkGetDeviceQueue get_device_queue{};
            PFN_vkQueueWaitIdle queue_wait_idle{};
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
            PFN_vkAllocateMemory allocate_memory{};
            PFN_vkFreeMemory free_memory{};
            PFN_vkMapMemory map_memory{};
            PFN_vkUnmapMemory unmap_memory{};
            PFN_vkCreateBuffer create_buffer{};
            PFN_vkDestroyBuffer destroy_buffer{};
            PFN_vkGetBufferMemoryRequirements get_buffer_memory_requirements{};
            PFN_vkBindBufferMemory bind_buffer_memory{};
            PFN_vkCmdFillBuffer cmd_fill_buffer{};
            PFN_vkCreateImage create_image{};
            PFN_vkDestroyImage destroy_image{};
            PFN_vkGetImageMemoryRequirements get_image_memory_requirements{};
            PFN_vkBindImageMemory bind_image_memory{};
            PFN_vkCmdPipelineBarrier cmd_pipeline_barrier{};
            PFN_vkCmdClearColorImage cmd_clear_color_image{};
            PFN_vkCmdCopyImageToBuffer cmd_copy_image_to_buffer{};
            PFN_vkCreateShaderModule create_shader_module{};
            PFN_vkDestroyShaderModule destroy_shader_module{};
            PFN_vkCreateImageView create_image_view{};
            PFN_vkDestroyImageView destroy_image_view{};
            PFN_vkCreateRenderPass create_render_pass{};
            PFN_vkDestroyRenderPass destroy_render_pass{};
            PFN_vkCreateFramebuffer create_framebuffer{};
            PFN_vkDestroyFramebuffer destroy_framebuffer{};
            PFN_vkCreatePipelineLayout create_pipeline_layout{};
            PFN_vkDestroyPipelineLayout destroy_pipeline_layout{};
            PFN_vkCreateGraphicsPipelines create_graphics_pipelines{};
            PFN_vkDestroyPipeline destroy_pipeline{};
            PFN_vkCmdBeginRenderPass cmd_begin_render_pass{};
            PFN_vkCmdBindPipeline cmd_bind_pipeline{};
            PFN_vkCmdDraw cmd_draw{};
            PFN_vkCmdEndRenderPass cmd_end_render_pass{};
            PFN_vkCmdPushConstants cmd_push_constants{};
        };

        // A guest-side window-system surface. There is no real host VkSurfaceKHR (the host driver may
        // lack Win32 WSI); the bridge only needs the guest HWND to present readback pixels to.
        struct surface_data
        {
            uint64_t hwnd{};
        };

        // A swapchain is modeled as N offscreen images plus a host-visible readback buffer. "Presenting"
        // copies the chosen image into the readback buffer; the bridge then hands those pixels to the
        // guest window via the UI backend. No real presentation engine is involved.
        struct swapchain_data
        {
            uint64_t device_id{};
            uint64_t hwnd{};
            uint32_t width{};
            uint32_t height{};
            VkFormat format{};
            std::vector<uint64_t> image_ids{};        // entries in `images`, owned by this swapchain
            std::vector<VkDeviceMemory> image_memory{};
            VkBuffer readback_buffer{};
            VkDeviceMemory readback_memory{};
            VkCommandPool present_pool{};
            VkCommandBuffer present_cmd{};
            VkFence present_fence{};
            uint32_t next_image{};
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

        struct memory_data
        {
            VkDeviceMemory handle{};
            uint64_t device_id{};
        };

        struct buffer_data
        {
            VkBuffer handle{};
            uint64_t device_id{};
        };

        struct image_data
        {
            VkImage handle{};
            uint64_t device_id{};
        };

        std::unordered_map<uint64_t, device_data> devices;
        std::unordered_map<uint64_t, queue_data> queues;
        std::unordered_map<uint64_t, command_pool_data> command_pools;
        std::unordered_map<uint64_t, command_buffer_data> command_buffers;
        std::unordered_map<uint64_t, fence_data> fences;
        std::unordered_map<uint64_t, memory_data> memories;
        std::unordered_map<uint64_t, buffer_data> buffers;
        std::unordered_map<uint64_t, image_data> images;
        std::unordered_map<uint64_t, surface_data> surfaces;
        std::unordered_map<uint64_t, swapchain_data> swapchains;

        // Graphics-pipeline objects. All non-dispatchable, device-owned, so one shape suffices.
        struct shader_module_data
        {
            VkShaderModule handle{};
            uint64_t device_id{};
        };
        struct image_view_data
        {
            VkImageView handle{};
            uint64_t device_id{};
        };
        struct render_pass_data
        {
            VkRenderPass handle{};
            uint64_t device_id{};
        };
        struct framebuffer_data
        {
            VkFramebuffer handle{};
            uint64_t device_id{};
        };
        struct pipeline_layout_data
        {
            VkPipelineLayout handle{};
            uint64_t device_id{};
        };
        struct pipeline_data
        {
            VkPipeline handle{};
            uint64_t device_id{};
        };

        std::unordered_map<uint64_t, shader_module_data> shader_modules;
        std::unordered_map<uint64_t, image_view_data> image_views;
        std::unordered_map<uint64_t, render_pass_data> render_passes;
        std::unordered_map<uint64_t, framebuffer_data> framebuffers;
        std::unordered_map<uint64_t, pipeline_layout_data> pipeline_layouts;
        std::unordered_map<uint64_t, pipeline_data> pipelines;
        uint64_t next_id{1};

        // Picks the first memory type set in `type_bits` that has all `required` property flags, using
        // the memory properties of the device's physical device. Returns UINT32_MAX if none qualifies.
        uint32_t find_memory_type(const device_data& dev, uint32_t type_bits, VkMemoryPropertyFlags required)
        {
            const auto instance = this->instances.find(dev.instance_id);
            if (instance == this->instances.end() || !instance->second.get_physical_device_memory_properties)
            {
                return UINT32_MAX;
            }

            VkPhysicalDeviceMemoryProperties props{};
            instance->second.get_physical_device_memory_properties(dev.physical_device, &props);

            for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
            {
                if ((type_bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & required) == required)
                {
                    return i;
                }
            }
            return UINT32_MAX;
        }

        template <typename Map, typename Pred>
        void erase_owned(Map& map, Pred pred)
        {
            for (auto it = map.begin(); it != map.end();)
            {
                it = pred(it->second) ? map.erase(it) : std::next(it);
            }
        }

        // Frees every Vulkan resource a swapchain owns (offscreen images + memory, readback buffer +
        // memory, present command pool/fence) and removes its images from the `images` table.
        void destroy_swapchain_resources(swapchain_data& sc)
        {
            const auto dev_it = this->devices.find(sc.device_id);
            if (dev_it == this->devices.end())
            {
                return;
            }
            device_data& dev = dev_it->second;

            for (const uint64_t image_id : sc.image_ids)
            {
                const auto img = this->images.find(image_id);
                if (img != this->images.end())
                {
                    if (img->second.handle && dev.destroy_image)
                    {
                        dev.destroy_image(dev.handle, img->second.handle, nullptr);
                    }
                    this->images.erase(img);
                }
            }
            for (VkDeviceMemory memory : sc.image_memory)
            {
                if (memory && dev.free_memory)
                {
                    dev.free_memory(dev.handle, memory, nullptr);
                }
            }
            if (sc.present_fence && dev.destroy_fence)
            {
                dev.destroy_fence(dev.handle, sc.present_fence, nullptr);
            }
            if (sc.present_pool && dev.destroy_command_pool)
            {
                dev.destroy_command_pool(dev.handle, sc.present_pool, nullptr); // also frees present_cmd
            }
            if (sc.readback_buffer && dev.destroy_buffer)
            {
                dev.destroy_buffer(dev.handle, sc.readback_buffer, nullptr);
            }
            if (sc.readback_memory && dev.free_memory)
            {
                dev.free_memory(dev.handle, sc.readback_memory, nullptr);
            }
        }

        void erase_device(uint64_t device_id)
        {
            const auto it = this->devices.find(device_id);
            if (it == this->devices.end())
            {
                return;
            }

            // Tear down swapchains first: they own offscreen images (in the `images` table), a readback
            // buffer, and a present command pool/fence that must go before the device.
            for (auto sc = this->swapchains.begin(); sc != this->swapchains.end();)
            {
                if (sc->second.device_id == device_id)
                {
                    this->destroy_swapchain_resources(sc->second);
                    sc = this->swapchains.erase(sc);
                }
                else
                {
                    ++sc;
                }
            }

            // Graphics-pipeline objects, in dependency order (framebuffers/pipelines reference render
            // passes, layouts, shader modules and image views; image views reference images).
            for (auto& [id, fb] : this->framebuffers)
            {
                if (fb.device_id == device_id && fb.handle && it->second.destroy_framebuffer)
                {
                    it->second.destroy_framebuffer(it->second.handle, fb.handle, nullptr);
                }
            }
            for (auto& [id, pipe] : this->pipelines)
            {
                if (pipe.device_id == device_id && pipe.handle && it->second.destroy_pipeline)
                {
                    it->second.destroy_pipeline(it->second.handle, pipe.handle, nullptr);
                }
            }
            for (auto& [id, layout] : this->pipeline_layouts)
            {
                if (layout.device_id == device_id && layout.handle && it->second.destroy_pipeline_layout)
                {
                    it->second.destroy_pipeline_layout(it->second.handle, layout.handle, nullptr);
                }
            }
            for (auto& [id, rp] : this->render_passes)
            {
                if (rp.device_id == device_id && rp.handle && it->second.destroy_render_pass)
                {
                    it->second.destroy_render_pass(it->second.handle, rp.handle, nullptr);
                }
            }
            for (auto& [id, view] : this->image_views)
            {
                if (view.device_id == device_id && view.handle && it->second.destroy_image_view)
                {
                    it->second.destroy_image_view(it->second.handle, view.handle, nullptr);
                }
            }
            for (auto& [id, sm] : this->shader_modules)
            {
                if (sm.device_id == device_id && sm.handle && it->second.destroy_shader_module)
                {
                    it->second.destroy_shader_module(it->second.handle, sm.handle, nullptr);
                }
            }
            this->erase_owned(this->framebuffers, [&](const framebuffer_data& d) { return d.device_id == device_id; });
            this->erase_owned(this->pipelines, [&](const pipeline_data& d) { return d.device_id == device_id; });
            this->erase_owned(this->pipeline_layouts, [&](const pipeline_layout_data& d) { return d.device_id == device_id; });
            this->erase_owned(this->render_passes, [&](const render_pass_data& d) { return d.device_id == device_id; });
            this->erase_owned(this->image_views, [&](const image_view_data& d) { return d.device_id == device_id; });
            this->erase_owned(this->shader_modules, [&](const shader_module_data& d) { return d.device_id == device_id; });

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
            // Buffers and images must be destroyed before the memory they are bound to is freed.
            for (auto& [id, buffer] : this->buffers)
            {
                if (buffer.device_id == device_id && buffer.handle && it->second.destroy_buffer)
                {
                    it->second.destroy_buffer(it->second.handle, buffer.handle, nullptr);
                }
            }
            for (auto& [id, image] : this->images)
            {
                if (image.device_id == device_id && image.handle && it->second.destroy_image)
                {
                    it->second.destroy_image(it->second.handle, image.handle, nullptr);
                }
            }
            for (auto& [id, memory] : this->memories)
            {
                if (memory.device_id == device_id && memory.handle && it->second.free_memory)
                {
                    it->second.free_memory(it->second.handle, memory.handle, nullptr);
                }
            }

            this->erase_owned(this->command_buffers, [&](const command_buffer_data& d) { return d.device_id == device_id; });
            this->erase_owned(this->command_pools, [&](const command_pool_data& d) { return d.device_id == device_id; });
            this->erase_owned(this->fences, [&](const fence_data& d) { return d.device_id == device_id; });
            this->erase_owned(this->buffers, [&](const buffer_data& d) { return d.device_id == device_id; });
            this->erase_owned(this->images, [&](const image_data& d) { return d.device_id == device_id; });
            this->erase_owned(this->memories, [&](const memory_data& d) { return d.device_id == device_id; });
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
        data.get_physical_device_memory_properties =
            this->impl_->load_instance_proc<PFN_vkGetPhysicalDeviceMemoryProperties>(instance,
                                                                                     "vkGetPhysicalDeviceMemoryProperties");
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
        data.physical_device = pd->second.handle;
        data.queue_family_index = queue_family_index;

        if (const auto gdpa = instance->second.get_device_proc_addr)
        {
            const auto resolve = [&](const char* name) { return gdpa(device, name); };

            data.destroy_device = reinterpret_cast<PFN_vkDestroyDevice>(resolve("vkDestroyDevice"));
            data.get_device_queue = reinterpret_cast<PFN_vkGetDeviceQueue>(resolve("vkGetDeviceQueue"));
            data.queue_wait_idle = reinterpret_cast<PFN_vkQueueWaitIdle>(resolve("vkQueueWaitIdle"));
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
            data.allocate_memory = reinterpret_cast<PFN_vkAllocateMemory>(resolve("vkAllocateMemory"));
            data.free_memory = reinterpret_cast<PFN_vkFreeMemory>(resolve("vkFreeMemory"));
            data.map_memory = reinterpret_cast<PFN_vkMapMemory>(resolve("vkMapMemory"));
            data.unmap_memory = reinterpret_cast<PFN_vkUnmapMemory>(resolve("vkUnmapMemory"));
            data.create_buffer = reinterpret_cast<PFN_vkCreateBuffer>(resolve("vkCreateBuffer"));
            data.destroy_buffer = reinterpret_cast<PFN_vkDestroyBuffer>(resolve("vkDestroyBuffer"));
            data.get_buffer_memory_requirements =
                reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(resolve("vkGetBufferMemoryRequirements"));
            data.bind_buffer_memory = reinterpret_cast<PFN_vkBindBufferMemory>(resolve("vkBindBufferMemory"));
            data.cmd_fill_buffer = reinterpret_cast<PFN_vkCmdFillBuffer>(resolve("vkCmdFillBuffer"));
            data.create_image = reinterpret_cast<PFN_vkCreateImage>(resolve("vkCreateImage"));
            data.destroy_image = reinterpret_cast<PFN_vkDestroyImage>(resolve("vkDestroyImage"));
            data.get_image_memory_requirements =
                reinterpret_cast<PFN_vkGetImageMemoryRequirements>(resolve("vkGetImageMemoryRequirements"));
            data.bind_image_memory = reinterpret_cast<PFN_vkBindImageMemory>(resolve("vkBindImageMemory"));
            data.cmd_pipeline_barrier = reinterpret_cast<PFN_vkCmdPipelineBarrier>(resolve("vkCmdPipelineBarrier"));
            data.cmd_clear_color_image = reinterpret_cast<PFN_vkCmdClearColorImage>(resolve("vkCmdClearColorImage"));
            data.cmd_copy_image_to_buffer =
                reinterpret_cast<PFN_vkCmdCopyImageToBuffer>(resolve("vkCmdCopyImageToBuffer"));
            data.create_shader_module = reinterpret_cast<PFN_vkCreateShaderModule>(resolve("vkCreateShaderModule"));
            data.destroy_shader_module = reinterpret_cast<PFN_vkDestroyShaderModule>(resolve("vkDestroyShaderModule"));
            data.create_image_view = reinterpret_cast<PFN_vkCreateImageView>(resolve("vkCreateImageView"));
            data.destroy_image_view = reinterpret_cast<PFN_vkDestroyImageView>(resolve("vkDestroyImageView"));
            data.create_render_pass = reinterpret_cast<PFN_vkCreateRenderPass>(resolve("vkCreateRenderPass"));
            data.destroy_render_pass = reinterpret_cast<PFN_vkDestroyRenderPass>(resolve("vkDestroyRenderPass"));
            data.create_framebuffer = reinterpret_cast<PFN_vkCreateFramebuffer>(resolve("vkCreateFramebuffer"));
            data.destroy_framebuffer = reinterpret_cast<PFN_vkDestroyFramebuffer>(resolve("vkDestroyFramebuffer"));
            data.create_pipeline_layout = reinterpret_cast<PFN_vkCreatePipelineLayout>(resolve("vkCreatePipelineLayout"));
            data.destroy_pipeline_layout = reinterpret_cast<PFN_vkDestroyPipelineLayout>(resolve("vkDestroyPipelineLayout"));
            data.create_graphics_pipelines =
                reinterpret_cast<PFN_vkCreateGraphicsPipelines>(resolve("vkCreateGraphicsPipelines"));
            data.destroy_pipeline = reinterpret_cast<PFN_vkDestroyPipeline>(resolve("vkDestroyPipeline"));
            data.cmd_begin_render_pass = reinterpret_cast<PFN_vkCmdBeginRenderPass>(resolve("vkCmdBeginRenderPass"));
            data.cmd_bind_pipeline = reinterpret_cast<PFN_vkCmdBindPipeline>(resolve("vkCmdBindPipeline"));
            data.cmd_draw = reinterpret_cast<PFN_vkCmdDraw>(resolve("vkCmdDraw"));
            data.cmd_end_render_pass = reinterpret_cast<PFN_vkCmdEndRenderPass>(resolve("vkCmdEndRenderPass"));
            data.cmd_push_constants = reinterpret_cast<PFN_vkCmdPushConstants>(resolve("vkCmdPushConstants"));
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
        if (queue_it == this->impl_->queues.end())
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

        // Zero-command-buffer submission: still a valid fence signal operation (no work batched).
        if (command_buffer == 0)
        {
            return dev->second.queue_submit(queue_it->second.handle, 0, nullptr, fence_handle);
        }

        const auto cb = this->impl_->command_buffers.find(command_buffer);
        if (cb == this->impl_->command_buffers.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cb->second.handle;

        return dev->second.queue_submit(queue_it->second.handle, 1, &submit, fence_handle);
    }

    int32_t vulkan_host::get_physical_device_memory_properties(uint64_t physical_device, void* out, size_t out_size)
    {
        const auto pd = this->impl_->physical_devices.find(physical_device);
        if (pd == this->impl_->physical_devices.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto instance = this->impl_->instances.find(pd->second.instance_id);
        if (instance == this->impl_->instances.end() || !instance->second.get_physical_device_memory_properties)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkPhysicalDeviceMemoryProperties properties{};
        instance->second.get_physical_device_memory_properties(pd->second.handle, &properties);

        std::memcpy(out, &properties, std::min(out_size, sizeof(properties)));
        return VK_SUCCESS;
    }

    int32_t vulkan_host::allocate_memory(uint64_t device, uint64_t size, uint32_t memory_type_index, uint64_t& out_memory)
    {
        out_memory = 0;

        const auto dev = this->impl_->devices.find(device);
        if (dev == this->impl_->devices.end() || !dev->second.allocate_memory)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkMemoryAllocateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        info.allocationSize = size;
        info.memoryTypeIndex = memory_type_index;

        VkDeviceMemory memory{};
        const VkResult result = dev->second.allocate_memory(dev->second.handle, &info, nullptr, &memory);
        if (result != VK_SUCCESS)
        {
            return result;
        }

        const uint64_t id = this->impl_->next_id++;
        this->impl_->memories.emplace(id, impl::memory_data{.handle = memory, .device_id = device});
        out_memory = id;
        return VK_SUCCESS;
    }

    void vulkan_host::free_memory(uint64_t device, uint64_t memory)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto it = this->impl_->memories.find(memory);
        if (it == this->impl_->memories.end())
        {
            return;
        }

        if (dev != this->impl_->devices.end() && it->second.handle && dev->second.free_memory)
        {
            dev->second.free_memory(dev->second.handle, it->second.handle, nullptr);
        }

        this->impl_->memories.erase(it);
    }

    int32_t vulkan_host::create_buffer(uint64_t device, uint64_t size, uint32_t usage, uint64_t& out_buffer)
    {
        out_buffer = 0;

        const auto dev = this->impl_->devices.find(device);
        if (dev == this->impl_->devices.end() || !dev->second.create_buffer)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = size;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VkBuffer buffer{};
        const VkResult result = dev->second.create_buffer(dev->second.handle, &info, nullptr, &buffer);
        if (result != VK_SUCCESS)
        {
            return result;
        }

        const uint64_t id = this->impl_->next_id++;
        this->impl_->buffers.emplace(id, impl::buffer_data{.handle = buffer, .device_id = device});
        out_buffer = id;
        return VK_SUCCESS;
    }

    void vulkan_host::destroy_buffer(uint64_t device, uint64_t buffer)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto it = this->impl_->buffers.find(buffer);
        if (it == this->impl_->buffers.end())
        {
            return;
        }

        if (dev != this->impl_->devices.end() && it->second.handle && dev->second.destroy_buffer)
        {
            dev->second.destroy_buffer(dev->second.handle, it->second.handle, nullptr);
        }

        this->impl_->buffers.erase(it);
    }

    int32_t vulkan_host::get_buffer_memory_requirements(uint64_t device, uint64_t buffer, uint64_t& out_size,
                                                        uint64_t& out_alignment, uint32_t& out_memory_type_bits)
    {
        out_size = 0;
        out_alignment = 0;
        out_memory_type_bits = 0;

        const auto dev = this->impl_->devices.find(device);
        const auto buf = this->impl_->buffers.find(buffer);
        if (dev == this->impl_->devices.end() || buf == this->impl_->buffers.end() ||
            !dev->second.get_buffer_memory_requirements)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkMemoryRequirements requirements{};
        dev->second.get_buffer_memory_requirements(dev->second.handle, buf->second.handle, &requirements);

        out_size = requirements.size;
        out_alignment = requirements.alignment;
        out_memory_type_bits = requirements.memoryTypeBits;
        return VK_SUCCESS;
    }

    int32_t vulkan_host::bind_buffer_memory(uint64_t device, uint64_t buffer, uint64_t memory, uint64_t offset)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto buf = this->impl_->buffers.find(buffer);
        const auto mem = this->impl_->memories.find(memory);
        if (dev == this->impl_->devices.end() || buf == this->impl_->buffers.end() || mem == this->impl_->memories.end() ||
            !dev->second.bind_buffer_memory)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        return dev->second.bind_buffer_memory(dev->second.handle, buf->second.handle, mem->second.handle, offset);
    }

    int32_t vulkan_host::cmd_fill_buffer(uint64_t command_buffer, uint64_t buffer, uint64_t offset, uint64_t size,
                                         uint32_t data)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        const auto buf = this->impl_->buffers.find(buffer);
        if (cb == this->impl_->command_buffers.end() || buf == this->impl_->buffers.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_fill_buffer)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        dev->second.cmd_fill_buffer(cb->second.handle, buf->second.handle, offset, size, data);
        return VK_SUCCESS;
    }

    int32_t vulkan_host::download_memory(uint64_t device, uint64_t memory, uint64_t offset, uint64_t size, void* out,
                                         size_t out_size)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto mem = this->impl_->memories.find(memory);
        if (dev == this->impl_->devices.end() || mem == this->impl_->memories.end() || !dev->second.map_memory ||
            !dev->second.unmap_memory)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const size_t copy_bytes = std::min(static_cast<size_t>(size), out_size);

        void* mapped = nullptr;
        const VkResult result = dev->second.map_memory(dev->second.handle, mem->second.handle, offset, size, 0, &mapped);
        if (result != VK_SUCCESS || !mapped)
        {
            return result != VK_SUCCESS ? result : VK_ERROR_MEMORY_MAP_FAILED;
        }

        std::memcpy(out, mapped, copy_bytes);
        dev->second.unmap_memory(dev->second.handle, mem->second.handle);
        return VK_SUCCESS;
    }

    int32_t vulkan_host::upload_memory(uint64_t device, uint64_t memory, uint64_t offset, uint64_t size, const void* data,
                                       size_t data_size)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto mem = this->impl_->memories.find(memory);
        if (dev == this->impl_->devices.end() || mem == this->impl_->memories.end() || !dev->second.map_memory ||
            !dev->second.unmap_memory)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const size_t copy_bytes = std::min(static_cast<size_t>(size), data_size);

        void* mapped = nullptr;
        const VkResult result = dev->second.map_memory(dev->second.handle, mem->second.handle, offset, size, 0, &mapped);
        if (result != VK_SUCCESS || !mapped)
        {
            return result != VK_SUCCESS ? result : VK_ERROR_MEMORY_MAP_FAILED;
        }

        std::memcpy(mapped, data, copy_bytes);
        dev->second.unmap_memory(dev->second.handle, mem->second.handle);
        return VK_SUCCESS;
    }

    int32_t vulkan_host::create_image(uint64_t device, uint32_t format, uint32_t width, uint32_t height, uint32_t usage,
                                      uint32_t tiling, uint64_t& out_image)
    {
        out_image = 0;

        const auto dev = this->impl_->devices.find(device);
        if (dev == this->impl_->devices.end() || !dev->second.create_image)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkImageCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = static_cast<VkFormat>(format);
        info.extent = {.width = width, .height = height, .depth = 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = static_cast<VkImageTiling>(tiling);
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VkImage image{};
        const VkResult result = dev->second.create_image(dev->second.handle, &info, nullptr, &image);
        if (result != VK_SUCCESS)
        {
            return result;
        }

        const uint64_t id = this->impl_->next_id++;
        this->impl_->images.emplace(id, impl::image_data{.handle = image, .device_id = device});
        out_image = id;
        return VK_SUCCESS;
    }

    void vulkan_host::destroy_image(uint64_t device, uint64_t image)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto it = this->impl_->images.find(image);
        if (it == this->impl_->images.end())
        {
            return;
        }

        if (dev != this->impl_->devices.end() && it->second.handle && dev->second.destroy_image)
        {
            dev->second.destroy_image(dev->second.handle, it->second.handle, nullptr);
        }

        this->impl_->images.erase(it);
    }

    int32_t vulkan_host::get_image_memory_requirements(uint64_t device, uint64_t image, uint64_t& out_size,
                                                       uint64_t& out_alignment, uint32_t& out_memory_type_bits)
    {
        out_size = 0;
        out_alignment = 0;
        out_memory_type_bits = 0;

        const auto dev = this->impl_->devices.find(device);
        const auto img = this->impl_->images.find(image);
        if (dev == this->impl_->devices.end() || img == this->impl_->images.end() ||
            !dev->second.get_image_memory_requirements)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkMemoryRequirements requirements{};
        dev->second.get_image_memory_requirements(dev->second.handle, img->second.handle, &requirements);

        out_size = requirements.size;
        out_alignment = requirements.alignment;
        out_memory_type_bits = requirements.memoryTypeBits;
        return VK_SUCCESS;
    }

    int32_t vulkan_host::bind_image_memory(uint64_t device, uint64_t image, uint64_t memory, uint64_t offset)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto img = this->impl_->images.find(image);
        const auto mem = this->impl_->memories.find(memory);
        if (dev == this->impl_->devices.end() || img == this->impl_->images.end() || mem == this->impl_->memories.end() ||
            !dev->second.bind_image_memory)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        return dev->second.bind_image_memory(dev->second.handle, img->second.handle, mem->second.handle, offset);
    }

    namespace
    {
        VkImageSubresourceRange to_vk_range(const vulkan_host::subresource_range& range)
        {
            VkImageSubresourceRange out{};
            out.aspectMask = range.aspect_mask;
            out.baseMipLevel = range.base_mip_level;
            out.levelCount = range.level_count;
            out.baseArrayLayer = range.base_array_layer;
            out.layerCount = range.layer_count;
            return out;
        }

        // The bridge's swapchain images are ordinary offscreen VkImages with no real presentation
        // engine, so the host driver would reject VK_IMAGE_LAYOUT_PRESENT_SRC_KHR. Map it to
        // TRANSFER_SRC_OPTIMAL (the layout the present-time readback copy reads from), keeping the guest
        // a faithful WSI app while the real driver stays valid.
        VkImageLayout translate_layout(uint32_t layout)
        {
            if (layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
            {
                return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            }
            return static_cast<VkImageLayout>(layout);
        }
    }

    int32_t vulkan_host::cmd_pipeline_barrier(uint64_t command_buffer, uint64_t image, uint32_t src_stage_mask,
                                              uint32_t dst_stage_mask, uint32_t src_access_mask, uint32_t dst_access_mask,
                                              uint32_t old_layout, uint32_t new_layout, const subresource_range& range)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        const auto img = this->impl_->images.find(image);
        if (cb == this->impl_->command_buffers.end() || img == this->impl_->images.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_pipeline_barrier)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = src_access_mask;
        barrier.dstAccessMask = dst_access_mask;
        barrier.oldLayout = translate_layout(old_layout);
        barrier.newLayout = translate_layout(new_layout);
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = img->second.handle;
        barrier.subresourceRange = to_vk_range(range);

        dev->second.cmd_pipeline_barrier(cb->second.handle, src_stage_mask, dst_stage_mask, 0, 0, nullptr, 0, nullptr, 1,
                                         &barrier);
        return VK_SUCCESS;
    }

    int32_t vulkan_host::cmd_clear_color_image(uint64_t command_buffer, uint64_t image, uint32_t image_layout, float r,
                                               float g, float b, float a, const subresource_range& range)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        const auto img = this->impl_->images.find(image);
        if (cb == this->impl_->command_buffers.end() || img == this->impl_->images.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_clear_color_image)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkClearColorValue clear{};
        clear.float32[0] = r;
        clear.float32[1] = g;
        clear.float32[2] = b;
        clear.float32[3] = a;

        const VkImageSubresourceRange vk_range = to_vk_range(range);
        dev->second.cmd_clear_color_image(cb->second.handle, img->second.handle, translate_layout(image_layout), &clear, 1,
                                          &vk_range);
        return VK_SUCCESS;
    }

    int32_t vulkan_host::cmd_copy_image_to_buffer(uint64_t command_buffer, uint64_t image, uint32_t image_layout,
                                                  uint64_t buffer, uint32_t width, uint32_t height, uint32_t aspect_mask)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        const auto img = this->impl_->images.find(image);
        const auto buf = this->impl_->buffers.find(buffer);
        if (cb == this->impl_->command_buffers.end() || img == this->impl_->images.end() ||
            buf == this->impl_->buffers.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_copy_image_to_buffer)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkBufferImageCopy region{};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;   // tightly packed
        region.bufferImageHeight = 0; // tightly packed
        region.imageSubresource.aspectMask = aspect_mask;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {.x = 0, .y = 0, .z = 0};
        region.imageExtent = {.width = width, .height = height, .depth = 1};

        dev->second.cmd_copy_image_to_buffer(cb->second.handle, img->second.handle, translate_layout(image_layout),
                                             buf->second.handle, 1, &region);
        return VK_SUCCESS;
    }

    int32_t vulkan_host::create_surface(uint64_t hwnd, uint64_t& out_surface)
    {
        const uint64_t id = this->impl_->next_id++;
        this->impl_->surfaces.emplace(id, impl::surface_data{.hwnd = hwnd});
        out_surface = id;
        return VK_SUCCESS;
    }

    void vulkan_host::destroy_surface(uint64_t surface)
    {
        this->impl_->surfaces.erase(surface);
    }

    int32_t vulkan_host::get_surface_capabilities(uint64_t /*physical_device*/, uint64_t surface, void* out,
                                                  size_t out_size)
    {
        if (!this->impl_->surfaces.contains(surface))
        {
            return VK_ERROR_SURFACE_LOST_KHR;
        }

        VkSurfaceCapabilitiesKHR caps{};
        caps.minImageCount = 2;
        caps.maxImageCount = 0; // no upper bound
        caps.currentExtent = {.width = 0xFFFFFFFFu, .height = 0xFFFFFFFFu}; // undefined: the app chooses
        caps.minImageExtent = {.width = 1, .height = 1};
        caps.maxImageExtent = {.width = 16384, .height = 16384};
        caps.maxImageArrayLayers = 1;
        caps.supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        caps.currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        caps.supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        caps.supportedUsageFlags =
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

        std::memcpy(out, &caps, std::min(out_size, sizeof(caps)));
        return VK_SUCCESS;
    }

    int32_t vulkan_host::create_swapchain(uint64_t device, uint64_t surface, uint32_t format, uint32_t width,
                                          uint32_t height, uint32_t min_image_count, uint32_t image_usage,
                                          uint64_t& out_swapchain, uint32_t& out_image_count)
    {
        out_swapchain = 0;
        out_image_count = 0;

        const auto dev_it = this->impl_->devices.find(device);
        const auto surf_it = this->impl_->surfaces.find(surface);
        if (dev_it == this->impl_->devices.end() || surf_it == this->impl_->surfaces.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        impl::device_data& dev = dev_it->second;
        if (!dev.create_image || !dev.allocate_memory || !dev.bind_image_memory || !dev.create_buffer ||
            !dev.bind_buffer_memory || !dev.create_command_pool || !dev.allocate_command_buffers || !dev.create_fence)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const uint32_t image_count = (min_image_count < 2) ? 2 : min_image_count;
        const auto vk_format = static_cast<VkFormat>(format);

        impl::swapchain_data sc{};
        sc.device_id = device;
        sc.hwnd = surf_it->second.hwnd;
        sc.width = width;
        sc.height = height;
        sc.format = vk_format;

        const auto fail = [&]() -> int32_t {
            this->impl_->destroy_swapchain_resources(sc);
            return VK_ERROR_INITIALIZATION_FAILED;
        };

        // Offscreen images that stand in for the swapchain's presentable images. TRANSFER_SRC is always
        // added so the present-time readback copy can source them.
        for (uint32_t i = 0; i < image_count; ++i)
        {
            VkImageCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            info.imageType = VK_IMAGE_TYPE_2D;
            info.format = vk_format;
            info.extent = {.width = width, .height = height, .depth = 1};
            info.mipLevels = 1;
            info.arrayLayers = 1;
            info.samples = VK_SAMPLE_COUNT_1_BIT;
            info.tiling = VK_IMAGE_TILING_OPTIMAL;
            info.usage = image_usage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

            VkImage image{};
            if (dev.create_image(dev.handle, &info, nullptr, &image) != VK_SUCCESS)
            {
                return fail();
            }

            VkMemoryRequirements reqs{};
            dev.get_image_memory_requirements(dev.handle, image, &reqs);
            uint32_t type_index = this->impl_->find_memory_type(dev, reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (type_index == UINT32_MAX)
            {
                type_index = this->impl_->find_memory_type(dev, reqs.memoryTypeBits, 0);
            }

            VkMemoryAllocateInfo alloc{};
            alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            alloc.allocationSize = reqs.size;
            alloc.memoryTypeIndex = type_index;
            VkDeviceMemory memory{};
            if (dev.allocate_memory(dev.handle, &alloc, nullptr, &memory) != VK_SUCCESS)
            {
                dev.destroy_image(dev.handle, image, nullptr);
                return fail();
            }
            dev.bind_image_memory(dev.handle, image, memory, 0);

            const uint64_t image_id = this->impl_->next_id++;
            this->impl_->images.emplace(image_id, impl::image_data{.handle = image, .device_id = device});
            sc.image_ids.push_back(image_id);
            sc.image_memory.push_back(memory);
        }

        // Host-visible readback buffer the presented image is copied into (4 bytes/pixel).
        const VkDeviceSize readback_size = static_cast<VkDeviceSize>(width) * height * 4;
        VkBufferCreateInfo buffer_info{};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = readback_size;
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (dev.create_buffer(dev.handle, &buffer_info, nullptr, &sc.readback_buffer) != VK_SUCCESS)
        {
            return fail();
        }

        VkMemoryRequirements buffer_reqs{};
        dev.get_buffer_memory_requirements(dev.handle, sc.readback_buffer, &buffer_reqs);
        const uint32_t buffer_type = this->impl_->find_memory_type(
            dev, buffer_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (buffer_type == UINT32_MAX)
        {
            return fail();
        }
        VkMemoryAllocateInfo buffer_alloc{};
        buffer_alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        buffer_alloc.allocationSize = buffer_reqs.size;
        buffer_alloc.memoryTypeIndex = buffer_type;
        if (dev.allocate_memory(dev.handle, &buffer_alloc, nullptr, &sc.readback_memory) != VK_SUCCESS)
        {
            return fail();
        }
        dev.bind_buffer_memory(dev.handle, sc.readback_buffer, sc.readback_memory, 0);

        // Reusable command buffer + fence for the per-present readback copy.
        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = dev.queue_family_index;
        if (dev.create_command_pool(dev.handle, &pool_info, nullptr, &sc.present_pool) != VK_SUCCESS)
        {
            return fail();
        }

        VkCommandBufferAllocateInfo cb_info{};
        cb_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cb_info.commandPool = sc.present_pool;
        cb_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cb_info.commandBufferCount = 1;
        if (dev.allocate_command_buffers(dev.handle, &cb_info, &sc.present_cmd) != VK_SUCCESS)
        {
            return fail();
        }

        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (dev.create_fence(dev.handle, &fence_info, nullptr, &sc.present_fence) != VK_SUCCESS)
        {
            return fail();
        }

        const uint64_t id = this->impl_->next_id++;
        out_image_count = image_count;
        this->impl_->swapchains.emplace(id, std::move(sc));
        out_swapchain = id;
        return VK_SUCCESS;
    }

    void vulkan_host::destroy_swapchain(uint64_t /*device*/, uint64_t swapchain)
    {
        const auto it = this->impl_->swapchains.find(swapchain);
        if (it == this->impl_->swapchains.end())
        {
            return;
        }
        this->impl_->destroy_swapchain_resources(it->second);
        this->impl_->swapchains.erase(it);
    }

    int32_t vulkan_host::get_swapchain_images(uint64_t swapchain, std::span<uint64_t> out_images, uint32_t& out_count)
    {
        out_count = 0;
        const auto it = this->impl_->swapchains.find(swapchain);
        if (it == this->impl_->swapchains.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        out_count = static_cast<uint32_t>(it->second.image_ids.size());
        for (size_t i = 0; i < it->second.image_ids.size() && i < out_images.size(); ++i)
        {
            out_images[i] = it->second.image_ids[i];
        }
        return VK_SUCCESS;
    }

    int32_t vulkan_host::acquire_next_image(uint64_t swapchain, uint32_t& out_index)
    {
        out_index = 0;
        const auto it = this->impl_->swapchains.find(swapchain);
        if (it == this->impl_->swapchains.end() || it->second.image_ids.empty())
        {
            return VK_ERROR_OUT_OF_DATE_KHR;
        }

        // No real presentation engine: images are always available, handed out round-robin.
        out_index = it->second.next_image;
        it->second.next_image = (it->second.next_image + 1) % static_cast<uint32_t>(it->second.image_ids.size());
        return VK_SUCCESS;
    }

    int32_t vulkan_host::queue_present(uint64_t queue, uint64_t swapchain, uint32_t image_index,
                                       std::vector<std::byte>& out_pixels, uint32_t& out_width, uint32_t& out_height,
                                       uint64_t& out_hwnd)
    {
        out_pixels.clear();
        out_width = 0;
        out_height = 0;
        out_hwnd = 0;

        const auto queue_it = this->impl_->queues.find(queue);
        const auto sc_it = this->impl_->swapchains.find(swapchain);
        if (queue_it == this->impl_->queues.end() || sc_it == this->impl_->swapchains.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        impl::swapchain_data& sc = sc_it->second;
        if (image_index >= sc.image_ids.size())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto dev_it = this->impl_->devices.find(sc.device_id);
        const auto img_it = this->impl_->images.find(sc.image_ids[image_index]);
        if (dev_it == this->impl_->devices.end() || img_it == this->impl_->images.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        impl::device_data& dev = dev_it->second;
        if (!dev.begin_command_buffer || !dev.end_command_buffer || !dev.cmd_copy_image_to_buffer || !dev.queue_submit ||
            !dev.queue_wait_idle || !dev.map_memory || !dev.unmap_memory || !dev.cmd_pipeline_barrier || !dev.reset_fences)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        // Record: make the rendered image's writes visible to the copy, then copy it into the readback
        // buffer. The guest left the image in PRESENT_SRC, which the bridge mapped to TRANSFER_SRC_OPTIMAL.
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (dev.begin_command_buffer(sc.present_cmd, &begin) != VK_SUCCESS)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = img_it->second.handle;
        barrier.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                    .baseMipLevel = 0,
                                    .levelCount = 1,
                                    .baseArrayLayer = 0,
                                    .layerCount = 1};
        dev.cmd_pipeline_barrier(sc.present_cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &barrier);

        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {.x = 0, .y = 0, .z = 0};
        region.imageExtent = {.width = sc.width, .height = sc.height, .depth = 1};
        dev.cmd_copy_image_to_buffer(sc.present_cmd, img_it->second.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                     sc.readback_buffer, 1, &region);
        dev.end_command_buffer(sc.present_cmd);

        dev.reset_fences(dev.handle, 1, &sc.present_fence);

        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &sc.present_cmd;
        if (dev.queue_submit(queue_it->second.handle, 1, &submit, sc.present_fence) != VK_SUCCESS)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        // The readback must complete before we can hand pixels to the UI. This blocks the host thread,
        // but only briefly (a small transfer) and independently of guest progress.
        dev.queue_wait_idle(queue_it->second.handle);

        const VkDeviceSize readback_size = static_cast<VkDeviceSize>(sc.width) * sc.height * 4;
        void* mapped = nullptr;
        if (dev.map_memory(dev.handle, sc.readback_memory, 0, readback_size, 0, &mapped) != VK_SUCCESS || !mapped)
        {
            return VK_ERROR_MEMORY_MAP_FAILED;
        }
        out_pixels.resize(static_cast<size_t>(readback_size));
        std::memcpy(out_pixels.data(), mapped, out_pixels.size());
        dev.unmap_memory(dev.handle, sc.readback_memory);

        out_width = sc.width;
        out_height = sc.height;
        out_hwnd = sc.hwnd;
        return VK_SUCCESS;
    }

    int32_t vulkan_host::create_shader_module(uint64_t device, const void* code, size_t code_size, uint64_t& out_module)
    {
        out_module = 0;
        const auto dev = this->impl_->devices.find(device);
        if (dev == this->impl_->devices.end() || !dev->second.create_shader_module)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkShaderModuleCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = code_size;
        info.pCode = static_cast<const uint32_t*>(code);

        VkShaderModule module{};
        const VkResult result = dev->second.create_shader_module(dev->second.handle, &info, nullptr, &module);
        if (result != VK_SUCCESS)
        {
            return result;
        }

        const uint64_t id = this->impl_->next_id++;
        this->impl_->shader_modules.emplace(id, impl::shader_module_data{.handle = module, .device_id = device});
        out_module = id;
        return VK_SUCCESS;
    }

    void vulkan_host::destroy_shader_module(uint64_t device, uint64_t shader_module)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto it = this->impl_->shader_modules.find(shader_module);
        if (it == this->impl_->shader_modules.end())
        {
            return;
        }
        if (dev != this->impl_->devices.end() && it->second.handle && dev->second.destroy_shader_module)
        {
            dev->second.destroy_shader_module(dev->second.handle, it->second.handle, nullptr);
        }
        this->impl_->shader_modules.erase(it);
    }

    int32_t vulkan_host::create_image_view(uint64_t device, uint64_t image, uint32_t format, uint64_t& out_view)
    {
        out_view = 0;
        const auto dev = this->impl_->devices.find(device);
        const auto img = this->impl_->images.find(image);
        if (dev == this->impl_->devices.end() || img == this->impl_->images.end() || !dev->second.create_image_view)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkImageViewCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        info.image = img->second.handle;
        info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        info.format = static_cast<VkFormat>(format);
        info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        info.subresourceRange.levelCount = 1;
        info.subresourceRange.layerCount = 1;

        VkImageView view{};
        const VkResult result = dev->second.create_image_view(dev->second.handle, &info, nullptr, &view);
        if (result != VK_SUCCESS)
        {
            return result;
        }

        const uint64_t id = this->impl_->next_id++;
        this->impl_->image_views.emplace(id, impl::image_view_data{.handle = view, .device_id = device});
        out_view = id;
        return VK_SUCCESS;
    }

    void vulkan_host::destroy_image_view(uint64_t device, uint64_t image_view)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto it = this->impl_->image_views.find(image_view);
        if (it == this->impl_->image_views.end())
        {
            return;
        }
        if (dev != this->impl_->devices.end() && it->second.handle && dev->second.destroy_image_view)
        {
            dev->second.destroy_image_view(dev->second.handle, it->second.handle, nullptr);
        }
        this->impl_->image_views.erase(it);
    }

    int32_t vulkan_host::create_render_pass(uint64_t device, uint32_t format, uint32_t load_op, uint32_t store_op,
                                            uint32_t initial_layout, uint32_t final_layout, uint64_t& out_render_pass)
    {
        out_render_pass = 0;
        const auto dev = this->impl_->devices.find(device);
        if (dev == this->impl_->devices.end() || !dev->second.create_render_pass)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkAttachmentDescription color{};
        color.format = static_cast<VkFormat>(format);
        color.samples = VK_SAMPLE_COUNT_1_BIT;
        color.loadOp = static_cast<VkAttachmentLoadOp>(load_op);
        color.storeOp = static_cast<VkAttachmentStoreOp>(store_op);
        color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.initialLayout = translate_layout(initial_layout);
        color.finalLayout = translate_layout(final_layout);

        VkAttachmentReference ref{};
        ref.attachment = 0;
        ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &ref;

        VkSubpassDependency dep{};
        dep.srcSubpass = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass = 0;
        dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.srcAccessMask = 0;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        info.attachmentCount = 1;
        info.pAttachments = &color;
        info.subpassCount = 1;
        info.pSubpasses = &subpass;
        info.dependencyCount = 1;
        info.pDependencies = &dep;

        VkRenderPass render_pass{};
        const VkResult result = dev->second.create_render_pass(dev->second.handle, &info, nullptr, &render_pass);
        if (result != VK_SUCCESS)
        {
            return result;
        }

        const uint64_t id = this->impl_->next_id++;
        this->impl_->render_passes.emplace(id, impl::render_pass_data{.handle = render_pass, .device_id = device});
        out_render_pass = id;
        return VK_SUCCESS;
    }

    void vulkan_host::destroy_render_pass(uint64_t device, uint64_t render_pass)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto it = this->impl_->render_passes.find(render_pass);
        if (it == this->impl_->render_passes.end())
        {
            return;
        }
        if (dev != this->impl_->devices.end() && it->second.handle && dev->second.destroy_render_pass)
        {
            dev->second.destroy_render_pass(dev->second.handle, it->second.handle, nullptr);
        }
        this->impl_->render_passes.erase(it);
    }

    int32_t vulkan_host::create_framebuffer(uint64_t device, uint64_t render_pass, uint64_t image_view, uint32_t width,
                                            uint32_t height, uint64_t& out_framebuffer)
    {
        out_framebuffer = 0;
        const auto dev = this->impl_->devices.find(device);
        const auto rp = this->impl_->render_passes.find(render_pass);
        const auto view = this->impl_->image_views.find(image_view);
        if (dev == this->impl_->devices.end() || rp == this->impl_->render_passes.end() ||
            view == this->impl_->image_views.end() || !dev->second.create_framebuffer)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkFramebufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        info.renderPass = rp->second.handle;
        info.attachmentCount = 1;
        info.pAttachments = &view->second.handle;
        info.width = width;
        info.height = height;
        info.layers = 1;

        VkFramebuffer framebuffer{};
        const VkResult result = dev->second.create_framebuffer(dev->second.handle, &info, nullptr, &framebuffer);
        if (result != VK_SUCCESS)
        {
            return result;
        }

        const uint64_t id = this->impl_->next_id++;
        this->impl_->framebuffers.emplace(id, impl::framebuffer_data{.handle = framebuffer, .device_id = device});
        out_framebuffer = id;
        return VK_SUCCESS;
    }

    void vulkan_host::destroy_framebuffer(uint64_t device, uint64_t framebuffer)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto it = this->impl_->framebuffers.find(framebuffer);
        if (it == this->impl_->framebuffers.end())
        {
            return;
        }
        if (dev != this->impl_->devices.end() && it->second.handle && dev->second.destroy_framebuffer)
        {
            dev->second.destroy_framebuffer(dev->second.handle, it->second.handle, nullptr);
        }
        this->impl_->framebuffers.erase(it);
    }

    int32_t vulkan_host::create_pipeline_layout(uint64_t device, uint32_t push_constant_stages, uint32_t push_constant_size,
                                                uint64_t& out_layout)
    {
        out_layout = 0;
        const auto dev = this->impl_->devices.find(device);
        if (dev == this->impl_->devices.end() || !dev->second.create_pipeline_layout)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkPushConstantRange push_range{};
        push_range.stageFlags = push_constant_stages;
        push_range.offset = 0;
        push_range.size = push_constant_size;

        VkPipelineLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        if (push_constant_size > 0)
        {
            info.pushConstantRangeCount = 1;
            info.pPushConstantRanges = &push_range;
        }

        VkPipelineLayout layout{};
        const VkResult result = dev->second.create_pipeline_layout(dev->second.handle, &info, nullptr, &layout);
        if (result != VK_SUCCESS)
        {
            return result;
        }

        const uint64_t id = this->impl_->next_id++;
        this->impl_->pipeline_layouts.emplace(id, impl::pipeline_layout_data{.handle = layout, .device_id = device});
        out_layout = id;
        return VK_SUCCESS;
    }

    void vulkan_host::destroy_pipeline_layout(uint64_t device, uint64_t pipeline_layout)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto it = this->impl_->pipeline_layouts.find(pipeline_layout);
        if (it == this->impl_->pipeline_layouts.end())
        {
            return;
        }
        if (dev != this->impl_->devices.end() && it->second.handle && dev->second.destroy_pipeline_layout)
        {
            dev->second.destroy_pipeline_layout(dev->second.handle, it->second.handle, nullptr);
        }
        this->impl_->pipeline_layouts.erase(it);
    }

    int32_t vulkan_host::create_graphics_pipeline(uint64_t device, uint64_t render_pass, uint64_t pipeline_layout,
                                                  uint64_t vertex_shader, uint64_t fragment_shader, uint32_t width,
                                                  uint32_t height, uint64_t& out_pipeline)
    {
        out_pipeline = 0;
        const auto dev = this->impl_->devices.find(device);
        const auto rp = this->impl_->render_passes.find(render_pass);
        const auto layout = this->impl_->pipeline_layouts.find(pipeline_layout);
        const auto vert = this->impl_->shader_modules.find(vertex_shader);
        const auto frag = this->impl_->shader_modules.find(fragment_shader);
        if (dev == this->impl_->devices.end() || rp == this->impl_->render_passes.end() ||
            layout == this->impl_->pipeline_layouts.end() || vert == this->impl_->shader_modules.end() ||
            frag == this->impl_->shader_modules.end() || !dev->second.create_graphics_pipelines)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vert->second.handle;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = frag->second.handle;
        stages[1].pName = "main";

        VkPipelineVertexInputStateCreateInfo vertex_input{};
        vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

        VkPipelineInputAssemblyStateCreateInfo input_assembly{};
        input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkViewport viewport{};
        viewport.width = static_cast<float>(width);
        viewport.height = static_cast<float>(height);
        viewport.maxDepth = 1.0f;
        VkRect2D scissor{};
        scissor.extent = {.width = width, .height = height};

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

        VkGraphicsPipelineCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        info.stageCount = static_cast<uint32_t>(stages.size());
        info.pStages = stages.data();
        info.pVertexInputState = &vertex_input;
        info.pInputAssemblyState = &input_assembly;
        info.pViewportState = &viewport_state;
        info.pRasterizationState = &rasterization;
        info.pMultisampleState = &multisample;
        info.pColorBlendState = &color_blend;
        info.layout = layout->second.handle;
        info.renderPass = rp->second.handle;
        info.subpass = 0;

        VkPipeline pipeline{};
        const VkResult result =
            dev->second.create_graphics_pipelines(dev->second.handle, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
        if (result != VK_SUCCESS)
        {
            return result;
        }

        const uint64_t id = this->impl_->next_id++;
        this->impl_->pipelines.emplace(id, impl::pipeline_data{.handle = pipeline, .device_id = device});
        out_pipeline = id;
        return VK_SUCCESS;
    }

    void vulkan_host::destroy_pipeline(uint64_t device, uint64_t pipeline)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto it = this->impl_->pipelines.find(pipeline);
        if (it == this->impl_->pipelines.end())
        {
            return;
        }
        if (dev != this->impl_->devices.end() && it->second.handle && dev->second.destroy_pipeline)
        {
            dev->second.destroy_pipeline(dev->second.handle, it->second.handle, nullptr);
        }
        this->impl_->pipelines.erase(it);
    }

    int32_t vulkan_host::cmd_begin_render_pass(uint64_t command_buffer, uint64_t render_pass, uint64_t framebuffer,
                                               uint32_t width, uint32_t height, float r, float g, float b, float a)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        const auto rp = this->impl_->render_passes.find(render_pass);
        const auto fb = this->impl_->framebuffers.find(framebuffer);
        if (cb == this->impl_->command_buffers.end() || rp == this->impl_->render_passes.end() ||
            fb == this->impl_->framebuffers.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_begin_render_pass)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkClearValue clear{};
        clear.color.float32[0] = r;
        clear.color.float32[1] = g;
        clear.color.float32[2] = b;
        clear.color.float32[3] = a;

        VkRenderPassBeginInfo info{};
        info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        info.renderPass = rp->second.handle;
        info.framebuffer = fb->second.handle;
        info.renderArea.extent = {.width = width, .height = height};
        info.clearValueCount = 1;
        info.pClearValues = &clear;

        dev->second.cmd_begin_render_pass(cb->second.handle, &info, VK_SUBPASS_CONTENTS_INLINE);
        return VK_SUCCESS;
    }

    int32_t vulkan_host::cmd_bind_pipeline(uint64_t command_buffer, uint64_t pipeline)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        const auto pipe = this->impl_->pipelines.find(pipeline);
        if (cb == this->impl_->command_buffers.end() || pipe == this->impl_->pipelines.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_bind_pipeline)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        dev->second.cmd_bind_pipeline(cb->second.handle, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe->second.handle);
        return VK_SUCCESS;
    }

    int32_t vulkan_host::cmd_draw(uint64_t command_buffer, uint32_t vertex_count, uint32_t instance_count,
                                  uint32_t first_vertex, uint32_t first_instance)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        if (cb == this->impl_->command_buffers.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_draw)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        dev->second.cmd_draw(cb->second.handle, vertex_count, instance_count, first_vertex, first_instance);
        return VK_SUCCESS;
    }

    int32_t vulkan_host::cmd_end_render_pass(uint64_t command_buffer)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        if (cb == this->impl_->command_buffers.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_end_render_pass)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        dev->second.cmd_end_render_pass(cb->second.handle);
        return VK_SUCCESS;
    }

    int32_t vulkan_host::cmd_push_constants(uint64_t command_buffer, uint64_t pipeline_layout, uint32_t stage_flags,
                                            uint32_t offset, uint32_t size, const void* data)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        const auto layout = this->impl_->pipeline_layouts.find(pipeline_layout);
        if (cb == this->impl_->command_buffers.end() || layout == this->impl_->pipeline_layouts.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_push_constants)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        dev->second.cmd_push_constants(cb->second.handle, layout->second.handle, stage_flags, offset, size, data);
        return VK_SUCCESS;
    }
}
