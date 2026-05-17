#pragma once

#include "syscall_utils.hpp"

// TODO: Here we are calling guest functions directly, but this is not how it works in the real Windows kernel.
//       In the real implementation, the kernel invokes ntdll!KiUserCallbackDispatcher and passes a callback
//       index that refers to an entry in PEB->KernelCallbackTable. The dispatcher then looks up the function
//       pointer in that table and invokes the corresponding user-mode callback.
//       See Also: https://web.archive.org/web/20080717175308/http://www.nynaeve.net/?p=204

template <typename... Args>
void prepare_call_stack(x86_64_emulator& emu, uint64_t return_address, Args... args)
{
    constexpr size_t arg_count = sizeof...(Args);
    const size_t stack_args_size = aligned_stack_space(arg_count);

    const uint64_t current_rsp = emu.read_stack_pointer();
    const uint64_t aligned_rsp = align_down(current_rsp, 16);

    // We subtract the args size (including the shadow space) AND the size of the return address
    const uint64_t new_rsp = aligned_rsp - stack_args_size - sizeof(emulator_pointer);
    emu.reg(x86_register::rsp, new_rsp);

    emu.write_memory(new_rsp, &return_address, sizeof(return_address));

    size_t index = 0;
    (set_function_argument(emu, index++, static_cast<uint64_t>(args)), ...);
}

template <typename StateT, typename... Args>
    requires(std::derived_from<std::remove_reference_t<StateT>, completion_state> ||
             std::same_as<std::remove_reference_t<StateT>, std::nullptr_t>)
void dispatch_user_callback(const syscall_context& c, callback_id completion_id, StateT&& state_obj, uint64_t func_address, Args... args)
{
    if (c.run_callback)
    {
        throw std::runtime_error("A callback has already been dispatched");
    }

    std::unique_ptr<completion_state> state;

    if constexpr (std::same_as<std::remove_reference_t<StateT>, std::nullptr_t>)
    {
        state = nullptr;
    }
    else
    {
        state = std::make_unique<std::remove_reference_t<StateT>>(std::forward<StateT>(state_obj));
    }

    callback_frame frame(completion_id, std::move(state));
    frame.save_registers(c.emu);
    c.proc.active_thread->callback_return_rax.reset();
    c.proc.active_thread->callback_stack.emplace_back(std::move(frame));

    prepare_call_stack(c.emu, c.proc.zw_callback_return, args...);

    c.emu.reg(x86_register::rip, func_address);
    c.run_callback = true;
}

template <typename... Args>
void dispatch_user_callback(const syscall_context& c, callback_id completion_id, uint64_t func_address, Args... args)
{
    dispatch_user_callback(c, completion_id, nullptr, func_address, args...);
}
