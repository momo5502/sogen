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

void syscall_dispatcher::setup(const exported_symbols& ntdll_exports, std::span<const std::byte> ntdll_data,
                               const exported_symbols& win32u_exports, std::span<const std::byte> win32u_data)
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

    for (auto& entry : this->handlers_)
    {
        const auto handler = handler_mapping.find(entry.second.name);
        if (handler == handler_mapping.end())
        {
            continue;
        }

        entry.second.handler = handler->second;

#ifndef NDEBUG
        handler_mapping.erase(handler);
#endif
    }
}

void syscall_dispatcher::dispatch(windows_emulator& win_emu)
{
    auto& emu = win_emu.emu();
    auto& context = win_emu.process();

    const auto address = emu.read_instruction_pointer();
    const auto syscall_id = emu.reg<uint32_t>(x64_register::eax);

    const syscall_context c{win_emu, emu, context, true};

    try
    {
        const auto entry = this->handlers_.find(syscall_id);
        if (entry == this->handlers_.end())
        {
            win_emu.log.error("Unknown syscall: 0x%X\n", syscall_id);
            c.emu.reg<uint64_t>(x64_register::rax, STATUS_NOT_SUPPORTED);
            c.emu.stop();
            return;
        }

        if (!entry->second.handler)
        {
            win_emu.log.error("Unimplemented syscall: %s - 0x%X\n", entry->second.name.c_str(), syscall_id);
            c.emu.reg<uint64_t>(x64_register::rax, STATUS_NOT_SUPPORTED);
            c.emu.stop();
            return;
        }

        const auto* mod = context.mod_manager.find_by_address(address);
        if (mod != context.ntdll && mod != context.win32u)
        {
            win_emu.on_inline_syscall(syscall_id, address, mod ? mod->name.c_str() : "<N/A>",
                                      entry->second.name.c_str());
            win_emu.log.print(color::blue, "Executing inline syscall: %s (0x%X) at 0x%" PRIx64 " (%s)\n",
                              entry->second.name.c_str(), syscall_id, address, mod ? mod->name.c_str() : "<N/A>");
        }
        else
        {
            if (mod->is_within(context.previous_ip))
            {
                const auto rsp = c.emu.read_stack_pointer();
                const auto return_address = c.emu.read_memory<uint64_t>(rsp);
                const auto* mod_name = context.mod_manager.find_name(return_address);

                win_emu.log.print(color::dark_gray,
                                  "Executing syscall: %s (0x%X) at 0x%" PRIx64 " via 0x%" PRIx64 " (%s)\n",
                                  entry->second.name.c_str(), syscall_id, address, return_address, mod_name);
            }
            else
            {
                const auto* previous_mod = context.mod_manager.find_by_address(context.previous_ip);
                win_emu.on_outofline_syscall(syscall_id, address, mod ? mod->name.c_str() : "<N/A>",
                                             entry->second.name.c_str(), context.previous_ip,
                                             previous_mod ? previous_mod->name.c_str() : "<N/A>");
                win_emu.log.print(color::blue,
                                  "Crafted out-of-line syscall: %s (0x%X) at 0x%" PRIx64 " (%s) via 0x%" PRIx64
                                  " (%s)\n",
                                  entry->second.name.c_str(), syscall_id, address, mod ? mod->name.c_str() : "<N/A>",
                                  context.previous_ip, previous_mod ? previous_mod->name.c_str() : "<N/A>");
            }
        }

        entry->second.handler(c);
    }
    catch (std::exception& e)
    {
        win_emu.log.error("Syscall threw an exception: %X (0x%" PRIx64 ") - %s\n", syscall_id, address, e.what());
        emu.reg<uint64_t>(x64_register::rax, STATUS_UNSUCCESSFUL);
        emu.stop();
    }
    catch (...)
    {
        win_emu.log.error("Syscall threw an unknown exception: %X (0x%" PRIx64 ")\n", syscall_id, address);
        emu.reg<uint64_t>(x64_register::rax, STATUS_UNSUCCESSFUL);
        emu.stop();
    }
}

syscall_dispatcher::syscall_dispatcher(const exported_symbols& ntdll_exports, std::span<const std::byte> ntdll_data,
                                       const exported_symbols& win32u_exports, std::span<const std::byte> win32u_data)
{
    this->setup(ntdll_exports, ntdll_data, win32u_exports, win32u_data);
}
