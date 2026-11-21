// Exercises the guest Vulkan shim (vulkan-shim.dll) through the genuine Vulkan loading dance --
// LoadLibrary + vkGetInstanceProcAddr -- exactly as a real application or the Khronos loader would.
// The shim forwards each call across the Sogen GPU bridge to the host driver, so this enumerates
// the host's physical devices end-to-end: guest app -> shim -> bridge -> host Vulkan -> GPU.

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

namespace
{
    // Records an empty primary command buffer, submits it with a fence, and waits on the fence
    // through the shim's poll-and-yield vkWaitForFences -- exercising the full submission + sync path
    // against the host GPU.
    void submit_and_wait(PFN_vkGetInstanceProcAddr get_instance_proc, VkInstance instance, VkDevice device, VkQueue queue,
                         uint32_t queue_family)
    {
        const auto create_command_pool = reinterpret_cast<PFN_vkCreateCommandPool>(get_instance_proc(instance, "vkCreateCommandPool"));
        const auto destroy_command_pool = reinterpret_cast<PFN_vkDestroyCommandPool>(get_instance_proc(instance, "vkDestroyCommandPool"));
        const auto allocate_command_buffers =
            reinterpret_cast<PFN_vkAllocateCommandBuffers>(get_instance_proc(instance, "vkAllocateCommandBuffers"));
        const auto begin_command_buffer = reinterpret_cast<PFN_vkBeginCommandBuffer>(get_instance_proc(instance, "vkBeginCommandBuffer"));
        const auto end_command_buffer = reinterpret_cast<PFN_vkEndCommandBuffer>(get_instance_proc(instance, "vkEndCommandBuffer"));
        const auto create_fence = reinterpret_cast<PFN_vkCreateFence>(get_instance_proc(instance, "vkCreateFence"));
        const auto destroy_fence = reinterpret_cast<PFN_vkDestroyFence>(get_instance_proc(instance, "vkDestroyFence"));
        const auto queue_submit = reinterpret_cast<PFN_vkQueueSubmit>(get_instance_proc(instance, "vkQueueSubmit"));
        const auto wait_for_fences = reinterpret_cast<PFN_vkWaitForFences>(get_instance_proc(instance, "vkWaitForFences"));

        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.queueFamilyIndex = queue_family;

        VkCommandPool pool = VK_NULL_HANDLE;
        if (create_command_pool(device, &pool_info, nullptr, &pool) != VK_SUCCESS)
        {
            std::printf("[shim-test] vkCreateCommandPool failed\n");
            return;
        }

        VkCommandBufferAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc_info.commandPool = pool;
        alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc_info.commandBufferCount = 1;

        VkCommandBuffer command_buffer = VK_NULL_HANDLE;
        allocate_command_buffers(device, &alloc_info, &command_buffer);

        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        begin_command_buffer(command_buffer, &begin_info);
        end_command_buffer(command_buffer);

        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence = VK_NULL_HANDLE;
        create_fence(device, &fence_info, nullptr, &fence);

        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command_buffer;

        const VkResult submit_result = queue_submit(queue, 1, &submit, fence);
        // First submission absorbs a software-driver (SwiftShader) JIT cold-start of several seconds;
        // a short timeout would spuriously report VK_TIMEOUT here.
        const VkResult wait_result = wait_for_fences(device, 1, &fence, VK_TRUE, 30000000000ULL /* 30s */);
        std::printf("[shim-test] vkQueueSubmit -> %d, vkWaitForFences -> %d\n", submit_result, wait_result);

        // A zero-batch submission (no command buffers) must still signal the fence; otherwise the wait
        // would spin forever.
        VkFence empty_fence = VK_NULL_HANDLE;
        create_fence(device, &fence_info, nullptr, &empty_fence);
        const VkResult empty_submit = queue_submit(queue, 0, nullptr, empty_fence);
        const VkResult empty_wait = wait_for_fences(device, 1, &empty_fence, VK_TRUE, 30000000000ULL /* 30s */);
        std::printf("[shim-test] empty submit -> %d, fence wait -> %d -> %s\n", empty_submit, empty_wait,
                    (empty_submit == VK_SUCCESS && empty_wait == VK_SUCCESS) ? "PASS" : "FAIL");
        destroy_fence(device, empty_fence, nullptr);

        destroy_fence(device, fence, nullptr);
        destroy_command_pool(device, pool, nullptr);
    }

    // Records both promoted spellings with synchronization2-only stage bits. Ending the command buffer
    // flushes the shim command stream, so this catches any truncation or legacy-stage substitution.
    bool test_write_timestamp2(PFN_vkGetInstanceProcAddr get_instance_proc, VkInstance instance, VkDevice device, uint32_t queue_family,
                               PFN_vkCmdWriteTimestamp2 write_timestamp2, PFN_vkCmdWriteTimestamp2KHR write_timestamp2_khr)
    {
        const auto create_command_pool = reinterpret_cast<PFN_vkCreateCommandPool>(get_instance_proc(instance, "vkCreateCommandPool"));
        const auto destroy_command_pool = reinterpret_cast<PFN_vkDestroyCommandPool>(get_instance_proc(instance, "vkDestroyCommandPool"));
        const auto allocate_command_buffers =
            reinterpret_cast<PFN_vkAllocateCommandBuffers>(get_instance_proc(instance, "vkAllocateCommandBuffers"));
        const auto begin_command_buffer = reinterpret_cast<PFN_vkBeginCommandBuffer>(get_instance_proc(instance, "vkBeginCommandBuffer"));
        const auto end_command_buffer = reinterpret_cast<PFN_vkEndCommandBuffer>(get_instance_proc(instance, "vkEndCommandBuffer"));
        const auto create_query_pool = reinterpret_cast<PFN_vkCreateQueryPool>(get_instance_proc(instance, "vkCreateQueryPool"));
        const auto destroy_query_pool = reinterpret_cast<PFN_vkDestroyQueryPool>(get_instance_proc(instance, "vkDestroyQueryPool"));
        if (!create_command_pool || !destroy_command_pool || !allocate_command_buffers || !begin_command_buffer || !end_command_buffer ||
            !create_query_pool || !destroy_query_pool)
        {
            std::printf("[shim-test] timestamp2 support entry point missing\n");
            return false;
        }

        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.queueFamilyIndex = queue_family;

        VkCommandPool pool = VK_NULL_HANDLE;
        if (create_command_pool(device, &pool_info, nullptr, &pool) != VK_SUCCESS)
        {
            std::printf("[shim-test] timestamp2 vkCreateCommandPool failed\n");
            return false;
        }

        VkCommandBufferAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc_info.commandPool = pool;
        alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc_info.commandBufferCount = 1;

        VkCommandBuffer command_buffer = VK_NULL_HANDLE;
        const VkResult allocate_result = allocate_command_buffers(device, &alloc_info, &command_buffer);

        VkQueryPoolCreateInfo query_info{};
        query_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        query_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        query_info.queryCount = 2;

        VkQueryPool query_pool = VK_NULL_HANDLE;
        const VkResult query_result = create_query_pool(device, &query_info, nullptr, &query_pool);

        VkResult begin_result = VK_ERROR_INITIALIZATION_FAILED;
        VkResult end_result = VK_ERROR_INITIALIZATION_FAILED;
        if (allocate_result == VK_SUCCESS && query_result == VK_SUCCESS)
        {
            VkCommandBufferBeginInfo begin_info{};
            begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            begin_result = begin_command_buffer(command_buffer, &begin_info);
            if (begin_result == VK_SUCCESS)
            {
                write_timestamp2(command_buffer, VK_PIPELINE_STAGE_2_COPY_BIT, query_pool, 0);
                write_timestamp2_khr(command_buffer, VK_PIPELINE_STAGE_2_RESOLVE_BIT, query_pool, 1);
                end_result = end_command_buffer(command_buffer);
            }
        }

        const bool ok =
            allocate_result == VK_SUCCESS && query_result == VK_SUCCESS && begin_result == VK_SUCCESS && end_result == VK_SUCCESS;
        std::printf("[shim-test] timestamp2 stage2-only recording -> %s\n", ok ? "PASS" : "FAIL");

        if (query_pool != VK_NULL_HANDLE)
        {
            destroy_query_pool(device, query_pool, nullptr);
        }
        destroy_command_pool(device, pool, nullptr);
        return ok;
    }

    bool test_calibrated_timestamps(PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR get_time_domains_khr,
                                    PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT get_time_domains_ext,
                                    PFN_vkGetCalibratedTimestampsKHR get_timestamps_khr,
                                    PFN_vkGetCalibratedTimestampsEXT get_timestamps_ext, VkPhysicalDevice physical_device, VkDevice device)
    {
        uint32_t khr_count = 0;
        uint32_t ext_count = 0;
        const VkResult khr_count_result = get_time_domains_khr(physical_device, &khr_count, nullptr);
        const VkResult ext_count_result = get_time_domains_ext(physical_device, &ext_count, nullptr);
        if (khr_count_result != VK_SUCCESS || ext_count_result != VK_SUCCESS || khr_count == 0 || ext_count == 0)
        {
            std::printf("[shim-test] calibrated timestamp domains KHR=%d/%u EXT=%d/%u -> FAIL\n", khr_count_result, khr_count,
                        ext_count_result, ext_count);
            return false;
        }

        std::vector<VkTimeDomainKHR> khr_domains(khr_count);
        std::vector<VkTimeDomainKHR> ext_domains(ext_count);
        const VkResult khr_domains_result = get_time_domains_khr(physical_device, &khr_count, khr_domains.data());
        const VkResult ext_domains_result = get_time_domains_ext(physical_device, &ext_count, ext_domains.data());

        VkCalibratedTimestampInfoKHR timestamp_info{};
        timestamp_info.sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR;
        timestamp_info.timeDomain = khr_domains.front();
        uint64_t khr_timestamp = 0;
        uint64_t ext_timestamp = 0;
        uint64_t khr_deviation = 0;
        uint64_t ext_deviation = 0;
        const VkResult khr_result = get_timestamps_khr(device, 1, &timestamp_info, &khr_timestamp, &khr_deviation);
        const VkResult ext_result = get_timestamps_ext(device, 1, &timestamp_info, &ext_timestamp, &ext_deviation);
        const bool ok =
            khr_domains_result == VK_SUCCESS && ext_domains_result == VK_SUCCESS && khr_result == VK_SUCCESS && ext_result == VK_SUCCESS;
        std::printf("[shim-test] calibrated timestamps KHR=%d/%llu EXT=%d/%llu -> %s\n", khr_result,
                    static_cast<unsigned long long>(khr_deviation), ext_result, static_cast<unsigned long long>(ext_deviation),
                    ok ? "PASS" : "FAIL");
        return ok;
    }

    // Picks the first memory type that is set in `type_bits` and carries all of `required` property
    // flags. Returns UINT32_MAX if none qualifies.
    uint32_t find_memory_type(const VkPhysicalDeviceMemoryProperties& props, uint32_t type_bits, VkMemoryPropertyFlags required)
    {
        for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
        {
            if ((type_bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & required) == required)
            {
                return i;
            }
        }
        return UINT32_MAX;
    }

    bool test_transform_feedback(PFN_vkGetInstanceProcAddr get_instance_proc, VkInstance instance, VkPhysicalDevice physical_device,
                                 VkDevice device, uint32_t queue_family)
    {
        const auto get_memory_properties =
            reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(get_instance_proc(instance, "vkGetPhysicalDeviceMemoryProperties"));
        const auto create_buffer = reinterpret_cast<PFN_vkCreateBuffer>(get_instance_proc(instance, "vkCreateBuffer"));
        const auto destroy_buffer = reinterpret_cast<PFN_vkDestroyBuffer>(get_instance_proc(instance, "vkDestroyBuffer"));
        const auto get_buffer_requirements =
            reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(get_instance_proc(instance, "vkGetBufferMemoryRequirements"));
        const auto allocate_memory = reinterpret_cast<PFN_vkAllocateMemory>(get_instance_proc(instance, "vkAllocateMemory"));
        const auto free_memory = reinterpret_cast<PFN_vkFreeMemory>(get_instance_proc(instance, "vkFreeMemory"));
        const auto bind_buffer_memory = reinterpret_cast<PFN_vkBindBufferMemory>(get_instance_proc(instance, "vkBindBufferMemory"));
        const auto create_command_pool = reinterpret_cast<PFN_vkCreateCommandPool>(get_instance_proc(instance, "vkCreateCommandPool"));
        const auto destroy_command_pool = reinterpret_cast<PFN_vkDestroyCommandPool>(get_instance_proc(instance, "vkDestroyCommandPool"));
        const auto allocate_command_buffers =
            reinterpret_cast<PFN_vkAllocateCommandBuffers>(get_instance_proc(instance, "vkAllocateCommandBuffers"));
        const auto begin_command_buffer = reinterpret_cast<PFN_vkBeginCommandBuffer>(get_instance_proc(instance, "vkBeginCommandBuffer"));
        const auto end_command_buffer = reinterpret_cast<PFN_vkEndCommandBuffer>(get_instance_proc(instance, "vkEndCommandBuffer"));
        const auto bind_transform_feedback =
            reinterpret_cast<PFN_vkCmdBindTransformFeedbackBuffersEXT>(get_instance_proc(instance, "vkCmdBindTransformFeedbackBuffersEXT"));
        const auto begin_transform_feedback =
            reinterpret_cast<PFN_vkCmdBeginTransformFeedbackEXT>(get_instance_proc(instance, "vkCmdBeginTransformFeedbackEXT"));
        const auto end_transform_feedback =
            reinterpret_cast<PFN_vkCmdEndTransformFeedbackEXT>(get_instance_proc(instance, "vkCmdEndTransformFeedbackEXT"));
        if (!get_memory_properties || !create_buffer || !destroy_buffer || !get_buffer_requirements || !allocate_memory || !free_memory ||
            !bind_buffer_memory || !create_command_pool || !destroy_command_pool || !allocate_command_buffers || !begin_command_buffer ||
            !end_command_buffer || !bind_transform_feedback || !begin_transform_feedback || !end_transform_feedback)
        {
            std::printf("[shim-test] transform feedback entry point missing\n");
            return false;
        }

        constexpr VkDeviceSize buffer_size = 256;
        VkBufferCreateInfo buffer_info{};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = buffer_size;
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT | VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_COUNTER_BUFFER_BIT_EXT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VkBuffer buffer = VK_NULL_HANDLE;
        const VkResult create_result = create_buffer(device, &buffer_info, nullptr, &buffer);
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkResult memory_result = VK_ERROR_INITIALIZATION_FAILED;
        VkResult bind_result = VK_ERROR_INITIALIZATION_FAILED;
        if (create_result == VK_SUCCESS)
        {
            VkMemoryRequirements requirements{};
            get_buffer_requirements(device, buffer, &requirements);
            VkPhysicalDeviceMemoryProperties memory_properties{};
            get_memory_properties(physical_device, &memory_properties);
            const uint32_t memory_type = find_memory_type(memory_properties, requirements.memoryTypeBits, 0);
            if (memory_type != UINT32_MAX)
            {
                VkMemoryAllocateInfo allocate_info{};
                allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
                allocate_info.allocationSize = requirements.size;
                allocate_info.memoryTypeIndex = memory_type;
                memory_result = allocate_memory(device, &allocate_info, nullptr, &memory);
                if (memory_result == VK_SUCCESS)
                {
                    bind_result = bind_buffer_memory(device, buffer, memory, 0);
                }
            }
        }

        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer command_buffer = VK_NULL_HANDLE;
        VkResult pool_result = VK_ERROR_INITIALIZATION_FAILED;
        VkResult allocate_result = VK_ERROR_INITIALIZATION_FAILED;
        VkResult begin_result = VK_ERROR_INITIALIZATION_FAILED;
        VkResult end_result = VK_ERROR_INITIALIZATION_FAILED;
        if (bind_result == VK_SUCCESS)
        {
            VkCommandPoolCreateInfo pool_info{};
            pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            pool_info.queueFamilyIndex = queue_family;
            pool_result = create_command_pool(device, &pool_info, nullptr, &pool);
            if (pool_result == VK_SUCCESS)
            {
                VkCommandBufferAllocateInfo allocate_info{};
                allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
                allocate_info.commandPool = pool;
                allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                allocate_info.commandBufferCount = 1;
                allocate_result = allocate_command_buffers(device, &allocate_info, &command_buffer);
                if (allocate_result == VK_SUCCESS)
                {
                    VkCommandBufferBeginInfo begin_info{};
                    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                    begin_result = begin_command_buffer(command_buffer, &begin_info);
                    if (begin_result == VK_SUCCESS)
                    {
                        constexpr VkDeviceSize offset = 0;
                        bind_transform_feedback(command_buffer, 0, 1, &buffer, &offset, &buffer_size);
                        begin_transform_feedback(command_buffer, 0, 1, &buffer, &offset);
                        end_transform_feedback(command_buffer, 0, 1, &buffer, &offset);
                        end_result = end_command_buffer(command_buffer);
                    }
                }
            }
        }

        const bool ok = create_result == VK_SUCCESS && memory_result == VK_SUCCESS && bind_result == VK_SUCCESS &&
                        pool_result == VK_SUCCESS && allocate_result == VK_SUCCESS && begin_result == VK_SUCCESS &&
                        end_result == VK_SUCCESS;
        std::printf("[shim-test] transform feedback command recording -> %s\n", ok ? "PASS" : "FAIL");
        if (pool != VK_NULL_HANDLE)
        {
            destroy_command_pool(device, pool, nullptr);
        }
        if (buffer != VK_NULL_HANDLE)
        {
            destroy_buffer(device, buffer, nullptr);
        }
        if (memory != VK_NULL_HANDLE)
        {
            free_memory(device, memory, nullptr);
        }
        return ok;
    }

    // Runs `work` on one thread per worker and waits for every thread it started, so the workers' state may be released
    // afterwards even when a thread could not be created.
    template <typename Workers>
    bool run_on_threads(Workers& workers, const LPTHREAD_START_ROUTINE work)
    {
        std::vector<HANDLE> threads{};
        bool started_all = true;
        for (auto& worker : workers)
        {
            const auto thread = CreateThread(nullptr, 0, work, &worker, 0, nullptr);
            if (!thread)
            {
                started_all = false;
                break;
            }
            threads.push_back(thread);
        }

        for (const auto thread : threads)
        {
            WaitForSingleObject(thread, INFINITE);
            CloseHandle(thread);
        }

        return started_all;
    }

    // vkFlushMappedMemoryRanges and vkInvalidateMappedMemoryRanges need no external synchronization on the memory object,
    // so two threads may sync disjoint, atom-aligned ranges of one mapping at once. Each thread writes its own range,
    // flushes it, invalidates it and checks that it still holds its own data: syncing a range must not move the bytes of
    // the other one.
    bool test_concurrent_mapped_ranges(PFN_vkGetInstanceProcAddr get_instance_proc, VkInstance instance, VkPhysicalDevice physical_device,
                                       VkDevice device)
    {
        const auto get_properties =
            reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(get_instance_proc(instance, "vkGetPhysicalDeviceProperties"));
        const auto get_memory_properties =
            reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(get_instance_proc(instance, "vkGetPhysicalDeviceMemoryProperties"));
        const auto allocate_memory = reinterpret_cast<PFN_vkAllocateMemory>(get_instance_proc(instance, "vkAllocateMemory"));
        const auto free_memory = reinterpret_cast<PFN_vkFreeMemory>(get_instance_proc(instance, "vkFreeMemory"));
        const auto map_memory = reinterpret_cast<PFN_vkMapMemory>(get_instance_proc(instance, "vkMapMemory"));
        const auto unmap_memory = reinterpret_cast<PFN_vkUnmapMemory>(get_instance_proc(instance, "vkUnmapMemory"));
        const auto flush_ranges = reinterpret_cast<PFN_vkFlushMappedMemoryRanges>(get_instance_proc(instance, "vkFlushMappedMemoryRanges"));
        const auto invalidate_ranges =
            reinterpret_cast<PFN_vkInvalidateMappedMemoryRanges>(get_instance_proc(instance, "vkInvalidateMappedMemoryRanges"));

        VkPhysicalDeviceProperties properties{};
        get_properties(physical_device, &properties);
        VkPhysicalDeviceMemoryProperties memory_properties{};
        get_memory_properties(physical_device, &memory_properties);

        uint32_t memory_type = UINT32_MAX;
        for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i)
        {
            const auto flags = memory_properties.memoryTypes[i].propertyFlags;
            if ((flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0 &&
                (memory_type == UINT32_MAX || (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0))
            {
                memory_type = i;
            }
        }

        if (memory_type == UINT32_MAX)
        {
            std::printf("[shim-test] concurrent mapped ranges: no host-visible memory type\n");
            return false;
        }

        const auto memory_flags = memory_properties.memoryTypes[memory_type].propertyFlags;
        const auto atom = (std::max)(properties.limits.nonCoherentAtomSize, VkDeviceSize{256});
        const auto range_size = atom * 16;

        VkMemoryAllocateInfo allocate_info{};
        allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate_info.allocationSize = range_size * 2;
        allocate_info.memoryTypeIndex = memory_type;

        VkDeviceMemory memory = VK_NULL_HANDLE;
        if (allocate_memory(device, &allocate_info, nullptr, &memory) != VK_SUCCESS)
        {
            std::printf("[shim-test] concurrent mapped ranges: vkAllocateMemory failed\n");
            return false;
        }

        void* mapped = nullptr;
        if (map_memory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS || !mapped)
        {
            free_memory(device, memory, nullptr);
            std::printf("[shim-test] concurrent mapped ranges: vkMapMemory failed\n");
            return false;
        }

        struct range_worker
        {
            VkDevice device{};
            VkDeviceMemory memory{};
            PFN_vkFlushMappedMemoryRanges flush{};
            PFN_vkInvalidateMappedMemoryRanges invalidate{};
            uint8_t* bytes{};
            VkDeviceSize offset{};
            VkDeviceSize size{};
            uint8_t tag{};
            uint32_t mismatches{};
            uint32_t failed_calls{};
        };

        constexpr uint32_t iterations = 200;
        const auto run = [](LPVOID param) -> DWORD {
            auto& worker = *static_cast<range_worker*>(param);
            VkMappedMemoryRange range{};
            range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
            range.memory = worker.memory;
            range.offset = worker.offset;
            range.size = worker.size;

            for (uint32_t i = 0; i < iterations; ++i)
            {
                const auto value = static_cast<uint8_t>(worker.tag + i);
                std::memset(worker.bytes + worker.offset, value, static_cast<size_t>(worker.size));
                if (worker.flush(worker.device, 1, &range) != VK_SUCCESS || worker.invalidate(worker.device, 1, &range) != VK_SUCCESS)
                {
                    ++worker.failed_calls;
                    continue;
                }
                for (VkDeviceSize b = 0; b < worker.size; ++b)
                {
                    if (worker.bytes[worker.offset + b] != value)
                    {
                        ++worker.mismatches;
                        break;
                    }
                }
            }
            return 0;
        };

        std::array<range_worker, 2> workers{};
        for (size_t i = 0; i < workers.size(); ++i)
        {
            workers[i] = range_worker{.device = device,
                                      .memory = memory,
                                      .flush = flush_ranges,
                                      .invalidate = invalidate_ranges,
                                      .bytes = static_cast<uint8_t*>(mapped),
                                      .offset = range_size * i,
                                      .size = range_size,
                                      .tag = static_cast<uint8_t>(0x40 * (i + 1))};
        }

        const auto started = run_on_threads(workers, run);

        unmap_memory(device, memory);
        free_memory(device, memory, nullptr);

        const auto mismatches = workers[0].mismatches + workers[1].mismatches;
        const auto failed_calls = workers[0].failed_calls + workers[1].failed_calls;
        const auto ok = started && mismatches == 0 && failed_calls == 0;
        std::printf("[shim-test] concurrent mapped ranges: memory type %u flags 0x%x atom %llu, %u/%u iterations lost data, %u failed "
                    "syncs -> %s\n",
                    memory_type, memory_flags, static_cast<unsigned long long>(properties.limits.nonCoherentAtomSize), mismatches,
                    iterations * 2, failed_calls, ok ? "PASS" : "FAIL");
        return ok;
    }

    // Command buffers from different pools may be recorded on different threads at the same time. Each thread keeps many
    // command buffers in recording at once, each filling its own chunk of the thread's region with a value of its own; the
    // GPU then runs every thread's last recordings, and each chunk must hold the value its command buffer recorded last.
    bool test_parallel_command_recording(PFN_vkGetInstanceProcAddr get_instance_proc, VkInstance instance, VkPhysicalDevice physical_device,
                                         VkDevice device, VkQueue queue, uint32_t queue_family)
    {
        const auto get_memory_properties =
            reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(get_instance_proc(instance, "vkGetPhysicalDeviceMemoryProperties"));
        const auto create_buffer = reinterpret_cast<PFN_vkCreateBuffer>(get_instance_proc(instance, "vkCreateBuffer"));
        const auto destroy_buffer = reinterpret_cast<PFN_vkDestroyBuffer>(get_instance_proc(instance, "vkDestroyBuffer"));
        const auto get_buffer_reqs =
            reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(get_instance_proc(instance, "vkGetBufferMemoryRequirements"));
        const auto allocate_memory = reinterpret_cast<PFN_vkAllocateMemory>(get_instance_proc(instance, "vkAllocateMemory"));
        const auto free_memory = reinterpret_cast<PFN_vkFreeMemory>(get_instance_proc(instance, "vkFreeMemory"));
        const auto bind_buffer_memory = reinterpret_cast<PFN_vkBindBufferMemory>(get_instance_proc(instance, "vkBindBufferMemory"));
        const auto map_memory = reinterpret_cast<PFN_vkMapMemory>(get_instance_proc(instance, "vkMapMemory"));
        const auto unmap_memory = reinterpret_cast<PFN_vkUnmapMemory>(get_instance_proc(instance, "vkUnmapMemory"));
        const auto create_command_pool = reinterpret_cast<PFN_vkCreateCommandPool>(get_instance_proc(instance, "vkCreateCommandPool"));
        const auto destroy_command_pool = reinterpret_cast<PFN_vkDestroyCommandPool>(get_instance_proc(instance, "vkDestroyCommandPool"));
        const auto allocate_command_buffers =
            reinterpret_cast<PFN_vkAllocateCommandBuffers>(get_instance_proc(instance, "vkAllocateCommandBuffers"));
        const auto create_fence = reinterpret_cast<PFN_vkCreateFence>(get_instance_proc(instance, "vkCreateFence"));
        const auto destroy_fence = reinterpret_cast<PFN_vkDestroyFence>(get_instance_proc(instance, "vkDestroyFence"));
        const auto queue_submit = reinterpret_cast<PFN_vkQueueSubmit>(get_instance_proc(instance, "vkQueueSubmit"));
        const auto wait_for_fences = reinterpret_cast<PFN_vkWaitForFences>(get_instance_proc(instance, "vkWaitForFences"));
        const auto device_wait_idle = reinterpret_cast<PFN_vkDeviceWaitIdle>(get_instance_proc(instance, "vkDeviceWaitIdle"));

        struct recorder
        {
            PFN_vkBeginCommandBuffer begin{};
            PFN_vkEndCommandBuffer end{};
            PFN_vkCmdFillBuffer fill{};
            PFN_vkCmdPipelineBarrier barrier{};
            VkCommandPool pool{};
            std::array<VkCommandBuffer, 32> command_buffers{};
            VkBuffer buffer{};
            VkDeviceSize offset{};
            uint32_t tag{};
            uint32_t last_round{};
            uint32_t failed_calls{};
        };

        constexpr size_t thread_count = 4;
        constexpr uint32_t rounds = 40;
        constexpr size_t buffers_per_thread = std::tuple_size_v<decltype(recorder::command_buffers)>;
        constexpr VkDeviceSize chunk_size = 64;
        constexpr VkDeviceSize region_size = chunk_size * buffers_per_thread;

        VkBufferCreateInfo buffer_info{};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = region_size * thread_count;
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VkBuffer buffer = VK_NULL_HANDLE;
        if (create_buffer(device, &buffer_info, nullptr, &buffer) != VK_SUCCESS)
        {
            std::printf("[shim-test] parallel recording: vkCreateBuffer failed\n");
            return false;
        }

        VkMemoryRequirements reqs{};
        get_buffer_reqs(device, buffer, &reqs);
        VkPhysicalDeviceMemoryProperties memory_properties{};
        get_memory_properties(physical_device, &memory_properties);
        const auto type_index = find_memory_type(memory_properties, reqs.memoryTypeBits,
                                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        VkMemoryAllocateInfo allocate_info{};
        allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate_info.allocationSize = reqs.size;
        allocate_info.memoryTypeIndex = type_index;

        VkDeviceMemory memory = VK_NULL_HANDLE;
        if (type_index == UINT32_MAX || allocate_memory(device, &allocate_info, nullptr, &memory) != VK_SUCCESS ||
            bind_buffer_memory(device, buffer, memory, 0) != VK_SUCCESS)
        {
            std::printf("[shim-test] parallel recording: buffer memory setup failed\n");
            if (memory)
            {
                free_memory(device, memory, nullptr);
            }
            destroy_buffer(device, buffer, nullptr);
            return false;
        }

        std::array<recorder, thread_count> recorders{};
        bool setup_ok = true;
        for (size_t i = 0; i < recorders.size(); ++i)
        {
            auto& r = recorders[i];
            r.begin = reinterpret_cast<PFN_vkBeginCommandBuffer>(get_instance_proc(instance, "vkBeginCommandBuffer"));
            r.end = reinterpret_cast<PFN_vkEndCommandBuffer>(get_instance_proc(instance, "vkEndCommandBuffer"));
            r.fill = reinterpret_cast<PFN_vkCmdFillBuffer>(get_instance_proc(instance, "vkCmdFillBuffer"));
            r.barrier = reinterpret_cast<PFN_vkCmdPipelineBarrier>(get_instance_proc(instance, "vkCmdPipelineBarrier"));
            r.buffer = buffer;
            r.offset = region_size * i;
            r.tag = static_cast<uint32_t>(i + 1) << 24;

            VkCommandPoolCreateInfo pool_info{};
            pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            pool_info.queueFamilyIndex = queue_family;

            VkCommandBufferAllocateInfo command_buffer_info{};
            command_buffer_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            command_buffer_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            command_buffer_info.commandBufferCount = static_cast<uint32_t>(buffers_per_thread);

            if (create_command_pool(device, &pool_info, nullptr, &r.pool) != VK_SUCCESS)
            {
                setup_ok = false;
                break;
            }
            command_buffer_info.commandPool = r.pool;
            if (allocate_command_buffers(device, &command_buffer_info, r.command_buffers.data()) != VK_SUCCESS)
            {
                setup_ok = false;
                break;
            }
        }

        const auto record = [](LPVOID param) -> DWORD {
            auto& r = *static_cast<recorder*>(param);
            VkCommandBufferBeginInfo begin_info{};
            begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

            // The host reads the fills back after the fence: transfer writes must be made available to host reads.
            VkMemoryBarrier to_host{};
            to_host.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            to_host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;

            for (uint32_t round = 0; round < rounds; ++round)
            {
                std::array<bool, std::tuple_size_v<decltype(recorder::command_buffers)>> recording{};
                for (size_t b = 0; b < r.command_buffers.size(); ++b)
                {
                    if (r.begin(r.command_buffers[b], &begin_info) != VK_SUCCESS)
                    {
                        ++r.failed_calls;
                        continue;
                    }
                    recording[b] = true;
                    r.fill(r.command_buffers[b], r.buffer, r.offset + chunk_size * b, chunk_size,
                           r.tag | (round << 8) | static_cast<uint32_t>(b));
                    r.barrier(r.command_buffers[b], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &to_host, 0, nullptr,
                              0, nullptr);
                }
                for (size_t b = 0; b < r.command_buffers.size(); ++b)
                {
                    if (recording[b] && r.end(r.command_buffers[b]) != VK_SUCCESS)
                    {
                        ++r.failed_calls;
                    }
                }
                r.last_round = round;
            }
            return 0;
        };

        const auto started = setup_ok && run_on_threads(recorders, record);

        uint32_t failed_calls = 0;
        std::vector<VkCommandBuffer> command_buffers{};
        for (const auto& r : recorders)
        {
            failed_calls += r.failed_calls;
            command_buffers.insert(command_buffers.end(), r.command_buffers.begin(), r.command_buffers.end());
        }

        bool ok = started && failed_calls == 0;
        VkFence fence = VK_NULL_HANDLE;
        if (ok)
        {
            VkFenceCreateInfo fence_info{};
            fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = static_cast<uint32_t>(command_buffers.size());
            submit.pCommandBuffers = command_buffers.data();
            ok = create_fence(device, &fence_info, nullptr, &fence) == VK_SUCCESS && queue_submit(queue, 1, &submit, fence) == VK_SUCCESS;
            if (ok && wait_for_fences(device, 1, &fence, VK_TRUE, 30000000000ULL) != VK_SUCCESS)
            {
                // Only completion or device loss leave nothing pending; on any other failure the work may still be running,
                // so the probe ends without destroying what it uses.
                const auto idle = device_wait_idle(device);
                if (idle != VK_SUCCESS && idle != VK_ERROR_DEVICE_LOST)
                {
                    std::printf("[shim-test] parallel recording: vkDeviceWaitIdle -> %d with work pending -> FAIL\n", idle);
                    std::fflush(stdout);
                    ExitProcess(1);
                }
                ok = false;
            }
        }

        uint32_t wrong_chunks = 0;
        void* mapped = nullptr;
        if (ok && map_memory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS && mapped)
        {
            for (const auto& r : recorders)
            {
                for (size_t b = 0; b < r.command_buffers.size(); ++b)
                {
                    const auto expected = r.tag | (r.last_round << 8) | static_cast<uint32_t>(b);
                    const auto* words = static_cast<const uint32_t*>(mapped) + (r.offset + chunk_size * b) / sizeof(uint32_t);
                    for (VkDeviceSize w = 0; w < chunk_size / sizeof(uint32_t); ++w)
                    {
                        if (words[w] != expected)
                        {
                            ++wrong_chunks;
                            break;
                        }
                    }
                }
            }
            unmap_memory(device, memory);
        }
        else
        {
            ok = false;
        }

        if (fence)
        {
            destroy_fence(device, fence, nullptr);
        }
        for (const auto& r : recorders)
        {
            if (r.pool)
            {
                destroy_command_pool(device, r.pool, nullptr);
            }
        }
        free_memory(device, memory, nullptr);
        destroy_buffer(device, buffer, nullptr);

        ok = ok && wrong_chunks == 0;
        std::printf(
            "[shim-test] parallel recording: %zu threads x %zu command buffers x %u rounds, %u failed calls, %u/%zu chunks wrong -> %s\n",
            thread_count, buffers_per_thread, rounds, failed_calls, wrong_chunks, thread_count * buffers_per_thread, ok ? "PASS" : "FAIL");
        return ok;
    }

    bool test_pipeline_cache(PFN_vkGetInstanceProcAddr get_instance_proc, VkInstance instance, VkDevice device)
    {
        const auto get_device_proc = reinterpret_cast<PFN_vkGetDeviceProcAddr>(get_instance_proc(instance, "vkGetDeviceProcAddr"));
        if (!get_device_proc)
        {
            std::printf("[shim-test] no vkGetDeviceProcAddr for pipeline cache test\n");
            return false;
        }

        const auto create_pipeline_cache = reinterpret_cast<PFN_vkCreatePipelineCache>(get_device_proc(device, "vkCreatePipelineCache"));
        const auto destroy_pipeline_cache = reinterpret_cast<PFN_vkDestroyPipelineCache>(get_device_proc(device, "vkDestroyPipelineCache"));
        const auto get_pipeline_cache_data =
            reinterpret_cast<PFN_vkGetPipelineCacheData>(get_device_proc(device, "vkGetPipelineCacheData"));
        const auto merge_pipeline_caches = reinterpret_cast<PFN_vkMergePipelineCaches>(get_device_proc(device, "vkMergePipelineCaches"));
        if (!create_pipeline_cache || !destroy_pipeline_cache || !get_pipeline_cache_data || !merge_pipeline_caches)
        {
            std::printf("[shim-test] pipeline cache entry point missing\n");
            return false;
        }

        VkPipelineCacheCreateInfo create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
        VkPipelineCache cache = VK_NULL_HANDLE;
        const VkResult create_result = create_pipeline_cache(device, &create_info, nullptr, &cache);

        size_t data_size = 0;
        VkResult size_result = VK_ERROR_INITIALIZATION_FAILED;
        VkResult data_result = VK_ERROR_INITIALIZATION_FAILED;
        VkResult seeded_result = VK_ERROR_INITIALIZATION_FAILED;
        VkResult merge_result = VK_ERROR_INITIALIZATION_FAILED;
        VkPipelineCache seeded_cache = VK_NULL_HANDLE;
        if (create_result == VK_SUCCESS)
        {
            size_result = get_pipeline_cache_data(device, cache, &data_size, nullptr);
            if (size_result == VK_SUCCESS)
            {
                std::vector<uint8_t> data(data_size);
                data_result = get_pipeline_cache_data(device, cache, &data_size, data.data());
                data.resize(data_size);
                if (data_result == VK_SUCCESS)
                {
                    VkPipelineCacheCreateInfo seeded_info{};
                    seeded_info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
                    seeded_info.initialDataSize = data.size();
                    seeded_info.pInitialData = data.empty() ? nullptr : data.data();
                    seeded_result = create_pipeline_cache(device, &seeded_info, nullptr, &seeded_cache);
                    if (seeded_result == VK_SUCCESS)
                    {
                        merge_result = merge_pipeline_caches(device, cache, 1, &seeded_cache);
                    }
                }
            }
        }

        const bool ok = create_result == VK_SUCCESS && size_result == VK_SUCCESS && data_result == VK_SUCCESS &&
                        seeded_result == VK_SUCCESS && merge_result == VK_SUCCESS;
        std::printf("[shim-test] pipeline cache round trip -> %s\n", ok ? "PASS" : "FAIL");
        if (seeded_cache != VK_NULL_HANDLE)
        {
            destroy_pipeline_cache(device, seeded_cache, nullptr);
        }
        if (cache != VK_NULL_HANDLE)
        {
            destroy_pipeline_cache(device, cache, nullptr);
        }
        return ok;
    }

    // Allocates a host-visible buffer, has the GPU fill it with a known pattern via vkCmdFillBuffer,
    // then maps it back and verifies the bytes -- the first end-to-end "GPU produces data the guest
    // reads back" path across the bridge.
    bool fill_buffer_and_readback(PFN_vkGetInstanceProcAddr get_instance_proc, VkInstance instance, VkPhysicalDevice physical_device,
                                  VkDevice device, VkQueue queue, uint32_t queue_family)
    {
        const auto get_memory_properties =
            reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(get_instance_proc(instance, "vkGetPhysicalDeviceMemoryProperties"));
        const auto create_buffer = reinterpret_cast<PFN_vkCreateBuffer>(get_instance_proc(instance, "vkCreateBuffer"));
        const auto destroy_buffer = reinterpret_cast<PFN_vkDestroyBuffer>(get_instance_proc(instance, "vkDestroyBuffer"));
        const auto get_buffer_reqs =
            reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(get_instance_proc(instance, "vkGetBufferMemoryRequirements"));
        const auto allocate_memory = reinterpret_cast<PFN_vkAllocateMemory>(get_instance_proc(instance, "vkAllocateMemory"));
        const auto free_memory = reinterpret_cast<PFN_vkFreeMemory>(get_instance_proc(instance, "vkFreeMemory"));
        const auto bind_buffer_memory = reinterpret_cast<PFN_vkBindBufferMemory>(get_instance_proc(instance, "vkBindBufferMemory"));
        const auto map_memory = reinterpret_cast<PFN_vkMapMemory>(get_instance_proc(instance, "vkMapMemory"));
        const auto unmap_memory = reinterpret_cast<PFN_vkUnmapMemory>(get_instance_proc(instance, "vkUnmapMemory"));
        const auto cmd_fill_buffer = reinterpret_cast<PFN_vkCmdFillBuffer>(get_instance_proc(instance, "vkCmdFillBuffer"));

        const auto create_command_pool = reinterpret_cast<PFN_vkCreateCommandPool>(get_instance_proc(instance, "vkCreateCommandPool"));
        const auto destroy_command_pool = reinterpret_cast<PFN_vkDestroyCommandPool>(get_instance_proc(instance, "vkDestroyCommandPool"));
        const auto allocate_command_buffers =
            reinterpret_cast<PFN_vkAllocateCommandBuffers>(get_instance_proc(instance, "vkAllocateCommandBuffers"));
        const auto begin_command_buffer = reinterpret_cast<PFN_vkBeginCommandBuffer>(get_instance_proc(instance, "vkBeginCommandBuffer"));
        const auto end_command_buffer = reinterpret_cast<PFN_vkEndCommandBuffer>(get_instance_proc(instance, "vkEndCommandBuffer"));
        const auto create_fence = reinterpret_cast<PFN_vkCreateFence>(get_instance_proc(instance, "vkCreateFence"));
        const auto destroy_fence = reinterpret_cast<PFN_vkDestroyFence>(get_instance_proc(instance, "vkDestroyFence"));
        const auto queue_submit = reinterpret_cast<PFN_vkQueueSubmit>(get_instance_proc(instance, "vkQueueSubmit"));
        const auto wait_for_fences = reinterpret_cast<PFN_vkWaitForFences>(get_instance_proc(instance, "vkWaitForFences"));

        constexpr VkDeviceSize buffer_size = 256;
        constexpr uint32_t fill_value = 0xDEADBEEFu;

        VkBufferCreateInfo buffer_info{};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = buffer_size;
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VkBuffer buffer = VK_NULL_HANDLE;
        if (create_buffer(device, &buffer_info, nullptr, &buffer) != VK_SUCCESS)
        {
            std::printf("[shim-test] vkCreateBuffer failed\n");
            return false;
        }

        VkMemoryRequirements reqs{};
        get_buffer_reqs(device, buffer, &reqs);

        VkPhysicalDeviceMemoryProperties mem_props{};
        get_memory_properties(physical_device, &mem_props);
        const uint32_t type_index =
            find_memory_type(mem_props, reqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (type_index == UINT32_MAX)
        {
            std::printf("[shim-test] no host-visible memory type\n");
            destroy_buffer(device, buffer, nullptr);
            return false;
        }

        VkMemoryAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = reqs.size;
        alloc.memoryTypeIndex = type_index;

        VkDeviceMemory memory = VK_NULL_HANDLE;
        if (allocate_memory(device, &alloc, nullptr, &memory) != VK_SUCCESS)
        {
            std::printf("[shim-test] vkAllocateMemory failed\n");
            destroy_buffer(device, buffer, nullptr);
            return false;
        }
        bind_buffer_memory(device, buffer, memory, 0);

        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.queueFamilyIndex = queue_family;
        VkCommandPool pool = VK_NULL_HANDLE;
        create_command_pool(device, &pool_info, nullptr, &pool);

        VkCommandBufferAllocateInfo cb_info{};
        cb_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cb_info.commandPool = pool;
        cb_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cb_info.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        allocate_command_buffers(device, &cb_info, &cmd);

        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        begin_command_buffer(cmd, &begin);
        cmd_fill_buffer(cmd, buffer, 0, buffer_size, fill_value);
        end_command_buffer(cmd);

        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence = VK_NULL_HANDLE;
        create_fence(device, &fence_info, nullptr, &fence);

        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        queue_submit(queue, 1, &submit, fence);
        // SwiftShader (software) JIT-compiles its submission path on first use, which can take several
        // seconds of wall-clock; give the fence a generous timeout so cold-start doesn't read as failure.
        const VkResult wait_result = wait_for_fences(device, 1, &fence, VK_TRUE, 30000000000ULL /* 30s */);
        std::printf("[shim-test] fill: vkWaitForFences -> %d\n", wait_result);

        bool ok = (wait_result == VK_SUCCESS);
        if (ok)
        {
            void* mapped = nullptr;
            if (map_memory(device, memory, 0, buffer_size, 0, &mapped) == VK_SUCCESS && mapped)
            {
                const auto* words = static_cast<const uint32_t*>(mapped);
                for (uint32_t i = 0; i < buffer_size / sizeof(uint32_t); ++i)
                {
                    if (words[i] != fill_value)
                    {
                        ok = false;
                        std::printf("[shim-test] readback mismatch at word %u: 0x%08X != 0x%08X\n", i, words[i], fill_value);
                        break;
                    }
                }
                unmap_memory(device, memory);
            }
            else
            {
                ok = false;
                std::printf("[shim-test] vkMapMemory failed\n");
            }
        }

        std::printf("[shim-test] fill+readback (0x%08X x%u) -> %s\n", fill_value, static_cast<uint32_t>(buffer_size / sizeof(uint32_t)),
                    ok ? "PASS" : "FAIL");

        destroy_fence(device, fence, nullptr);
        destroy_command_pool(device, pool, nullptr);
        free_memory(device, memory, nullptr);
        destroy_buffer(device, buffer, nullptr);
        return ok;
    }

    // The calls the shim queues in its command ring instead of making them (descriptor-set allocations and updates,
    // recorded command buffers, command-pool and descriptor-pool resets, buffer-view destroys), round after round, each
    // checked by what the GPU filled in, until the ring has wrapped several times; then command buffers recorded back to
    // back with no other call in between, which fill the ring until the shim has the host run it. Where the host maps no
    // ring, the same calls are made one by one and the results must be the same.
    bool test_command_ring(PFN_vkGetInstanceProcAddr get_instance_proc, VkInstance instance, VkPhysicalDevice physical_device,
                           VkDevice device, VkQueue queue, uint32_t queue_family)
    {
        const auto get_memory_properties =
            reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(get_instance_proc(instance, "vkGetPhysicalDeviceMemoryProperties"));
        const auto create_buffer = reinterpret_cast<PFN_vkCreateBuffer>(get_instance_proc(instance, "vkCreateBuffer"));
        const auto destroy_buffer = reinterpret_cast<PFN_vkDestroyBuffer>(get_instance_proc(instance, "vkDestroyBuffer"));
        const auto get_buffer_reqs =
            reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(get_instance_proc(instance, "vkGetBufferMemoryRequirements"));
        const auto allocate_memory = reinterpret_cast<PFN_vkAllocateMemory>(get_instance_proc(instance, "vkAllocateMemory"));
        const auto free_memory = reinterpret_cast<PFN_vkFreeMemory>(get_instance_proc(instance, "vkFreeMemory"));
        const auto bind_buffer_memory = reinterpret_cast<PFN_vkBindBufferMemory>(get_instance_proc(instance, "vkBindBufferMemory"));
        const auto map_memory = reinterpret_cast<PFN_vkMapMemory>(get_instance_proc(instance, "vkMapMemory"));
        const auto unmap_memory = reinterpret_cast<PFN_vkUnmapMemory>(get_instance_proc(instance, "vkUnmapMemory"));
        const auto create_buffer_view = reinterpret_cast<PFN_vkCreateBufferView>(get_instance_proc(instance, "vkCreateBufferView"));
        const auto destroy_buffer_view = reinterpret_cast<PFN_vkDestroyBufferView>(get_instance_proc(instance, "vkDestroyBufferView"));
        const auto create_set_layout =
            reinterpret_cast<PFN_vkCreateDescriptorSetLayout>(get_instance_proc(instance, "vkCreateDescriptorSetLayout"));
        const auto destroy_set_layout =
            reinterpret_cast<PFN_vkDestroyDescriptorSetLayout>(get_instance_proc(instance, "vkDestroyDescriptorSetLayout"));
        const auto create_descriptor_pool =
            reinterpret_cast<PFN_vkCreateDescriptorPool>(get_instance_proc(instance, "vkCreateDescriptorPool"));
        const auto destroy_descriptor_pool =
            reinterpret_cast<PFN_vkDestroyDescriptorPool>(get_instance_proc(instance, "vkDestroyDescriptorPool"));
        const auto reset_descriptor_pool =
            reinterpret_cast<PFN_vkResetDescriptorPool>(get_instance_proc(instance, "vkResetDescriptorPool"));
        const auto allocate_sets = reinterpret_cast<PFN_vkAllocateDescriptorSets>(get_instance_proc(instance, "vkAllocateDescriptorSets"));
        const auto update_sets = reinterpret_cast<PFN_vkUpdateDescriptorSets>(get_instance_proc(instance, "vkUpdateDescriptorSets"));
        const auto create_command_pool = reinterpret_cast<PFN_vkCreateCommandPool>(get_instance_proc(instance, "vkCreateCommandPool"));
        const auto destroy_command_pool = reinterpret_cast<PFN_vkDestroyCommandPool>(get_instance_proc(instance, "vkDestroyCommandPool"));
        const auto reset_command_pool = reinterpret_cast<PFN_vkResetCommandPool>(get_instance_proc(instance, "vkResetCommandPool"));
        const auto allocate_command_buffers =
            reinterpret_cast<PFN_vkAllocateCommandBuffers>(get_instance_proc(instance, "vkAllocateCommandBuffers"));
        const auto begin_command_buffer = reinterpret_cast<PFN_vkBeginCommandBuffer>(get_instance_proc(instance, "vkBeginCommandBuffer"));
        const auto end_command_buffer = reinterpret_cast<PFN_vkEndCommandBuffer>(get_instance_proc(instance, "vkEndCommandBuffer"));
        const auto cmd_fill_buffer = reinterpret_cast<PFN_vkCmdFillBuffer>(get_instance_proc(instance, "vkCmdFillBuffer"));
        const auto create_fence = reinterpret_cast<PFN_vkCreateFence>(get_instance_proc(instance, "vkCreateFence"));
        const auto destroy_fence = reinterpret_cast<PFN_vkDestroyFence>(get_instance_proc(instance, "vkDestroyFence"));
        const auto reset_fences = reinterpret_cast<PFN_vkResetFences>(get_instance_proc(instance, "vkResetFences"));
        const auto queue_submit = reinterpret_cast<PFN_vkQueueSubmit>(get_instance_proc(instance, "vkQueueSubmit"));
        const auto wait_for_fences = reinterpret_cast<PFN_vkWaitForFences>(get_instance_proc(instance, "vkWaitForFences"));

        // 512 fills a command buffer make a ~25 KB record, so 600 rounds wrap the 4 MiB ring several times and 300
        // recordings in a row overrun it.
        constexpr uint32_t words = 512;
        constexpr VkDeviceSize buffer_size = words * sizeof(uint32_t);
        constexpr uint32_t rounds = 600;
        constexpr uint32_t back_to_back = 300;
        const auto value = [](const uint32_t round, const uint32_t word) { return (round << 16) | word; };

        VkBufferCreateInfo buffer_info{};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = buffer_size;
        buffer_info.usage =
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkBuffer buffer = VK_NULL_HANDLE;
        if (create_buffer(device, &buffer_info, nullptr, &buffer) != VK_SUCCESS)
        {
            std::printf("[shim-test] command ring: vkCreateBuffer failed -> FAIL\n");
            return false;
        }

        VkMemoryRequirements reqs{};
        get_buffer_reqs(device, buffer, &reqs);
        VkPhysicalDeviceMemoryProperties mem_props{};
        get_memory_properties(physical_device, &mem_props);
        VkMemoryAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = reqs.size;
        alloc.memoryTypeIndex =
            find_memory_type(mem_props, reqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkDeviceMemory memory = VK_NULL_HANDLE;
        if (alloc.memoryTypeIndex == UINT32_MAX || allocate_memory(device, &alloc, nullptr, &memory) != VK_SUCCESS)
        {
            std::printf("[shim-test] command ring: no host-visible memory -> FAIL\n");
            destroy_buffer(device, buffer, nullptr);
            return false;
        }
        bind_buffer_memory(device, buffer, memory, 0);

        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout_info.bindingCount = 1;
        layout_info.pBindings = &binding;
        VkDescriptorSetLayout layout = VK_NULL_HANDLE;
        create_set_layout(device, &layout_info, nullptr, &layout);

        // No FREE_DESCRIPTOR_SET: the shim answers these allocations itself and queues them.
        const VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 32};
        VkDescriptorPoolCreateInfo descriptor_pool_info{};
        descriptor_pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        descriptor_pool_info.maxSets = 32;
        descriptor_pool_info.poolSizeCount = 1;
        descriptor_pool_info.pPoolSizes = &pool_size;
        VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
        create_descriptor_pool(device, &descriptor_pool_info, nullptr, &descriptor_pool);

        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = queue_family;
        VkCommandPool pool = VK_NULL_HANDLE;
        create_command_pool(device, &pool_info, nullptr, &pool);
        VkCommandBufferAllocateInfo cb_info{};
        cb_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cb_info.commandPool = pool;
        cb_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cb_info.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        allocate_command_buffers(device, &cb_info, &cmd);

        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence = VK_NULL_HANDLE;
        create_fence(device, &fence_info, nullptr, &fence);

        void* mapped = nullptr;
        const bool mapped_ok = map_memory(device, memory, 0, buffer_size, 0, &mapped) == VK_SUCCESS && mapped;
        const auto* readback = static_cast<const volatile uint32_t*>(mapped);

        const auto record = [&](const uint32_t round) {
            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            begin_command_buffer(cmd, &begin);
            for (uint32_t word = 0; word < words; ++word)
            {
                cmd_fill_buffer(cmd, buffer, word * sizeof(uint32_t), sizeof(uint32_t), value(round, word));
            }
            return end_command_buffer(cmd);
        };

        std::array<char, 160> failure{};
        const auto submit_and_check = [&](const uint32_t round) {
            reset_fences(device, 1, &fence);
            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &cmd;
            const VkResult submitted = queue_submit(queue, 1, &submit, fence);
            const VkResult waited = submitted == VK_SUCCESS ? wait_for_fences(device, 1, &fence, VK_TRUE, 30000000000ULL) : submitted;
            if (submitted != VK_SUCCESS || waited != VK_SUCCESS)
            {
                std::snprintf(failure.data(), failure.size(), "round %u: submit %d, wait %d", round, submitted, waited);
                return false;
            }
            for (uint32_t word = 0; word < words; ++word)
            {
                if (readback[word] != value(round, word))
                {
                    std::snprintf(failure.data(), failure.size(), "round %u: word %u is 0x%08X, not 0x%08X", round, word, readback[word],
                                  value(round, word));
                    return false;
                }
            }
            return true;
        };

        LARGE_INTEGER frequency{};
        LARGE_INTEGER start{};
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&start);

        bool ok = mapped_ok && layout && descriptor_pool && pool && cmd && fence;
        if (!ok)
        {
            std::snprintf(failure.data(), failure.size(), "setup failed");
        }
        for (uint32_t round = 0; ok && round < rounds; ++round)
        {
            reset_command_pool(device, pool, 0);

            VkDescriptorSetAllocateInfo set_info{};
            set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            set_info.descriptorPool = descriptor_pool;
            set_info.descriptorSetCount = 1;
            set_info.pSetLayouts = &layout;
            VkDescriptorSet set = VK_NULL_HANDLE;
            if (const VkResult allocated = allocate_sets(device, &set_info, &set); allocated != VK_SUCCESS)
            {
                std::snprintf(failure.data(), failure.size(), "round %u: vkAllocateDescriptorSets %d", round, allocated);
                ok = false;
                break;
            }
            const VkDescriptorBufferInfo buffer_descriptor{buffer, 0, VK_WHOLE_SIZE};
            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = set;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            write.pBufferInfo = &buffer_descriptor;
            update_sets(device, 1, &write, 0, nullptr);

            VkBufferViewCreateInfo view_info{};
            view_info.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
            view_info.buffer = buffer;
            view_info.format = VK_FORMAT_R32_UINT;
            view_info.range = VK_WHOLE_SIZE;
            VkBufferView view = VK_NULL_HANDLE;
            if (create_buffer_view(device, &view_info, nullptr, &view) == VK_SUCCESS)
            {
                destroy_buffer_view(device, view, nullptr);
            }

            if (const VkResult recorded = record(round); recorded != VK_SUCCESS)
            {
                std::snprintf(failure.data(), failure.size(), "round %u: vkEndCommandBuffer %d", round, recorded);
                ok = false;
                break;
            }
            ok = submit_and_check(round);

            if ((round + 1) % 16 == 0)
            {
                reset_descriptor_pool(device, descriptor_pool, 0);
            }
        }

        // Nothing but recording between these, so the ring fills up and the shim has to have the host run it.
        uint32_t last = rounds;
        for (uint32_t i = 0; ok && i < back_to_back; ++i)
        {
            last = rounds + i;
            if (const VkResult recorded = record(last); recorded != VK_SUCCESS)
            {
                std::snprintf(failure.data(), failure.size(), "recording %u: vkEndCommandBuffer %d", last, recorded);
                ok = false;
            }
        }
        ok = ok && submit_and_check(last);

        LARGE_INTEGER end{};
        QueryPerformanceCounter(&end);
        std::printf("[shim-test] command ring: %u checked rounds + %u recordings in a row, %.1f ms -> %s%s%s\n", rounds, back_to_back,
                    1000.0 * static_cast<double>(end.QuadPart - start.QuadPart) / static_cast<double>(frequency.QuadPart),
                    ok ? "PASS" : "FAIL", ok ? "" : ": ", ok ? "" : failure.data());

        if (mapped_ok)
        {
            unmap_memory(device, memory);
        }
        destroy_fence(device, fence, nullptr);
        destroy_command_pool(device, pool, nullptr);
        destroy_descriptor_pool(device, descriptor_pool, nullptr);
        destroy_set_layout(device, layout, nullptr);
        free_memory(device, memory, nullptr);
        destroy_buffer(device, buffer, nullptr);
        return ok;
    }

    // Renders into an image the simplest possible way -- clears it to a known color on the GPU -- then
    // copies it into a host-visible buffer and reads the pixels back. This is the offscreen render
    // target + readback path that windowed present will reuse (present = clear/draw -> copy -> readback).
    bool clear_image_and_readback(PFN_vkGetInstanceProcAddr get_instance_proc, VkInstance instance, VkPhysicalDevice physical_device,
                                  VkDevice device, VkQueue queue, uint32_t queue_family)
    {
        const auto get_memory_properties =
            reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(get_instance_proc(instance, "vkGetPhysicalDeviceMemoryProperties"));
        const auto create_image = reinterpret_cast<PFN_vkCreateImage>(get_instance_proc(instance, "vkCreateImage"));
        const auto destroy_image = reinterpret_cast<PFN_vkDestroyImage>(get_instance_proc(instance, "vkDestroyImage"));
        const auto get_image_reqs =
            reinterpret_cast<PFN_vkGetImageMemoryRequirements>(get_instance_proc(instance, "vkGetImageMemoryRequirements"));
        const auto bind_image_memory = reinterpret_cast<PFN_vkBindImageMemory>(get_instance_proc(instance, "vkBindImageMemory"));
        const auto create_buffer = reinterpret_cast<PFN_vkCreateBuffer>(get_instance_proc(instance, "vkCreateBuffer"));
        const auto destroy_buffer = reinterpret_cast<PFN_vkDestroyBuffer>(get_instance_proc(instance, "vkDestroyBuffer"));
        const auto get_buffer_reqs =
            reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(get_instance_proc(instance, "vkGetBufferMemoryRequirements"));
        const auto allocate_memory = reinterpret_cast<PFN_vkAllocateMemory>(get_instance_proc(instance, "vkAllocateMemory"));
        const auto free_memory = reinterpret_cast<PFN_vkFreeMemory>(get_instance_proc(instance, "vkFreeMemory"));
        const auto bind_buffer_memory = reinterpret_cast<PFN_vkBindBufferMemory>(get_instance_proc(instance, "vkBindBufferMemory"));
        const auto map_memory = reinterpret_cast<PFN_vkMapMemory>(get_instance_proc(instance, "vkMapMemory"));
        const auto unmap_memory = reinterpret_cast<PFN_vkUnmapMemory>(get_instance_proc(instance, "vkUnmapMemory"));
        const auto cmd_pipeline_barrier = reinterpret_cast<PFN_vkCmdPipelineBarrier>(get_instance_proc(instance, "vkCmdPipelineBarrier"));
        const auto cmd_clear_color_image = reinterpret_cast<PFN_vkCmdClearColorImage>(get_instance_proc(instance, "vkCmdClearColorImage"));
        const auto cmd_copy_image_to_buffer =
            reinterpret_cast<PFN_vkCmdCopyImageToBuffer>(get_instance_proc(instance, "vkCmdCopyImageToBuffer"));

        const auto create_command_pool = reinterpret_cast<PFN_vkCreateCommandPool>(get_instance_proc(instance, "vkCreateCommandPool"));
        const auto destroy_command_pool = reinterpret_cast<PFN_vkDestroyCommandPool>(get_instance_proc(instance, "vkDestroyCommandPool"));
        const auto allocate_command_buffers =
            reinterpret_cast<PFN_vkAllocateCommandBuffers>(get_instance_proc(instance, "vkAllocateCommandBuffers"));
        const auto begin_command_buffer = reinterpret_cast<PFN_vkBeginCommandBuffer>(get_instance_proc(instance, "vkBeginCommandBuffer"));
        const auto end_command_buffer = reinterpret_cast<PFN_vkEndCommandBuffer>(get_instance_proc(instance, "vkEndCommandBuffer"));
        const auto create_fence = reinterpret_cast<PFN_vkCreateFence>(get_instance_proc(instance, "vkCreateFence"));
        const auto destroy_fence = reinterpret_cast<PFN_vkDestroyFence>(get_instance_proc(instance, "vkDestroyFence"));
        const auto queue_submit = reinterpret_cast<PFN_vkQueueSubmit>(get_instance_proc(instance, "vkQueueSubmit"));
        const auto wait_for_fences = reinterpret_cast<PFN_vkWaitForFences>(get_instance_proc(instance, "vkWaitForFences"));

        constexpr uint32_t width = 16;
        constexpr uint32_t height = 16;
        constexpr VkDeviceSize readback_size = static_cast<VkDeviceSize>(width) * height * 4;
        // Clear color (R,G,B,A) = (1,0,0,1); R8G8B8A8_UNORM little-endian word = 0xFF0000FF.
        constexpr uint32_t expected = 0xFF0000FFu;

        VkPhysicalDeviceMemoryProperties mem_props{};
        get_memory_properties(physical_device, &mem_props);

        // --- offscreen render target image ---
        VkImageCreateInfo image_info{};
        image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        image_info.extent = {.width = width, .height = height, .depth = 1};
        image_info.mipLevels = 1;
        image_info.arrayLayers = 1;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

        VkImage image = VK_NULL_HANDLE;
        if (create_image(device, &image_info, nullptr, &image) != VK_SUCCESS)
        {
            std::printf("[shim-test] vkCreateImage failed\n");
            return false;
        }

        VkMemoryRequirements image_reqs{};
        get_image_reqs(device, image, &image_reqs);
        const uint32_t image_type = find_memory_type(mem_props, image_reqs.memoryTypeBits, 0);

        VkMemoryAllocateInfo image_alloc{};
        image_alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        image_alloc.allocationSize = image_reqs.size;
        image_alloc.memoryTypeIndex = image_type;
        VkDeviceMemory image_memory = VK_NULL_HANDLE;
        allocate_memory(device, &image_alloc, nullptr, &image_memory);
        bind_image_memory(device, image, image_memory, 0);

        // --- host-visible readback buffer ---
        VkBufferCreateInfo buffer_info{};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = readback_size;
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkBuffer buffer = VK_NULL_HANDLE;
        create_buffer(device, &buffer_info, nullptr, &buffer);

        VkMemoryRequirements buffer_reqs{};
        get_buffer_reqs(device, buffer, &buffer_reqs);
        const uint32_t buffer_type = find_memory_type(mem_props, buffer_reqs.memoryTypeBits,
                                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        VkMemoryAllocateInfo buffer_alloc{};
        buffer_alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        buffer_alloc.allocationSize = buffer_reqs.size;
        buffer_alloc.memoryTypeIndex = buffer_type;
        VkDeviceMemory buffer_memory = VK_NULL_HANDLE;
        allocate_memory(device, &buffer_alloc, nullptr, &buffer_memory);
        bind_buffer_memory(device, buffer, buffer_memory, 0);

        if (image_type == UINT32_MAX || buffer_type == UINT32_MAX)
        {
            std::printf("[shim-test] no suitable memory type for image/readback\n");
            destroy_buffer(device, buffer, nullptr);
            free_memory(device, buffer_memory, nullptr);
            destroy_image(device, image, nullptr);
            free_memory(device, image_memory, nullptr);
            return false;
        }

        // --- record: transition -> clear -> transition -> copy to buffer ---
        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.queueFamilyIndex = queue_family;
        VkCommandPool pool = VK_NULL_HANDLE;
        create_command_pool(device, &pool_info, nullptr, &pool);

        VkCommandBufferAllocateInfo cb_info{};
        cb_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cb_info.commandPool = pool;
        cb_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cb_info.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        allocate_command_buffers(device, &cb_info, &cmd);

        VkImageSubresourceRange full_range{};
        full_range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        full_range.baseMipLevel = 0;
        full_range.levelCount = 1;
        full_range.baseArrayLayer = 0;
        full_range.layerCount = 1;

        const auto transition = [&](VkImageLayout old_layout, VkImageLayout new_layout, VkAccessFlags src_access, VkAccessFlags dst_access,
                                    VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.srcAccessMask = src_access;
            barrier.dstAccessMask = dst_access;
            barrier.oldLayout = old_layout;
            barrier.newLayout = new_layout;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image;
            barrier.subresourceRange = full_range;
            cmd_pipeline_barrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        };

        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        begin_command_buffer(cmd, &begin);

        transition(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

        VkClearColorValue clear{};
        clear.float32[0] = 1.0f;
        clear.float32[1] = 0.0f;
        clear.float32[2] = 0.0f;
        clear.float32[3] = 1.0f;
        cmd_clear_color_image(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &full_range);

        transition(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {.width = width, .height = height, .depth = 1};
        cmd_copy_image_to_buffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);

        end_command_buffer(cmd);

        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence = VK_NULL_HANDLE;
        create_fence(device, &fence_info, nullptr, &fence);

        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        queue_submit(queue, 1, &submit, fence);
        const VkResult wait_result = wait_for_fences(device, 1, &fence, VK_TRUE, 30000000000ULL /* 30s */);

        bool ok = (wait_result == VK_SUCCESS);
        if (ok)
        {
            void* mapped = nullptr;
            if (map_memory(device, buffer_memory, 0, readback_size, 0, &mapped) == VK_SUCCESS && mapped)
            {
                const auto* pixels = static_cast<const uint32_t*>(mapped);
                for (uint32_t i = 0; i < width * height; ++i)
                {
                    if (pixels[i] != expected)
                    {
                        ok = false;
                        std::printf("[shim-test] clear readback mismatch at pixel %u: 0x%08X != 0x%08X\n", i, pixels[i], expected);
                        break;
                    }
                }
                unmap_memory(device, buffer_memory);
            }
            else
            {
                ok = false;
                std::printf("[shim-test] readback vkMapMemory failed\n");
            }
        }

        std::printf("[shim-test] clear+readback (%ux%u -> 0x%08X) wait=%d -> %s\n", width, height, expected, wait_result,
                    ok ? "PASS" : "FAIL");

        destroy_fence(device, fence, nullptr);
        destroy_command_pool(device, pool, nullptr);
        destroy_buffer(device, buffer, nullptr);
        free_memory(device, buffer_memory, nullptr);
        destroy_image(device, image, nullptr);
        free_memory(device, image_memory, nullptr);
        return ok;
    }

    // One 4x4 block of a BC format and the texels the Khronos Data Format Specification decodes it to. The indices of each
    // row step through the palette, so every entry is checked.
    struct bc_case
    {
        const char* name;
        VkFormat format;
        VkFormat texel_format;
        std::vector<uint8_t> block;
        std::array<std::array<int, 4>, 16> texels;
        // Which channels hold interpolated BC3 alpha, BC4 or BC5 values, which decoders compute at different precisions.
        std::array<bool, 4> interpolated_channels;
        // The format of the image the block is copied into, whose decoding the texels are, when it is not the same.
        VkFormat copy_format = VK_FORMAT_UNDEFINED;
    };

    using rgb = std::array<int, 3>;

    std::vector<uint8_t> color_block(const uint16_t c0, const uint16_t c1)
    {
        return {static_cast<uint8_t>(c0),
                static_cast<uint8_t>(c0 >> 8),
                static_cast<uint8_t>(c1),
                static_cast<uint8_t>(c1 >> 8),
                0xE4,
                0xE4,
                0xE4,
                0xE4};
    }

    // Texel t uses palette entry t % 8.
    std::vector<uint8_t> channel_block(const int e0, const int e1)
    {
        uint64_t indices = 0;
        for (uint64_t t = 0; t < 16; ++t)
        {
            indices |= (t % 8) << (3 * t);
        }

        std::vector<uint8_t> block{static_cast<uint8_t>(e0), static_cast<uint8_t>(e1)};
        for (int i = 0; i < 6; ++i)
        {
            block.push_back(static_cast<uint8_t>(indices >> (8 * i)));
        }
        return block;
    }

    std::array<int, 8> channel_palette(const int e0, const int e1, const int low, const int high)
    {
        std::array<int, 8> values{e0, e1};
        if (e0 > e1)
        {
            for (int i = 1; i < 7; ++i)
            {
                values[i + 1] = static_cast<int>(std::lround(((7.0 - i) * e0 + i * e1) / 7.0));
            }
        }
        else
        {
            for (int i = 1; i < 5; ++i)
            {
                values[i + 1] = static_cast<int>(std::lround(((5.0 - i) * e0 + i * e1) / 5.0));
            }
            values[6] = low;
            values[7] = high;
        }
        return values;
    }

    std::vector<bc_case> make_bc_cases()
    {
        // 0xF800 is red and 0x001F blue; four colors interpolate at thirds, three colors at the middle plus black.
        const std::array<rgb, 4> four{rgb{255, 0, 0}, rgb{0, 0, 255}, rgb{170, 0, 85}, rgb{85, 0, 170}};
        const std::array<rgb, 4> three{rgb{0, 0, 255}, rgb{255, 0, 0}, rgb{128, 0, 128}, rgb{0, 0, 0}};
        const auto unsigned_palette = channel_palette(255, 0, 0, 255);
        const auto rising_palette = channel_palette(0, 255, 0, 255);
        const auto signed_palette = channel_palette(127, -127, -127, 127);
        const auto signed_rising_palette = channel_palette(-127, 127, -127, 127);

        std::vector<bc_case> cases;
        const auto add = [&](const char* name, const VkFormat format, const VkFormat texel_format, std::vector<uint8_t> block,
                             const std::array<bool, 4> interpolated_channels, const auto& texel) {
            bc_case entry{name, format, texel_format, std::move(block), {}, interpolated_channels};
            for (int t = 0; t < 16; ++t)
            {
                entry.texels[t] = texel(t);
            }
            cases.push_back(std::move(entry));
        };

        add("BC1 four colors", VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_R8G8B8A8_UNORM, color_block(0xF800, 0x001F),
            {false, false, false, false}, [&](const int t) {
                const auto& c = four[t % 4];
                return std::array<int, 4>{c[0], c[1], c[2], 255};
            });
        add("BC1 three colors and transparent black", VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_R8G8B8A8_UNORM, color_block(0x001F, 0xF800),
            {false, false, false, false}, [&](const int t) {
                const auto& c = three[t % 4];
                return std::array<int, 4>{c[0], c[1], c[2], t % 4 == 3 ? 0 : 255};
            });

        std::vector<uint8_t> bc2{0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE};
        const auto colors = color_block(0xF800, 0x001F);
        bc2.insert(bc2.end(), colors.begin(), colors.end());
        add("BC2 explicit alpha", VK_FORMAT_BC2_UNORM_BLOCK, VK_FORMAT_R8G8B8A8_UNORM, bc2, {false, false, false, false}, [&](const int t) {
            const auto& c = four[t % 4];
            return std::array<int, 4>{c[0], c[1], c[2], t * 17};
        });

        // The same blocks without alpha: the fourth color of the three-color mode is opaque black.
        add("BC1 copied into BC1 without alpha", VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_R8G8B8A8_UNORM, color_block(0x001F, 0xF800),
            {false, false, false, false}, [&](const int t) {
                const auto& c = three[t % 4];
                return std::array<int, 4>{c[0], c[1], c[2], 255};
            });
        cases.back().copy_format = VK_FORMAT_BC1_RGB_UNORM_BLOCK;

        // BC4 reads the same 8 bytes as one channel: the color endpoints' low bytes as its endpoints, the rest as indices.
        const auto bc1 = color_block(0xF800, 0x001F);
        uint64_t bc1_as_indices = 0;
        for (size_t i = 2; i < bc1.size(); ++i)
        {
            bc1_as_indices |= static_cast<uint64_t>(bc1[i]) << (8 * (i - 2));
        }
        const auto bc1_as_channel = channel_palette(bc1[0], bc1[1], 0, 255);
        add("BC1 copied into BC4", VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_R8G8B8A8_UNORM, bc1, {true, false, false, false},
            [&](const int t) { return std::array<int, 4>{bc1_as_channel[(bc1_as_indices >> (3 * t)) & 7], 0, 0, 255}; });
        cases.back().copy_format = VK_FORMAT_BC4_UNORM_BLOCK;

        auto bc3 = channel_block(255, 0);
        bc3.insert(bc3.end(), colors.begin(), colors.end());
        add("BC3 interpolated alpha", VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_R8G8B8A8_UNORM, bc3, {false, false, false, true},
            [&](const int t) {
                const auto& c = four[t % 4];
                return std::array<int, 4>{c[0], c[1], c[2], unsigned_palette[t % 8]};
            });

        add("BC4 unsigned", VK_FORMAT_BC4_UNORM_BLOCK, VK_FORMAT_R8G8B8A8_UNORM, channel_block(255, 0), {true, false, false, false},
            [&](const int t) { return std::array<int, 4>{unsigned_palette[t % 8], 0, 0, 255}; });
        add("BC4 signed", VK_FORMAT_BC4_SNORM_BLOCK, VK_FORMAT_R8G8B8A8_SNORM, channel_block(127, -127), {true, false, false, false},
            [&](const int t) { return std::array<int, 4>{signed_palette[t % 8], 0, 0, 127}; });

        auto bc5 = channel_block(255, 0);
        const auto green = channel_block(0, 255);
        bc5.insert(bc5.end(), green.begin(), green.end());
        add("BC5 unsigned", VK_FORMAT_BC5_UNORM_BLOCK, VK_FORMAT_R8G8B8A8_UNORM, bc5, {true, true, false, false},
            [&](const int t) { return std::array<int, 4>{unsigned_palette[t % 8], rising_palette[t % 8], 0, 255}; });

        auto bc5_signed = channel_block(127, -127);
        const auto signed_green = channel_block(-127, 127);
        bc5_signed.insert(bc5_signed.end(), signed_green.begin(), signed_green.end());
        add("BC5 signed", VK_FORMAT_BC5_SNORM_BLOCK, VK_FORMAT_R8G8B8A8_SNORM, bc5_signed, {true, true, false, false},
            [&](const int t) { return std::array<int, 4>{signed_palette[t % 8], signed_rising_palette[t % 8], 0, 127}; });
        return cases;
    }

    // Uploads each block, copies it to a second image, reads that back, and blits the second image into an uncompressed one
    // to compare its texels. Decoders may round interpolated colors either way, so those may differ by one. Interpolated
    // BC3 alpha, BC4 and BC5 values differ by more on hardware that decodes them at another precision (up to 7 on NVIDIA).
    bool test_bc_formats(PFN_vkGetInstanceProcAddr get_instance_proc, VkInstance instance, VkPhysicalDevice physical_device,
                         VkDevice device, VkQueue queue, uint32_t queue_family)
    {
#define LOAD(type, name) const auto name = reinterpret_cast<PFN_##type>(get_instance_proc(instance, #type))
        LOAD(vkGetPhysicalDeviceFormatProperties, vkGetPhysicalDeviceFormatProperties);
        LOAD(vkGetPhysicalDeviceMemoryProperties, vkGetPhysicalDeviceMemoryProperties);
        LOAD(vkCreateImage, vkCreateImage);
        LOAD(vkDestroyImage, vkDestroyImage);
        LOAD(vkGetImageMemoryRequirements, vkGetImageMemoryRequirements);
        LOAD(vkBindImageMemory, vkBindImageMemory);
        LOAD(vkCreateBuffer, vkCreateBuffer);
        LOAD(vkDestroyBuffer, vkDestroyBuffer);
        LOAD(vkGetBufferMemoryRequirements, vkGetBufferMemoryRequirements);
        LOAD(vkBindBufferMemory, vkBindBufferMemory);
        LOAD(vkAllocateMemory, vkAllocateMemory);
        LOAD(vkFreeMemory, vkFreeMemory);
        LOAD(vkMapMemory, vkMapMemory);
        LOAD(vkUnmapMemory, vkUnmapMemory);
        LOAD(vkCreateCommandPool, vkCreateCommandPool);
        LOAD(vkDestroyCommandPool, vkDestroyCommandPool);
        LOAD(vkAllocateCommandBuffers, vkAllocateCommandBuffers);
        LOAD(vkBeginCommandBuffer, vkBeginCommandBuffer);
        LOAD(vkEndCommandBuffer, vkEndCommandBuffer);
        LOAD(vkCmdPipelineBarrier, vkCmdPipelineBarrier);
        LOAD(vkCmdCopyBufferToImage, vkCmdCopyBufferToImage);
        LOAD(vkCmdCopyImageToBuffer, vkCmdCopyImageToBuffer);
        LOAD(vkCmdCopyImage, vkCmdCopyImage);
        LOAD(vkCmdBlitImage, vkCmdBlitImage);
        LOAD(vkQueueSubmit, vkQueueSubmit);
        LOAD(vkQueueWaitIdle, vkQueueWaitIdle);
#undef LOAD

        VkPhysicalDeviceMemoryProperties memory_properties{};
        vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);

        constexpr VkDeviceSize readback_offset = 256;
        constexpr VkDeviceSize texel_offset = 512;
        constexpr VkDeviceSize buffer_size = 1024;
        constexpr VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                                                VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT;

        std::vector<VkDeviceMemory> memories;
        const auto bind = [&](const VkMemoryRequirements& requirements, const VkMemoryPropertyFlags flags) {
            VkMemoryAllocateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            info.allocationSize = requirements.size;
            info.memoryTypeIndex = find_memory_type(memory_properties, requirements.memoryTypeBits, flags);
            VkDeviceMemory memory = VK_NULL_HANDLE;
            if (info.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(device, &info, nullptr, &memory) != VK_SUCCESS)
            {
                return VkDeviceMemory{VK_NULL_HANDLE};
            }
            memories.push_back(memory);
            return memory;
        };

        const auto create_image = [&](const VkFormat format, const VkImageUsageFlags usage) {
            VkImageCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            info.imageType = VK_IMAGE_TYPE_2D;
            info.format = format;
            info.extent = {.width = 4, .height = 4, .depth = 1};
            info.mipLevels = 1;
            info.arrayLayers = 1;
            info.samples = VK_SAMPLE_COUNT_1_BIT;
            info.tiling = VK_IMAGE_TILING_OPTIMAL;
            info.usage = usage;
            info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            VkImage image = VK_NULL_HANDLE;
            if (vkCreateImage(device, &info, nullptr, &image) != VK_SUCCESS)
            {
                return VkImage{VK_NULL_HANDLE};
            }
            VkMemoryRequirements requirements{};
            vkGetImageMemoryRequirements(device, image, &requirements);
            const auto memory = bind(requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (!memory || vkBindImageMemory(device, image, memory, 0) != VK_SUCCESS)
            {
                vkDestroyImage(device, image, nullptr);
                return VkImage{VK_NULL_HANDLE};
            }
            return image;
        };

        const auto barrier = [&](const VkCommandBuffer cmd, const VkImage image, const VkImageLayout from, const VkImageLayout to) {
            VkImageMemoryBarrier info{};
            info.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            info.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            info.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
            info.oldLayout = from;
            info.newLayout = to;
            info.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            info.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            info.image = image;
            info.subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &info);
        };

        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = queue_family;
        VkCommandPool pool = VK_NULL_HANDLE;
        if (vkCreateCommandPool(device, &pool_info, nullptr, &pool) != VK_SUCCESS)
        {
            std::printf("[shim-test] BC formats -> FAILED (command pool)\n");
            return false;
        }

        bool all_ok = true;
        for (const auto& test : make_bc_cases())
        {
            const VkFormat copy_format = test.copy_format != VK_FORMAT_UNDEFINED ? test.copy_format : test.format;
            VkFormatProperties bc_properties{};
            vkGetPhysicalDeviceFormatProperties(physical_device, test.format, &bc_properties);
            VkFormatProperties copy_properties{};
            vkGetPhysicalDeviceFormatProperties(physical_device, copy_format, &copy_properties);
            VkFormatProperties texel_properties{};
            vkGetPhysicalDeviceFormatProperties(physical_device, test.texel_format, &texel_properties);
            if ((bc_properties.optimalTilingFeatures & needed) != needed || (copy_properties.optimalTilingFeatures & needed) != needed ||
                (texel_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) == 0)
            {
                std::printf("[shim-test] %s -> SKIP (format unsupported)\n", test.name);
                continue;
            }

            constexpr VkImageUsageFlags transfer = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            const VkImage blocks = create_image(test.format, transfer | VK_IMAGE_USAGE_SAMPLED_BIT);
            const VkImage copied = create_image(copy_format, transfer | VK_IMAGE_USAGE_SAMPLED_BIT);
            const VkImage texels = create_image(test.texel_format, transfer);

            VkBufferCreateInfo buffer_info{};
            buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            buffer_info.size = buffer_size;
            buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            VkBuffer buffer = VK_NULL_HANDLE;
            vkCreateBuffer(device, &buffer_info, nullptr, &buffer);
            VkMemoryRequirements buffer_requirements{};
            vkGetBufferMemoryRequirements(device, buffer, &buffer_requirements);
            const auto buffer_memory =
                bind(buffer_requirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

            void* mapped = nullptr;
            const bool ready = blocks && copied && texels && buffer_memory &&
                               vkBindBufferMemory(device, buffer, buffer_memory, 0) == VK_SUCCESS &&
                               vkMapMemory(device, buffer_memory, 0, buffer_size, 0, &mapped) == VK_SUCCESS;
            bool ok = ready;
            if (ready)
            {
                std::memset(mapped, 0, buffer_size);
                std::memcpy(mapped, test.block.data(), test.block.size());

                VkCommandBufferAllocateInfo allocate_info{};
                allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
                allocate_info.commandPool = pool;
                allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                allocate_info.commandBufferCount = 1;
                VkCommandBuffer cmd = VK_NULL_HANDLE;
                vkAllocateCommandBuffers(device, &allocate_info, &cmd);

                VkCommandBufferBeginInfo begin{};
                begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                vkBeginCommandBuffer(cmd, &begin);

                VkBufferImageCopy region{};
                region.imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1};
                region.imageExtent = {.width = 4, .height = 4, .depth = 1};

                barrier(cmd, blocks, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                vkCmdCopyBufferToImage(cmd, buffer, blocks, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
                barrier(cmd, blocks, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

                VkImageCopy copy{};
                copy.srcSubresource = region.imageSubresource;
                copy.dstSubresource = region.imageSubresource;
                copy.extent = region.imageExtent;
                barrier(cmd, copied, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                vkCmdCopyImage(cmd, blocks, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, copied, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
                barrier(cmd, copied, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                region.bufferOffset = readback_offset;
                vkCmdCopyImageToBuffer(cmd, copied, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);

                VkImageBlit blit{};
                blit.srcSubresource = region.imageSubresource;
                blit.dstSubresource = region.imageSubresource;
                blit.srcOffsets[1] = {.x = 4, .y = 4, .z = 1};
                blit.dstOffsets[1] = {.x = 4, .y = 4, .z = 1};
                barrier(cmd, texels, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                vkCmdBlitImage(cmd, copied, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, texels, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                               VK_FILTER_NEAREST);
                barrier(cmd, texels, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                region.bufferOffset = texel_offset;
                vkCmdCopyImageToBuffer(cmd, texels, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
                vkEndCommandBuffer(cmd);

                VkSubmitInfo submit{};
                submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                submit.commandBufferCount = 1;
                submit.pCommandBuffers = &cmd;
                ok = vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS && vkQueueWaitIdle(queue) == VK_SUCCESS;

                const auto* bytes = static_cast<const uint8_t*>(mapped);
                if (ok && std::memcmp(bytes + readback_offset, test.block.data(), test.block.size()) != 0)
                {
                    std::printf("[shim-test] %s -> FAILED (blocks read back differ)\n", test.name);
                    ok = false;
                }

                const bool is_signed = test.texel_format == VK_FORMAT_R8G8B8A8_SNORM;
                for (int t = 0; ok && t < 16; ++t)
                {
                    for (int c = 0; c < 4; ++c)
                    {
                        const uint8_t raw = bytes[texel_offset + (t * 4) + c];
                        const int value = is_signed ? static_cast<int8_t>(raw) : raw;
                        const int tolerance = test.interpolated_channels[c] ? 8 : 1;
                        if (std::abs(value - test.texels[t][c]) > tolerance)
                        {
                            std::printf("[shim-test] %s -> FAILED (texel %d channel %d is %d, expected %d)\n", test.name, t, c, value,
                                        test.texels[t][c]);
                            ok = false;
                            break;
                        }
                    }
                }
                vkUnmapMemory(device, buffer_memory);
            }

            std::printf("[shim-test] %s -> %s\n", test.name, ok ? "ok" : "FAILED");
            all_ok = all_ok && ok;

            vkDestroyBuffer(device, buffer, nullptr);
            for (const VkImage image : {blocks, copied, texels})
            {
                if (image)
                {
                    vkDestroyImage(device, image, nullptr);
                }
            }
            for (const auto memory : memories)
            {
                vkFreeMemory(device, memory, nullptr);
            }
            memories.clear();
        }

        vkDestroyCommandPool(device, pool, nullptr);
        return all_ok;
    }

    bool test_shader_module_identifier(PFN_vkGetInstanceProcAddr get_instance_proc, VkInstance instance, VkDevice device)
    {
        const auto get_device_proc = reinterpret_cast<PFN_vkGetDeviceProcAddr>(get_instance_proc(instance, "vkGetDeviceProcAddr"));
        if (!get_device_proc)
        {
            std::printf("[shim-test] no vkGetDeviceProcAddr for shader identifier test\n");
            return false;
        }

        const auto get_create_info_identifier = reinterpret_cast<PFN_vkGetShaderModuleCreateInfoIdentifierEXT>(
            get_device_proc(device, "vkGetShaderModuleCreateInfoIdentifierEXT"));
        const auto get_module_identifier =
            reinterpret_cast<PFN_vkGetShaderModuleIdentifierEXT>(get_device_proc(device, "vkGetShaderModuleIdentifierEXT"));
        const auto create_shader_module = reinterpret_cast<PFN_vkCreateShaderModule>(get_device_proc(device, "vkCreateShaderModule"));
        const auto destroy_shader_module = reinterpret_cast<PFN_vkDestroyShaderModule>(get_device_proc(device, "vkDestroyShaderModule"));
        const auto create_pipeline_layout = reinterpret_cast<PFN_vkCreatePipelineLayout>(get_device_proc(device, "vkCreatePipelineLayout"));
        const auto destroy_pipeline_layout =
            reinterpret_cast<PFN_vkDestroyPipelineLayout>(get_device_proc(device, "vkDestroyPipelineLayout"));
        const auto create_compute_pipelines =
            reinterpret_cast<PFN_vkCreateComputePipelines>(get_device_proc(device, "vkCreateComputePipelines"));
        const auto destroy_pipeline = reinterpret_cast<PFN_vkDestroyPipeline>(get_device_proc(device, "vkDestroyPipeline"));
        if (!get_create_info_identifier || !get_module_identifier || !create_shader_module || !destroy_shader_module ||
            !create_pipeline_layout || !destroy_pipeline_layout || !create_compute_pipelines || !destroy_pipeline)
        {
            std::printf("[shim-test] shader module identifier entry point missing\n");
            return false;
        }

        // Minimal SPIR-V 1.0 compute shader: layout(local_size_x=1, local_size_y=1, local_size_z=1) in; void main() {}
        constexpr std::array<uint32_t, 42> compute_spirv{
            0x07230203, 0x00010000, 0x00000000, 0x00000005, 0x00000000, 0x00020011, 0x00000001, 0x0003000e, 0x00000000,
            0x00000001, 0x0005000f, 0x00000005, 0x00000003, 0x6e69616d, 0x00000000, 0x00060010, 0x00000003, 0x00000011,
            0x00000001, 0x00000001, 0x00000001, 0x00030003, 0x00000002, 0x000001c2, 0x00040005, 0x00000003, 0x6e69616d,
            0x00000000, 0x00020013, 0x00000001, 0x00030021, 0x00000002, 0x00000001, 0x00050036, 0x00000001, 0x00000003,
            0x00000000, 0x00000002, 0x000200f8, 0x00000004, 0x000100fd, 0x00010038,
        };

        VkShaderModuleCreateInfo shader_info{};
        shader_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        shader_info.codeSize = compute_spirv.size() * sizeof(uint32_t);
        shader_info.pCode = compute_spirv.data();

        VkShaderModuleIdentifierEXT from_create_info{};
        from_create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_IDENTIFIER_EXT;
        get_create_info_identifier(device, &shader_info, &from_create_info);

        VkShaderModule shader = VK_NULL_HANDLE;
        const VkResult shader_result = create_shader_module(device, &shader_info, nullptr, &shader);
        if (shader_result != VK_SUCCESS)
        {
            std::printf("[shim-test] shader identifier vkCreateShaderModule -> %d -> FAIL\n", shader_result);
            return false;
        }

        VkShaderModuleIdentifierEXT from_module{};
        from_module.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_IDENTIFIER_EXT;
        get_module_identifier(device, shader, &from_module);

        const bool identifiers_match =
            from_create_info.identifierSize > 0 && from_create_info.identifierSize <= VK_MAX_SHADER_MODULE_IDENTIFIER_SIZE_EXT &&
            from_create_info.identifierSize == from_module.identifierSize &&
            std::memcmp(from_create_info.identifier, from_module.identifier, from_create_info.identifierSize) == 0;

        VkPipelineLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        const VkResult layout_result = create_pipeline_layout(device, &layout_info, nullptr, &layout);

        VkResult pipeline_result = VK_ERROR_INITIALIZATION_FAILED;
        VkPipeline pipeline = VK_NULL_HANDLE;
        if (layout_result == VK_SUCCESS && identifiers_match)
        {
            VkPipelineShaderStageModuleIdentifierCreateInfoEXT identifier_info{};
            identifier_info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_MODULE_IDENTIFIER_CREATE_INFO_EXT;
            identifier_info.identifierSize = from_create_info.identifierSize;
            identifier_info.pIdentifier = from_create_info.identifier;

            VkPipelineShaderStageCreateInfo stage{};
            stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stage.pNext = &identifier_info;
            stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            stage.module = VK_NULL_HANDLE;
            stage.pName = "main";

            VkComputePipelineCreateInfo pipeline_info{};
            pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            pipeline_info.flags = VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
            pipeline_info.stage = stage;
            pipeline_info.layout = layout;
            pipeline_result = create_compute_pipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline);
        }

        const bool pipeline_ok = pipeline_result == VK_SUCCESS || pipeline_result == VK_PIPELINE_COMPILE_REQUIRED_EXT;
        std::printf("[shim-test] shader identifiers size=%u match=%s, identifier pipeline=%d -> %s\n", from_create_info.identifierSize,
                    identifiers_match ? "yes" : "no", pipeline_result, (identifiers_match && pipeline_ok) ? "PASS" : "FAIL");

        if (pipeline != VK_NULL_HANDLE)
        {
            destroy_pipeline(device, pipeline, nullptr);
        }
        if (layout != VK_NULL_HANDLE)
        {
            destroy_pipeline_layout(device, layout, nullptr);
        }
        destroy_shader_module(device, shader, nullptr);
        return identifiers_match && pipeline_ok;
    }

    // A ring record the host could not run fails the next submission, and only that one: the submissions after it ran.
    // The record resets a command pool the host never handed out. Where the host maps no ring, the reset is made at once
    // and fails there.
    bool test_ring_failure_is_reported_once(PFN_vkGetInstanceProcAddr get_instance_proc, VkInstance instance, VkDevice device,
                                            VkQueue queue)
    {
        const auto reset_command_pool = reinterpret_cast<PFN_vkResetCommandPool>(get_instance_proc(instance, "vkResetCommandPool"));
        const auto create_fence = reinterpret_cast<PFN_vkCreateFence>(get_instance_proc(instance, "vkCreateFence"));
        const auto destroy_fence = reinterpret_cast<PFN_vkDestroyFence>(get_instance_proc(instance, "vkDestroyFence"));
        const auto wait_for_fences = reinterpret_cast<PFN_vkWaitForFences>(get_instance_proc(instance, "vkWaitForFences"));
        const auto reset_fences = reinterpret_cast<PFN_vkResetFences>(get_instance_proc(instance, "vkResetFences"));
        const auto queue_submit = reinterpret_cast<PFN_vkQueueSubmit>(get_instance_proc(instance, "vkQueueSubmit"));
        if (!reset_command_pool || !create_fence || !destroy_fence || !wait_for_fences || !reset_fences || !queue_submit)
        {
            std::printf("[shim-test] ring failure: missing entry points -> FAIL\n");
            return false;
        }

        const VkResult reset = reset_command_pool(device, VK_NULL_HANDLE, 0);
        if (reset != VK_SUCCESS)
        {
            std::printf("[shim-test] ring failure: no ring, the reset failed at once (%d) -> SKIP\n", reset);
            return true;
        }

        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence = VK_NULL_HANDLE;
        if (create_fence(device, &fence_info, nullptr, &fence) != VK_SUCCESS)
        {
            std::printf("[shim-test] ring failure: vkCreateFence failed -> FAIL\n");
            return false;
        }

        // An empty submission with a fence still goes to the host, which runs the ring before it.
        const VkResult first = queue_submit(queue, 0, nullptr, fence);
        wait_for_fences(device, 1, &fence, VK_TRUE, UINT64_MAX);
        reset_fences(device, 1, &fence);
        const VkResult second = queue_submit(queue, 0, nullptr, fence);
        wait_for_fences(device, 1, &fence, VK_TRUE, UINT64_MAX);
        destroy_fence(device, fence, nullptr);

        const bool ok = first == VK_ERROR_INITIALIZATION_FAILED && second == VK_SUCCESS;
        std::printf("[shim-test] ring failure: first submission %d, second %d -> %s\n", first, second, ok ? "PASS" : "FAIL");
        return ok;
    }

    // Compiles graphics pipelines on this thread while another thread keeps calling the bridge, so bridge calls arrive
    // during the compiles. The bridge compiles with neither the kernel lock nor its own lock held: a compile that kept its
    // own lock would take the kernel lock back while the polling thread holds the kernel lock and waits for the bridge's,
    // and the emulator would hang here. That takes more than one vCPU.
    bool test_pipelines_compile_beside_bridge_calls(PFN_vkGetInstanceProcAddr get_instance_proc, VkInstance instance, VkDevice device)
    {
        const auto get_device_proc = reinterpret_cast<PFN_vkGetDeviceProcAddr>(get_instance_proc(instance, "vkGetDeviceProcAddr"));
        if (!get_device_proc)
        {
            std::printf("[shim-test] no vkGetDeviceProcAddr for the pipeline compile test\n");
            return false;
        }

        const auto create_shader_module = reinterpret_cast<PFN_vkCreateShaderModule>(get_device_proc(device, "vkCreateShaderModule"));
        const auto destroy_shader_module = reinterpret_cast<PFN_vkDestroyShaderModule>(get_device_proc(device, "vkDestroyShaderModule"));
        const auto create_pipeline_layout = reinterpret_cast<PFN_vkCreatePipelineLayout>(get_device_proc(device, "vkCreatePipelineLayout"));
        const auto destroy_pipeline_layout =
            reinterpret_cast<PFN_vkDestroyPipelineLayout>(get_device_proc(device, "vkDestroyPipelineLayout"));
        const auto create_render_pass = reinterpret_cast<PFN_vkCreateRenderPass>(get_device_proc(device, "vkCreateRenderPass"));
        const auto destroy_render_pass = reinterpret_cast<PFN_vkDestroyRenderPass>(get_device_proc(device, "vkDestroyRenderPass"));
        const auto create_graphics_pipelines =
            reinterpret_cast<PFN_vkCreateGraphicsPipelines>(get_device_proc(device, "vkCreateGraphicsPipelines"));
        const auto destroy_pipeline = reinterpret_cast<PFN_vkDestroyPipeline>(get_device_proc(device, "vkDestroyPipeline"));
        const auto create_fence = reinterpret_cast<PFN_vkCreateFence>(get_device_proc(device, "vkCreateFence"));
        const auto destroy_fence = reinterpret_cast<PFN_vkDestroyFence>(get_device_proc(device, "vkDestroyFence"));
        const auto get_fence_status = reinterpret_cast<PFN_vkGetFenceStatus>(get_device_proc(device, "vkGetFenceStatus"));
        if (!create_shader_module || !destroy_shader_module || !create_pipeline_layout || !destroy_pipeline_layout || !create_render_pass ||
            !destroy_render_pass || !create_graphics_pipelines || !destroy_pipeline || !create_fence || !destroy_fence || !get_fence_status)
        {
            std::printf("[shim-test] pipeline compile test entry point missing\n");
            return false;
        }

        // SPIR-V 1.0 (glslang -V --target-env vulkan1.0 -g0) of
        //   #version 450
        //   out gl_PerVertex { vec4 gl_Position; };
        //   void main() { gl_Position = vec4(0.0, 0.0, 0.0, 1.0); }
        constexpr std::array<uint32_t, 119> vertex_spirv{
            0x07230203, 0x00010000, 0x0008000b, 0x00000012, 0x00000000, 0x00020011, 0x00000001, 0x0006000b, 0x00000001, 0x4c534c47,
            0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0006000f, 0x00000000, 0x00000004, 0x6e69616d,
            0x00000000, 0x0000000a, 0x00030003, 0x00000002, 0x000001c2, 0x00040005, 0x00000004, 0x6e69616d, 0x00000000, 0x00060005,
            0x00000008, 0x505f6c67, 0x65567265, 0x78657472, 0x00000000, 0x00060006, 0x00000008, 0x00000000, 0x505f6c67, 0x7469736f,
            0x006e6f69, 0x00030005, 0x0000000a, 0x00000000, 0x00030047, 0x00000008, 0x00000002, 0x00050048, 0x00000008, 0x00000000,
            0x0000000b, 0x00000000, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00030016, 0x00000006, 0x00000020,
            0x00040017, 0x00000007, 0x00000006, 0x00000004, 0x0003001e, 0x00000008, 0x00000007, 0x00040020, 0x00000009, 0x00000003,
            0x00000008, 0x0004003b, 0x00000009, 0x0000000a, 0x00000003, 0x00040015, 0x0000000b, 0x00000020, 0x00000001, 0x0004002b,
            0x0000000b, 0x0000000c, 0x00000000, 0x0004002b, 0x00000006, 0x0000000d, 0x00000000, 0x0004002b, 0x00000006, 0x0000000e,
            0x3f800000, 0x0007002c, 0x00000007, 0x0000000f, 0x0000000d, 0x0000000d, 0x0000000d, 0x0000000e, 0x00040020, 0x00000010,
            0x00000003, 0x00000007, 0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x00050041,
            0x00000010, 0x00000011, 0x0000000a, 0x0000000c, 0x0003003e, 0x00000011, 0x0000000f, 0x000100fd, 0x00010038,
        };
        // and of
        //   #version 450
        //   layout(constant_id = 0) const float shade = 1.0;
        //   layout(location = 0) out vec4 color;
        //   void main() { color = vec4(shade); }
        constexpr std::array<uint32_t, 91> fragment_spirv{
            0x07230203, 0x00010000, 0x0008000b, 0x0000000c, 0x00000000, 0x00020011, 0x00000001, 0x0006000b, 0x00000001, 0x4c534c47,
            0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0006000f, 0x00000004, 0x00000004, 0x6e69616d,
            0x00000000, 0x00000009, 0x00030010, 0x00000004, 0x00000007, 0x00030003, 0x00000002, 0x000001c2, 0x00040005, 0x00000004,
            0x6e69616d, 0x00000000, 0x00040005, 0x00000009, 0x6f6c6f63, 0x00000072, 0x00040005, 0x0000000a, 0x64616873, 0x00000065,
            0x00040047, 0x00000009, 0x0000001e, 0x00000000, 0x00040047, 0x0000000a, 0x00000001, 0x00000000, 0x00020013, 0x00000002,
            0x00030021, 0x00000003, 0x00000002, 0x00030016, 0x00000006, 0x00000020, 0x00040017, 0x00000007, 0x00000006, 0x00000004,
            0x00040020, 0x00000008, 0x00000003, 0x00000007, 0x0004003b, 0x00000008, 0x00000009, 0x00000003, 0x00040032, 0x00000006,
            0x0000000a, 0x3f800000, 0x00070033, 0x00000007, 0x0000000b, 0x0000000a, 0x0000000a, 0x0000000a, 0x0000000a, 0x00050036,
            0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0003003e, 0x00000009, 0x0000000b, 0x000100fd,
            0x00010038,
        };

        VkShaderModuleCreateInfo vertex_info{};
        vertex_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        vertex_info.codeSize = vertex_spirv.size() * sizeof(uint32_t);
        vertex_info.pCode = vertex_spirv.data();
        VkShaderModuleCreateInfo fragment_info = vertex_info;
        fragment_info.codeSize = fragment_spirv.size() * sizeof(uint32_t);
        fragment_info.pCode = fragment_spirv.data();

        VkPipelineLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;

        VkAttachmentDescription attachment{};
        attachment.format = VK_FORMAT_R8G8B8A8_UNORM;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        const VkAttachmentReference color_reference{.attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &color_reference;
        VkRenderPassCreateInfo render_pass_info{};
        render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        render_pass_info.attachmentCount = 1;
        render_pass_info.pAttachments = &attachment;
        render_pass_info.subpassCount = 1;
        render_pass_info.pSubpasses = &subpass;

        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;

        VkShaderModule vertex_shader = VK_NULL_HANDLE;
        VkShaderModule fragment_shader = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkRenderPass render_pass = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        const bool setup_ok = create_shader_module(device, &vertex_info, nullptr, &vertex_shader) == VK_SUCCESS &&
                              create_shader_module(device, &fragment_info, nullptr, &fragment_shader) == VK_SUCCESS &&
                              create_pipeline_layout(device, &layout_info, nullptr, &layout) == VK_SUCCESS &&
                              create_render_pass(device, &render_pass_info, nullptr, &render_pass) == VK_SUCCESS &&
                              create_fence(device, &fence_info, nullptr, &fence) == VK_SUCCESS;

        // A new constant value for each pipeline, so the host driver compiles every one instead of reusing the first.
        float shade = 0.0f;
        const VkSpecializationMapEntry shade_entry{.constantID = 0, .offset = 0, .size = sizeof(shade)};
        VkSpecializationInfo specialization{};
        specialization.mapEntryCount = 1;
        specialization.pMapEntries = &shade_entry;
        specialization.dataSize = sizeof(shade);
        specialization.pData = &shade;

        std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertex_shader;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragment_shader;
        stages[1].pName = "main";
        stages[1].pSpecializationInfo = &specialization;

        VkPipelineVertexInputStateCreateInfo vertex_input{};
        vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo input_assembly{};
        input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        constexpr uint32_t extent = 16;
        VkViewport viewport{};
        viewport.width = static_cast<float>(extent);
        viewport.height = static_cast<float>(extent);
        viewport.maxDepth = 1.0f;
        const VkRect2D scissor{.offset = {.x = 0, .y = 0}, .extent = {.width = extent, .height = extent}};
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

        VkGraphicsPipelineCreateInfo pipeline_info{};
        pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipeline_info.stageCount = static_cast<uint32_t>(stages.size());
        pipeline_info.pStages = stages.data();
        pipeline_info.pVertexInputState = &vertex_input;
        pipeline_info.pInputAssemblyState = &input_assembly;
        pipeline_info.pViewportState = &viewport_state;
        pipeline_info.pRasterizationState = &rasterization;
        pipeline_info.pMultisampleState = &multisample;
        pipeline_info.pColorBlendState = &color_blend;
        pipeline_info.layout = layout;
        pipeline_info.renderPass = render_pass;

        constexpr uint32_t pipeline_count = 64;
        uint32_t compiled = 0;
        std::atomic<bool> compiling{true};
        std::atomic<uint32_t> polls{0};
        std::atomic<bool> polls_ok{true};
        if (setup_ok)
        {
            // The fence is never submitted, so every poll is a bridge call that answers VK_NOT_READY.
            std::thread poller([&] {
                while (compiling.load())
                {
                    if (get_fence_status(device, fence) != VK_NOT_READY)
                    {
                        polls_ok = false;
                    }
                    polls.fetch_add(1);
                }
            });
            // The polls start before the first compile, so they run through all of them.
            while (polls.load() == 0)
            {
                std::this_thread::yield();
            }

            for (uint32_t i = 0; i < pipeline_count; ++i)
            {
                shade = static_cast<float>(i) / static_cast<float>(pipeline_count);
                VkPipeline pipeline = VK_NULL_HANDLE;
                if (create_graphics_pipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline) == VK_SUCCESS)
                {
                    ++compiled;
                    destroy_pipeline(device, pipeline, nullptr);
                }
            }

            compiling = false;
            poller.join();
        }

        const bool ok = setup_ok && compiled == pipeline_count && polls_ok.load();
        std::printf("[shim-test] %u of %u graphics pipelines compiled beside %u fence polls -> %s\n", compiled, pipeline_count,
                    polls.load(), ok ? "PASS" : "FAIL");

        if (fence != VK_NULL_HANDLE)
        {
            destroy_fence(device, fence, nullptr);
        }
        if (render_pass != VK_NULL_HANDLE)
        {
            destroy_render_pass(device, render_pass, nullptr);
        }
        if (layout != VK_NULL_HANDLE)
        {
            destroy_pipeline_layout(device, layout, nullptr);
        }
        if (fragment_shader != VK_NULL_HANDLE)
        {
            destroy_shader_module(device, fragment_shader, nullptr);
        }
        if (vertex_shader != VK_NULL_HANDLE)
        {
            destroy_shader_module(device, vertex_shader, nullptr);
        }
        return ok;
    }
}

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

    const auto write_timestamp2 = reinterpret_cast<PFN_vkCmdWriteTimestamp2>(get_instance_proc(nullptr, "vkCmdWriteTimestamp2"));
    const auto write_timestamp2_khr = reinterpret_cast<PFN_vkCmdWriteTimestamp2KHR>(get_instance_proc(nullptr, "vkCmdWriteTimestamp2KHR"));
    const auto draw_indirect_count_khr =
        reinterpret_cast<PFN_vkCmdDrawIndirectCountKHR>(get_instance_proc(nullptr, "vkCmdDrawIndirectCountKHR"));
    const auto draw_indexed_indirect_count_khr =
        reinterpret_cast<PFN_vkCmdDrawIndexedIndirectCountKHR>(get_instance_proc(nullptr, "vkCmdDrawIndexedIndirectCountKHR"));
    const auto get_time_domains_khr = reinterpret_cast<PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR>(
        get_instance_proc(nullptr, "vkGetPhysicalDeviceCalibrateableTimeDomainsKHR"));
    const auto get_time_domains_ext = reinterpret_cast<PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT>(
        get_instance_proc(nullptr, "vkGetPhysicalDeviceCalibrateableTimeDomainsEXT"));
    const auto get_calibrated_timestamps_khr =
        reinterpret_cast<PFN_vkGetCalibratedTimestampsKHR>(get_instance_proc(nullptr, "vkGetCalibratedTimestampsKHR"));
    const auto get_calibrated_timestamps_ext =
        reinterpret_cast<PFN_vkGetCalibratedTimestampsEXT>(get_instance_proc(nullptr, "vkGetCalibratedTimestampsEXT"));
    if (!write_timestamp2 || !write_timestamp2_khr || !draw_indirect_count_khr || !draw_indexed_indirect_count_khr ||
        !get_time_domains_khr || !get_time_domains_ext || !get_calibrated_timestamps_khr || !get_calibrated_timestamps_ext)
    {
        std::printf("[shim-test] promoted command alias missing\n");
        return 3;
    }

    bool timestamp2_test_ok = true;
    bool calibrated_timestamps_test_ok = true;

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

    const auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(get_instance_proc(instance, "vkEnumeratePhysicalDevices"));
    const auto get_properties =
        reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(get_instance_proc(instance, "vkGetPhysicalDeviceProperties"));
    const auto destroy_instance = reinterpret_cast<PFN_vkDestroyInstance>(get_instance_proc(instance, "vkDestroyInstance"));

    bool transform_feedback_test_ok = true;
    bool transform_feedback_capabilities_ok = true;
    bool shader_identifier_test_ok = true;
    bool pipeline_cache_test_ok = true;
    bool concurrent_mapped_ranges_test_ok = true;
    bool parallel_command_recording_test_ok = true;
    bool bc_formats_test_ok = true;
    bool command_ring_test_ok = true;
    bool pipeline_compile_test_ok = true;
    bool ring_failure_test_ok = true;
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
        const auto enumerate_device_extensions =
            reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(get_instance_proc(instance, "vkEnumerateDeviceExtensionProperties"));
        const auto get_features2 =
            reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(get_instance_proc(instance, "vkGetPhysicalDeviceFeatures2"));
        const auto create_device = reinterpret_cast<PFN_vkCreateDevice>(get_instance_proc(instance, "vkCreateDevice"));
        const auto get_device_queue = reinterpret_cast<PFN_vkGetDeviceQueue>(get_instance_proc(instance, "vkGetDeviceQueue"));
        const auto destroy_device = reinterpret_cast<PFN_vkDestroyDevice>(get_instance_proc(instance, "vkDestroyDevice"));
        const auto get_properties2 =
            reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(get_instance_proc(instance, "vkGetPhysicalDeviceProperties2"));

        bool transform_feedback_extension_present = false;
        bool shader_identifier_extension_present = false;
        if (enumerate_device_extensions)
        {
            uint32_t extension_count = 0;
            enumerate_device_extensions(devices[0], nullptr, &extension_count, nullptr);
            std::vector<VkExtensionProperties> extensions(extension_count);
            enumerate_device_extensions(devices[0], nullptr, &extension_count, extensions.data());
            for (const auto& extension : extensions)
            {
                if (std::strcmp(extension.extensionName, VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME) == 0)
                {
                    transform_feedback_extension_present = true;
                }
                else if (std::strcmp(extension.extensionName, VK_EXT_SHADER_MODULE_IDENTIFIER_EXTENSION_NAME) == 0)
                {
                    shader_identifier_extension_present = true;
                }
            }
        }

        VkPhysicalDeviceTransformFeedbackFeaturesEXT available_transform_feedback_features{};
        available_transform_feedback_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT;
        if (transform_feedback_extension_present && get_features2)
        {
            VkPhysicalDeviceFeatures2 features2{};
            features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            features2.pNext = &available_transform_feedback_features;
            get_features2(devices[0], &features2);
        }

        VkPhysicalDeviceTransformFeedbackPropertiesEXT available_transform_feedback_properties{};
        available_transform_feedback_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_PROPERTIES_EXT;
        if (transform_feedback_extension_present && get_properties2)
        {
            VkPhysicalDeviceProperties2 properties2{};
            properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            properties2.pNext = &available_transform_feedback_properties;
            get_properties2(devices[0], &properties2);
        }
        if (transform_feedback_extension_present)
        {
            transform_feedback_capabilities_ok =
                get_features2 && get_properties2 && available_transform_feedback_features.geometryStreams == VK_FALSE &&
                available_transform_feedback_properties.maxTransformFeedbackStreams <= 1 &&
                available_transform_feedback_properties.transformFeedbackStreamsLinesTriangles == VK_FALSE &&
                available_transform_feedback_properties.transformFeedbackRasterizationStreamSelect == VK_FALSE;
        }
        const bool transform_feedback_supported =
            transform_feedback_extension_present && available_transform_feedback_features.transformFeedback == VK_TRUE;
        std::printf("[shim-test] %s advertised=%s feature=%u capability mask=%s\n", VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME,
                    transform_feedback_extension_present ? "yes" : "no", available_transform_feedback_features.transformFeedback,
                    transform_feedback_capabilities_ok ? "PASS" : "FAIL");

        const char* calibrated_timestamps_extension = nullptr;
        if (enumerate_device_extensions)
        {
            uint32_t extension_count = 0;
            if (enumerate_device_extensions(devices[0], nullptr, &extension_count, nullptr) == VK_SUCCESS)
            {
                std::vector<VkExtensionProperties> extensions(extension_count);
                if (enumerate_device_extensions(devices[0], nullptr, &extension_count, extensions.data()) == VK_SUCCESS)
                {
                    for (const VkExtensionProperties& extension : extensions)
                    {
                        if (std::strcmp(extension.extensionName, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME) == 0)
                        {
                            calibrated_timestamps_extension = VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME;
                            break;
                        }
                        if (std::strcmp(extension.extensionName, VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME) == 0)
                        {
                            calibrated_timestamps_extension = VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME;
                        }
                    }
                }
            }
        }

        bool synchronization2_supported = false;
        if (get_features2)
        {
            VkPhysicalDeviceVulkan13Features vulkan13_features{};
            vulkan13_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
            VkPhysicalDeviceFeatures2 features2{};
            features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            features2.pNext = &vulkan13_features;
            get_features2(devices[0], &features2);
            synchronization2_supported = vulkan13_features.synchronization2 == VK_TRUE;
        }

        bool shader_identifier_supported = false;
        if (shader_identifier_extension_present && get_features2 && get_properties2)
        {
            VkPhysicalDeviceShaderModuleIdentifierFeaturesEXT identifier_features{};
            identifier_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MODULE_IDENTIFIER_FEATURES_EXT;
            VkPhysicalDeviceFeatures2 features2{};
            features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            features2.pNext = &identifier_features;
            get_features2(devices[0], &features2);

            VkPhysicalDeviceShaderModuleIdentifierPropertiesEXT identifier_properties{};
            identifier_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MODULE_IDENTIFIER_PROPERTIES_EXT;
            VkPhysicalDeviceProperties2 properties2{};
            properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            properties2.pNext = &identifier_properties;
            get_properties2(devices[0], &properties2);

            shader_identifier_supported = identifier_features.shaderModuleIdentifier == VK_TRUE;
            std::printf("[shim-test] %s advertised=%s feature=%u algorithm UUID=%02X%02X%02X%02X...\n",
                        VK_EXT_SHADER_MODULE_IDENTIFIER_EXTENSION_NAME, shader_identifier_extension_present ? "yes" : "no",
                        identifier_features.shaderModuleIdentifier, identifier_properties.shaderModuleIdentifierAlgorithmUUID[0],
                        identifier_properties.shaderModuleIdentifierAlgorithmUUID[1],
                        identifier_properties.shaderModuleIdentifierAlgorithmUUID[2],
                        identifier_properties.shaderModuleIdentifierAlgorithmUUID[3]);
        }

        uint32_t family_count = 0;
        get_queue_families(devices[0], &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        get_queue_families(devices[0], &family_count, families.data());

        uint32_t graphics_family = UINT32_MAX;
        uint32_t timestamp_family = UINT32_MAX;
        for (uint32_t i = 0; i < family_count; ++i)
        {
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && graphics_family == UINT32_MAX)
            {
                graphics_family = i;
            }
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && families[i].timestampValidBits != 0 && timestamp_family == UINT32_MAX)
            {
                timestamp_family = i;
            }
        }
        const bool timestamp2_test_supported = synchronization2_supported && timestamp_family != UINT32_MAX;
        std::printf("[shim-test] queue families=%u, graphics family=%u, timestamp family=%u\n", family_count, graphics_family,
                    timestamp_family);

        if (graphics_family != UINT32_MAX)
        {
            const float priority = 1.0f;
            std::array<VkDeviceQueueCreateInfo, 2> queue_infos{};
            queue_infos[0].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queue_infos[0].queueFamilyIndex = graphics_family;
            queue_infos[0].queueCount = 1;
            queue_infos[0].pQueuePriorities = &priority;
            uint32_t queue_info_count = 1;
            if (timestamp2_test_supported && timestamp_family != graphics_family)
            {
                queue_infos[1].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
                queue_infos[1].queueFamilyIndex = timestamp_family;
                queue_infos[1].queueCount = 1;
                queue_infos[1].pQueuePriorities = &priority;
                queue_info_count = 2;
            }

            VkPhysicalDeviceVulkan13Features enabled_vulkan13_features{};
            enabled_vulkan13_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
            enabled_vulkan13_features.synchronization2 = timestamp2_test_supported ? VK_TRUE : VK_FALSE;

            VkPhysicalDeviceShaderModuleIdentifierFeaturesEXT identifier_features{};
            identifier_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MODULE_IDENTIFIER_FEATURES_EXT;
            identifier_features.shaderModuleIdentifier = VK_TRUE;

            VkDeviceCreateInfo device_info{};
            device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
            device_info.queueCreateInfoCount = queue_info_count;
            device_info.pQueueCreateInfos = queue_infos.data();

            VkPhysicalDeviceTransformFeedbackFeaturesEXT transform_feedback_features{};
            void* enabled_feature_chain = nullptr;
            if (transform_feedback_supported)
            {
                transform_feedback_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT;
                transform_feedback_features.transformFeedback = VK_TRUE;
                transform_feedback_features.pNext = enabled_feature_chain;
                enabled_feature_chain = &transform_feedback_features;
            }
            if (shader_identifier_supported)
            {
                identifier_features.pNext = enabled_feature_chain;
                enabled_feature_chain = &identifier_features;
            }
            if (timestamp2_test_supported)
            {
                enabled_vulkan13_features.pNext = enabled_feature_chain;
                enabled_feature_chain = &enabled_vulkan13_features;
            }
            device_info.pNext = enabled_feature_chain;
            std::vector<const char*> enabled_extensions;
            if (calibrated_timestamps_extension)
            {
                enabled_extensions.push_back(calibrated_timestamps_extension);
            }
            if (transform_feedback_supported)
            {
                enabled_extensions.push_back(VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME);
            }
            if (shader_identifier_supported)
            {
                enabled_extensions.push_back(VK_EXT_SHADER_MODULE_IDENTIFIER_EXTENSION_NAME);
            }
            device_info.enabledExtensionCount = static_cast<uint32_t>(enabled_extensions.size());
            device_info.ppEnabledExtensionNames = enabled_extensions.data();

            VkDevice device = VK_NULL_HANDLE;
            const VkResult device_result = create_device(devices[0], &device_info, nullptr, &device);
            std::printf("[shim-test] vkCreateDevice -> %d, device=%p\n", device_result, static_cast<void*>(device));

            if (device_result == VK_SUCCESS && device != VK_NULL_HANDLE)
            {
                VkQueue queue = VK_NULL_HANDLE;
                get_device_queue(device, graphics_family, 0, &queue);
                std::printf("[shim-test] vkGetDeviceQueue -> queue=%p\n", static_cast<void*>(queue));

                if (timestamp2_test_supported)
                {
                    timestamp2_test_ok = test_write_timestamp2(get_instance_proc, instance, device, timestamp_family, write_timestamp2,
                                                               write_timestamp2_khr);
                }
                else
                {
                    std::printf("[shim-test] timestamp2 stage2-only recording -> SKIP (synchronization2 or timestamps unavailable)\n");
                }
                if (calibrated_timestamps_extension)
                {
                    calibrated_timestamps_test_ok =
                        test_calibrated_timestamps(get_time_domains_khr, get_time_domains_ext, get_calibrated_timestamps_khr,
                                                   get_calibrated_timestamps_ext, devices[0], device);
                }
                else
                {
                    std::printf("[shim-test] calibrated timestamps -> SKIP (extension unavailable)\n");
                }

                if (transform_feedback_supported)
                {
                    transform_feedback_test_ok = test_transform_feedback(get_instance_proc, instance, devices[0], device, graphics_family);
                }
                else
                {
                    std::printf("[shim-test] transform feedback command recording -> SKIP (feature unavailable)\n");
                }
                submit_and_wait(get_instance_proc, instance, device, queue, graphics_family);
                fill_buffer_and_readback(get_instance_proc, instance, devices[0], device, queue, graphics_family);
                command_ring_test_ok = test_command_ring(get_instance_proc, instance, devices[0], device, queue, graphics_family);
                clear_image_and_readback(get_instance_proc, instance, devices[0], device, queue, graphics_family);
                bc_formats_test_ok = test_bc_formats(get_instance_proc, instance, devices[0], device, queue, graphics_family);
                if (shader_identifier_supported)
                {
                    shader_identifier_test_ok = test_shader_module_identifier(get_instance_proc, instance, device);
                }
                pipeline_cache_test_ok = test_pipeline_cache(get_instance_proc, instance, device);
                concurrent_mapped_ranges_test_ok = test_concurrent_mapped_ranges(get_instance_proc, instance, devices[0], device);
                parallel_command_recording_test_ok =
                    test_parallel_command_recording(get_instance_proc, instance, devices[0], device, queue, graphics_family);
                pipeline_compile_test_ok = test_pipelines_compile_beside_bridge_calls(get_instance_proc, instance, device);
                ring_failure_test_ok = test_ring_failure_is_reported_once(get_instance_proc, instance, device, queue);

                destroy_device(device, nullptr);
            }
        }
    }

    if (destroy_instance)
    {
        destroy_instance(instance, nullptr);
    }

    const bool all_ok = timestamp2_test_ok && calibrated_timestamps_test_ok && transform_feedback_test_ok &&
                        transform_feedback_capabilities_ok && shader_identifier_test_ok && pipeline_cache_test_ok && bc_formats_test_ok &&
                        concurrent_mapped_ranges_test_ok && parallel_command_recording_test_ok && command_ring_test_ok &&
                        pipeline_compile_test_ok && ring_failure_test_ok;
    std::printf("[shim-test] %s\n", all_ok ? "ok" : "FAILED");
    return all_ok ? 0 : 6;
}
