#include "../std_include.hpp"
#include "registry_notification.hpp"
#include "../io_completion_wait.hpp"
#include "../syscall_utils.hpp"
#include "../windows_emulator.hpp"

namespace sogen
{
    namespace syscalls
    {
        NTSTATUS handle_NtSetEvent(const syscall_context& c, uint64_t event_handle, emulator_object<LONG> previous_state);
        NTSTATUS handle_NtClearEvent(const syscall_context& c, handle event_handle);
    }

    namespace registry_notification
    {
        namespace
        {
            std::filesystem::path get_key_path(const registry_key& key)
            {
                return utils::path_key{key.hive.get() / key.path.get()}.get();
            }

            bool is_watched(const std::filesystem::path& watched_path, const bool watch_tree, const std::filesystem::path& changed_path)
            {
                auto changed_component = changed_path.begin();
                for (const auto& watched_component : watched_path)
                {
                    if (changed_component == changed_path.end() || *changed_component != watched_component)
                    {
                        return false;
                    }

                    ++changed_component;
                }

                return changed_component == changed_path.end() || watch_tree;
            }

            size_t get_io_status_block_size(const syscall_context& c)
            {
                return c.proc.is_wow64_process ? sizeof(IO_STATUS_BLOCK<EmulatorTraits<Emu32>>)
                                               : sizeof(IO_STATUS_BLOCK<EmulatorTraits<Emu64>>);
            }

            bool is_writable_memory(const syscall_context& c, const uint64_t address, const size_t size)
            {
                const auto end = address + size;
                if (address == 0 || end < address)
                {
                    return false;
                }

                for (auto current = address; current < end;)
                {
                    const auto region = c.win_emu.memory.get_region_info(current);
                    if (!region.is_committed || !is_writable(region.permissions.common))
                    {
                        return false;
                    }

                    current = region.start + region.length;
                }

                return true;
            }

            // Windows writes only the low 32 bits of the native Status field.
            void write_io_status(const syscall_context& c, const uint64_t io_status_block, const NTSTATUS status)
            {
                const auto status32 = static_cast<uint32_t>(status);
                if (c.proc.is_wow64_process)
                {
                    const IO_STATUS_BLOCK<EmulatorTraits<Emu32>> block{.Status = status32, .Information = 0};
                    c.win_emu.memory.try_write_memory(io_status_block, &block, sizeof(block));
                    return;
                }

                constexpr EmulatorTraits<Emu64>::ULONG_PTR information = 0;
                c.win_emu.memory.try_write_memory(io_status_block, &status32, sizeof(status32));
                c.win_emu.memory.try_write_memory(io_status_block + offsetof(IO_STATUS_BLOCK<EmulatorTraits<Emu64>>, Information),
                                                  &information, sizeof(information));
            }

            void release(const syscall_context& c, registry_notify_request& request)
            {
                io_completion_wait::release_handle_reference(c.proc, request.event);
            }

            void complete(const syscall_context& c, registry_notify_request& request, const NTSTATUS status)
            {
                if (request.io_status_block)
                {
                    write_io_status(c, request.io_status_block, status);
                }

                if (request.event.bits != 0)
                {
                    syscalls::handle_NtSetEvent(c, request.event.bits, emulator_object<LONG>{c.emu.memory()});
                }

                if (request.apc_routine)
                {
                    if (auto* thread = c.proc.find_thread_by_id(request.thread_id))
                    {
                        thread->pending_apcs.push_back({
                            .apc_routine = request.apc_routine,
                            .apc_argument1 = request.apc_context,
                            .apc_argument2 = request.io_status_block,
                        });
                    }
                }

                release(c, request);
                c.win_emu.wake_idle_vcpu();
            }

            // A synchronous request ends without completing when an APC interrupts its wait.
            bool is_abandoned(const syscall_context& c, const registry_notify_request& request)
            {
                if (!request.synchronous)
                {
                    return false;
                }

                const auto* thread = c.proc.find_thread_by_id(request.thread_id);
                return !thread || thread->ended_waits != request.thread_ended_waits;
            }

            void drop_abandoned_requests(const syscall_context& c, registry_notify_block& block)
            {
                std::erase_if(block.requests, [&](registry_notify_request& request) {
                    if (!is_abandoned(c, request))
                    {
                        return false;
                    }

                    release(c, request);
                    return true;
                });
            }

            void complete_all(const syscall_context& c, registry_notify_block& block, const NTSTATUS status)
            {
                for (auto& request : block.requests)
                {
                    complete(c, request, status);
                }

                block.requests.clear();
            }

            void report_change(const syscall_context& c, const std::filesystem::path& changed_path, const ULONG filter)
            {
                for (auto& key : c.proc.registry_keys | std::views::values)
                {
                    auto& block = key.notify_block;
                    if (!block || (block->filter & filter) == 0 || !is_watched(get_key_path(key), block->watch_tree, changed_path))
                    {
                        continue;
                    }

                    drop_abandoned_requests(c, *block);
                    if (block->requests.empty())
                    {
                        block->change_pending = true;
                    }
                    else
                    {
                        complete_all(c, *block, STATUS_NOTIFY_ENUM_DIR);
                    }
                }
            }
        }

        bool has_watchers(const syscall_context& c)
        {
            return std::ranges::any_of(c.proc.registry_keys | std::views::values,
                                       [](const registry_key& key) { return key.notify_block.has_value(); });
        }

        bool write_pending_status(const syscall_context& c, const uint64_t io_status_block)
        {
            if (!is_writable_memory(c, io_status_block, get_io_status_block_size(c)))
            {
                return false;
            }

            write_io_status(c, io_status_block, STATUS_PENDING);
            return true;
        }

        NTSTATUS watch(const syscall_context& c, registry_key& key, registry_notify_request request, const ULONG filter,
                       const bool watch_tree)
        {
            if (request.event.bits != 0)
            {
                if (!io_completion_wait::retain_handle_reference(c.proc, c.vcpu.active_thread, request.event, request.event))
                {
                    return STATUS_INVALID_HANDLE;
                }

                syscalls::handle_NtClearEvent(c, request.event);
            }

            if (!key.notify_block)
            {
                key.notify_block = registry_notify_block{.filter = filter, .watch_tree = watch_tree};
            }

            drop_abandoned_requests(c, *key.notify_block);

            if (key.notify_block->change_pending)
            {
                key.notify_block->change_pending = false;
                complete(c, request, STATUS_NOTIFY_ENUM_DIR);
                return request.synchronous ? STATUS_NOTIFY_ENUM_DIR : STATUS_SUCCESS;
            }

            key.notify_block->requests.push_back(request);
            return STATUS_PENDING;
        }

        void report_value_change(const syscall_context& c, const registry_key& key)
        {
            report_change(c, get_key_path(key), reg_notify_change_last_set);
        }

        void report_sub_key_change(const syscall_context& c, const registry_key& sub_key)
        {
            report_change(c, get_key_path(sub_key).parent_path(), reg_notify_change_name);
        }

        void close(const syscall_context& c, registry_key& key)
        {
            if (key.notify_block)
            {
                drop_abandoned_requests(c, *key.notify_block);
                complete_all(c, *key.notify_block, STATUS_NOTIFY_CLEANUP);
            }
        }

        void end_deleted_key_requests(const syscall_context& c)
        {
            for (auto& key : c.proc.registry_keys | std::views::values)
            {
                if (key.notify_block && !c.win_emu.registry.key_exists(key))
                {
                    close(c, key);
                }
            }
        }

        void end_thread_requests(const syscall_context& c, const uint32_t thread_id)
        {
            for (auto& key : c.proc.registry_keys | std::views::values)
            {
                if (!key.notify_block)
                {
                    continue;
                }

                std::erase_if(key.notify_block->requests, [&](registry_notify_request& request) {
                    if (request.thread_id != thread_id || request.thread_agnostic)
                    {
                        return false;
                    }

                    if (request.synchronous)
                    {
                        release(c, request);
                    }
                    else
                    {
                        complete(c, request, STATUS_NOTIFY_CLEANUP);
                    }

                    return true;
                });
            }
        }
    }
} // namespace sogen
