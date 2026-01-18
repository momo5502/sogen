#include "std_include.hpp"
#include "syscall_dispatcher.hpp"
#include "syscall_utils.hpp"

static void serialize(utils::buffer_serializer& buffer, const syscall_handler_entry& obj)
{
    buffer.write(obj.name);
}

static void deserialize(utils::buffer_deserializer& buffer, syscall_handler_entry& obj)
{
    buffer.read(obj.name);
    obj.handler = nullptr;
}

void syscall_dispatcher::serialize(utils::buffer_serializer& buffer) const
{
    buffer.write_map(this->handlers_);
}

void syscall_dispatcher::deserialize(utils::buffer_deserializer& buffer)
{
    buffer.read_map(this->handlers_);
    this->add_handlers();
    this->add_callbacks();
}

void syscall_dispatcher::setup(const exported_symbols& ntdll_exports, const std::span<const std::byte> ntdll_data,
                               const exported_symbols& win32u_exports, const std::span<const std::byte> win32u_data)
{
    this->handlers_ = {};

    const auto ntdll_syscalls = find_syscalls(ntdll_exports, ntdll_data);
    const auto win32u_syscalls = find_syscalls(win32u_exports, win32u_data);

    map_syscalls(this->handlers_, ntdll_syscalls);
    map_syscalls(this->handlers_, win32u_syscalls);

    this->add_handlers();
    this->add_callbacks();
}

void syscall_dispatcher::add_handlers()
{
    std::map<std::string, syscall_handler> handler_mapping{};
    syscall_dispatcher::add_handlers(handler_mapping);

    for (auto& entry : this->handlers_ | std::views::values)
    {
        const auto handler = handler_mapping.find(entry.name);
        if (handler == handler_mapping.end())
        {
            continue;
        }

        entry.handler = handler->second;

#ifndef NDEBUG
        handler_mapping.erase(handler);
#endif
    }
}

void syscall_dispatcher::dispatch(windows_emulator& win_emu)
{
    auto& emu = win_emu.emu();
    auto& context = win_emu.process;

    const auto address = emu.read_instruction_pointer();
    const auto raw_syscall_id = emu.reg<uint32_t>(x86_register::eax);
    const auto syscall_id = raw_syscall_id & 0xFFFF; // Only take low bits for WOW64 compatibility

    const auto entry = this->handlers_.find(syscall_id);
    const auto* syscall_name = (entry != this->handlers_.end()) ? entry->second.name.c_str() : "<unknown>";

    const syscall_context c{
        .win_emu = win_emu,
        .emu = emu,
        .proc = context,
        .write_status = true,
    };

    try
    {
        if (entry == this->handlers_.end())
        {
            win_emu.log.error("Unknown syscall: 0x%X (raw: 0x%X)\n", syscall_id, raw_syscall_id);
            c.emu.reg<uint64_t>(x86_register::rax, STATUS_NOT_SUPPORTED);
            c.emu.stop();
            return;
        }

        const auto res = win_emu.callbacks.on_syscall(syscall_id, entry->second.name);
        if (res == instruction_hook_continuation::skip_instruction)
        {
            return;
        }

        if (!entry->second.handler)
        {
            win_emu.log.error("Unimplemented syscall: %s - 0x%X (raw: 0x%X)\n", entry->second.name.c_str(), syscall_id, raw_syscall_id);
            c.emu.reg<uint64_t>(x86_register::rax, STATUS_NOT_SUPPORTED);
            c.emu.stop();
            return;
        }

        entry->second.handler(c);

        dispatch_callback(win_emu, entry->second.name);
    }
    catch (std::exception& e)
    {
        win_emu.log.error("Syscall %s threw an exception: 0x%X (raw: 0x%X) (0x%" PRIx64 ") - %s\n", syscall_name, syscall_id,
                          raw_syscall_id, address, e.what());
        emu.reg<uint64_t>(x86_register::rax, STATUS_UNSUCCESSFUL);
        emu.stop();
    }
    catch (...)
    {
        win_emu.log.error("Syscall %s threw an unknown exception: 0x%X (raw: 0x%X) (0x%" PRIx64 ")\n", syscall_name, syscall_id,
                          raw_syscall_id, address);
        emu.reg<uint64_t>(x86_register::rax, STATUS_UNSUCCESSFUL);
        emu.stop();
    }
}

void syscall_dispatcher::dispatch_callback(windows_emulator& win_emu, std::string& syscall_name)
{
    auto& emu = win_emu.emu();
    auto& context = win_emu.process;

    if (context.instrumentation_callback != 0 && syscall_name != "NtContinue")
    {
        auto rip_old = emu.reg<uint64_t>(x86_register::rip);

        // The increase in RIP caused by executing the syscall here has not yet occurred.
        // If RIP is set directly, it will lead to an incorrect address, so the length of
        // the syscall instruction needs to be subtracted.
        emu.reg<uint64_t>(x86_register::rip, context.instrumentation_callback - 2);

        emu.reg<uint64_t>(x86_register::r10, rip_old);
    }
}

dispatch_result syscall_dispatcher::dispatch_completion(windows_emulator& win_emu, callback_id callback_id,
                                                        completion_state* completion_state, uint64_t callback_result)
{
    auto& emu = win_emu.emu();

    const syscall_context c{.win_emu = win_emu,
                            .emu = emu,
                            .proc = win_emu.process,
                            .write_status = true,
                            .is_callback_completion = true,
                            .current_completion_state = completion_state,
                            .previous_callback_result = callback_result};

    const auto entry = this->completion_handlers_.find(callback_id);

    if (entry == this->completion_handlers_.end())
    {
        win_emu.log.error("Unknown callback: 0x%X\n", static_cast<uint32_t>(callback_id));
        c.emu.stop();
        return dispatch_result::error;
    }

    try
    {
        entry->second(c);
        return c.run_callback ? dispatch_result::new_callback : dispatch_result::completed;
    }
    catch (std::exception& e)
    {
        win_emu.log.error("Completion for callback 0x%X threw an exception - %s\n", static_cast<int>(callback_id), e.what());
        emu.stop();
        return dispatch_result::error;
    }
    catch (...)
    {
        win_emu.log.error("Completion for callback 0x%X threw an unknown exception\n", static_cast<int>(callback_id));
        emu.stop();
        return dispatch_result::error;
    }
}

syscall_dispatcher::syscall_dispatcher(const exported_symbols& ntdll_exports, const std::span<const std::byte> ntdll_data,
                                       const exported_symbols& win32u_exports, const std::span<const std::byte> win32u_data)
{
    this->setup(ntdll_exports, ntdll_data, win32u_exports, win32u_data);
}

std::map<callback_id, std::function<std::unique_ptr<completion_state>()>> syscall_dispatcher::completion_state_factories_{};
