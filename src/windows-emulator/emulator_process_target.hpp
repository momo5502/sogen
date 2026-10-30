#pragma once

#include "windows_emulator.hpp"

#include "process_manager.hpp"
#include "thread_context.hpp"

#include <limits>

namespace sogen
{
    // What VirtualQuery reports for the region at address, including the file mapped there.
    process_memory_region describe_memory_region(windows_emulator& win_emu, uint64_t address);

    class emulator_process_target final : public managed_process_target
    {
        windows_emulator& emulator_;

        // The emulation thread does the work; see debug_port.
        template <typename Function>
        auto access_emulator(Function&& function) const
        {
            std::optional<decltype(function())> result{};
            this->emulator_.debugger().run_on_emulation_thread(this->emulator_, [&] { result.emplace(function()); });
            return std::move(*result);
        }

      public:
        explicit emulator_process_target(windows_emulator& emulator)
            : emulator_(emulator)
        {
        }

        uint64_t native_environment() const override
        {
            return this->emulator_.process.peb64.value();
        }

        uint64_t compatibility_environment() const override
        {
            return this->emulator_.process.peb32 ? this->emulator_.process.peb32->value() : 0;
        }

        uint64_t native_parameters() const override
        {
            return this->emulator_.process.process_params64.value();
        }

        uint64_t compatibility_parameters() const override
        {
            return this->emulator_.process.process_params32 ? this->emulator_.process.process_params32->value() : 0;
        }

        process_memory_result allocate_memory(const uint64_t address, const uint64_t size, const memory_permission permission,
                                              const bool reserve, const bool commit) override
        {
            return this->access_emulator([&] {
                if (size > std::numeric_limits<size_t>::max())
                {
                    return process_memory_result{.error = process_error::resource_limit};
                }

                const nt_memory_permission emulator_permission{permission};
                uint64_t result = address;
                if (reserve)
                {
                    if (address)
                    {
                        result = this->emulator_.memory.allocate_memory(address, static_cast<size_t>(size), emulator_permission, !commit)
                                     ? address
                                     : 0;
                    }
                    else
                    {
                        result = this->emulator_.memory.allocate_memory(static_cast<size_t>(size), emulator_permission, !commit);
                    }
                }
                else if (commit && address && this->emulator_.memory.commit_memory(address, static_cast<size_t>(size), emulator_permission))
                {
                    result = address;
                }
                else if (commit)
                {
                    if (address)
                    {
                        result =
                            this->emulator_.memory.allocate_memory(address, static_cast<size_t>(size), emulator_permission) ? address : 0;
                    }
                    else
                    {
                        result = this->emulator_.memory.allocate_memory(static_cast<size_t>(size), emulator_permission);
                    }
                }
                else
                {
                    result = 0;
                }

                return result ? process_memory_result{.address = result, .size = size}
                              : process_memory_result{.error = process_error::unavailable};
            });
        }

        process_error free_memory(const uint64_t address, const uint64_t size, const bool release) override
        {
            return this->access_emulator([&] {
                if (size > std::numeric_limits<size_t>::max())
                {
                    return process_error::resource_limit;
                }
                const auto success = release ? this->emulator_.memory.release_memory(address, static_cast<size_t>(size))
                                             : this->emulator_.memory.decommit_memory(address, static_cast<size_t>(size));
                return success ? process_error::none : process_error::unavailable;
            });
        }

        process_memory_result protect_memory(const uint64_t address, const uint64_t size, const memory_permission permission) override
        {
            return this->access_emulator([&] {
                if (size > std::numeric_limits<size_t>::max())
                {
                    return process_memory_result{.error = process_error::resource_limit};
                }
                nt_memory_permission old_permission{};
                if (!this->emulator_.memory.protect_memory(address, static_cast<size_t>(size), nt_memory_permission{permission},
                                                           &old_permission))
                {
                    return process_memory_result{.error = process_error::unavailable};
                }
                return process_memory_result{.address = address, .size = size, .permission = old_permission.flattened()};
            });
        }

        process_memory_read_result read_memory(const uint64_t address, const uint64_t size) const override
        {
            return this->access_emulator([&] {
                if (size > std::numeric_limits<size_t>::max())
                {
                    return process_memory_read_result{.error = process_error::resource_limit};
                }
                process_memory_read_result result{};
                result.data.resize(static_cast<size_t>(size));
                if (!this->emulator_.memory.try_read_memory(address, result.data.data(), result.data.size()))
                {
                    return process_memory_read_result{.error = process_error::unavailable};
                }
                return result;
            });
        }

        process_memory_result write_memory(const uint64_t address, const std::span<const uint8_t> data) override
        {
            return this->access_emulator([&] {
                if (!this->emulator_.memory.try_write_memory(address, data.data(), data.size()))
                {
                    return process_memory_result{.error = process_error::unavailable};
                }
                return process_memory_result{.address = address, .size = data.size()};
            });
        }

        process_debug_event_result next_debug_event() override
        {
            return {.event = this->emulator_.debugger().take_event()};
        }

        process_error continue_debug_event(const uint32_t thread_id, const uint32_t status) override
        {
            return this->emulator_.debugger().continue_event(thread_id, status) ? process_error::none : process_error::unavailable;
        }

        process_error detach_debugger() override
        {
            this->emulator_.detach_debugger();
            return process_error::none;
        }

        process_thread_context_result get_thread_context(const uint32_t thread_id, const bool wow64) override
        {
            process_thread_context_result result{.error = process_error::invalid_process};
            this->on_thread(thread_id, [&](emulator_thread& thread) {
                auto& vcpu = this->emulator_.vcpu(0);
                result.context = wow64 ? capture_wow64_thread_context(this->emulator_, vcpu, thread)
                                       : capture_thread_context(this->emulator_, vcpu, thread);
                result.error = result.context.empty() ? process_error::unavailable : process_error::none;
            });
            return result;
        }

        process_error set_thread_context(const uint32_t thread_id, const std::span<const uint8_t> context, const bool wow64) override
        {
            auto error = process_error::invalid_process;
            this->on_thread(thread_id, [&](emulator_thread& thread) {
                auto& vcpu = this->emulator_.vcpu(0);
                const auto applied = wow64 ? apply_wow64_thread_context(this->emulator_, vcpu, thread, context)
                                           : apply_thread_context(this->emulator_, vcpu, thread, context);
                error = applied ? process_error::none : process_error::unavailable;
            });
            return error;
        }

        process_thread_information thread_information(const uint32_t thread_id) override
        {
            process_thread_information result{.error = process_error::invalid_process};
            this->on_thread(thread_id, [&](emulator_thread& thread) {
                if (thread.teb64)
                {
                    result = {.teb = thread.teb64->value()};
                }
            });
            return result;
        }

        process_suspend_result suspend_thread(const uint32_t thread_id, const bool resume) override
        {
            return this->access_emulator([&] {
                auto* thread = this->find_thread(thread_id);
                if (!thread)
                {
                    return process_suspend_result{.error = process_error::invalid_process};
                }

                const auto previous = thread->suspended;
                if (resume)
                {
                    thread->suspended = previous > 0 ? previous - 1 : 0;
                }
                else
                {
                    ++thread->suspended;
                }
                return process_suspend_result{.previous_count = previous};
            });
        }

        process_memory_region query_memory(const uint64_t address) const override
        {
            return this->access_emulator([&] { return describe_memory_region(this->emulator_, address); });
        }

        process_object_install_result install_object(const process_shared_object& object) override
        {
            return this->access_emulator([&]() -> process_object_install_result {
                const auto installed = this->emulator_.process.install_shared_object(object);
                if (!installed)
                {
                    return {.error = process_error::internal_failure};
                }
                return {.handle = installed->bits};
            });
        }

        process_object_export_result export_object(const uint64_t handle) override
        {
            return this->access_emulator([&]() -> process_object_export_result {
                auto shared =
                    this->emulator_.process.share_object(make_handle(handle), this->emulator_.memory.can_alias_host_memory_anywhere());
                if (!shared)
                {
                    return {.error = process_error::unavailable};
                }
                return {.object = std::move(*shared)};
            });
        }

        process_error set_thread_token(const uint32_t thread_id, const uint64_t token) override
        {
            const handle impersonation_token{.bits = token};
            if (impersonation_token != 0 && impersonation_token != DUMMY_IMPERSONATION_TOKEN &&
                impersonation_token != ANONYMOUS_IMPERSONATION_TOKEN)
            {
                return process_error::internal_failure;
            }

            auto error = process_error::invalid_process;
            this->on_thread(thread_id, [&](emulator_thread& thread) {
                thread.set_impersonation_token(impersonation_token);
                error = process_error::none;
            });
            return error;
        }

        process_thread_token_result get_thread_token(const uint32_t thread_id) override
        {
            process_thread_token_result result{.error = process_error::invalid_process};
            this->on_thread(thread_id, [&](const emulator_thread& thread) { result = {.token = thread.impersonation_token.bits}; });
            return result;
        }

        process_cycle_time_result cycle_time() const override
        {
            return this->access_emulator(
                [&] { return process_cycle_time_result{.cycles = this->emulator_.process.get_accumulated_cycles()}; });
        }

        process_error close_handle(const uint64_t handle) override
        {
            return this->access_emulator(
                [&] { return this->emulator_.close_handle(make_handle(handle)) ? process_error::none : process_error::invalid_process; });
        }

      private:
        emulator_thread* find_thread(const uint32_t thread_id) const
        {
            for (auto& thread : this->emulator_.process.threads | std::views::values)
            {
                if (thread.id == thread_id)
                {
                    return &thread;
                }
            }
            return nullptr;
        }

        template <typename Access>
        void on_thread(const uint32_t thread_id, Access&& access)
        {
            this->emulator_.debugger().run_on_emulation_thread(this->emulator_, [&] {
                if (auto* thread = this->find_thread(thread_id))
                {
                    access(*thread);
                }
            });
        }
    };
}
