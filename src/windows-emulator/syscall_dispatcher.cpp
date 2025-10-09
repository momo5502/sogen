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

    const syscall_context c{
        .win_emu = win_emu,
        .emu = emu,
        .proc = context,
        .write_status = true,
    };

    try
    {
        const auto entry = this->handlers_.find(syscall_id);
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

        if (c.write_status && !c.retrigger_syscall)
        {
            const auto status = static_cast<uint32_t>(emu.reg<uint64_t>(x86_register::rax));
            win_emu.log.print(color::dark_gray, "Syscall return: %s (0x%X, raw: 0x%X) -> 0x%08X\n", entry->second.name.c_str(), syscall_id,
                              raw_syscall_id, status);
        }
    }
    catch (std::exception& e)
    {
        win_emu.log.error("Syscall threw an exception: %X (raw: %X) (0x%" PRIx64 ") - %s\n", syscall_id, raw_syscall_id, address, e.what());
        emu.reg<uint64_t>(x86_register::rax, STATUS_UNSUCCESSFUL);
        emu.stop();
    }
    catch (...)
    {
        win_emu.log.error("Syscall threw an unknown exception: %X (raw: %X) (0x%" PRIx64 ")\n", syscall_id, raw_syscall_id, address);
        emu.reg<uint64_t>(x86_register::rax, STATUS_UNSUCCESSFUL);
        emu.stop();
    }
}

syscall_dispatcher::syscall_dispatcher(const exported_symbols& ntdll_exports, const std::span<const std::byte> ntdll_data,
                                       const exported_symbols& win32u_exports, const std::span<const std::byte> win32u_data)
{
    this->setup(ntdll_exports, ntdll_data, win32u_exports, win32u_data);
}
