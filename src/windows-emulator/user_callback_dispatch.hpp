#pragma once

#include "syscall_utils.hpp"

namespace sogen
{

    template <typename... Args>
    constexpr uint32_t user_callback_args_size()
    {
        size_t offset = 0;
        ((offset = static_cast<size_t>(align_up(offset, alignof(Args))), offset += sizeof(Args)), ...);
        return static_cast<uint32_t>(offset);
    }

    template <typename T>
    void write_user_callback_arg(x86_64_cpu& emu, const uint64_t arg_buffer, size_t& offset, const T& arg)
    {
        offset = static_cast<size_t>(align_up(offset, alignof(T)));
        emu.write_memory(arg_buffer + offset, &arg, sizeof(arg));
        offset += sizeof(arg);
    }

    template <typename StateT>
        requires(std::derived_from<std::remove_reference_t<StateT>, completion_state> ||
                 std::same_as<std::remove_reference_t<StateT>, std::nullptr_t>)
    void push_callback_frame(const syscall_context& c, callback_id completion_id, StateT&& state_obj)
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

        auto& thread = c.thread();
        thread.callback_return_rax.reset();
        thread.callback_stack.emplace_back(std::move(frame));
    }

    template <typename... Args>
    void prepare_call_stack(x86_64_cpu& emu, const uint32_t callback_index, const Args&... args)
    {
        const uint32_t arg_length = user_callback_args_size<Args...>();
        const uint64_t stack_args_size = align_up(arg_length, 0x10);
        const uint64_t current_rsp = emu.read_stack_pointer();
        const uint64_t aligned_rsp = align_down(current_rsp, 16);

        // KiUserCallbackDispatcher expects 0x20 bytes of shadow space plus the arg buffer pointer,
        // arg length, and callback index stored in the 0x10 bytes above it.
        const uint64_t new_rsp = aligned_rsp - 0x30 - stack_args_size;
        const uint64_t arg_buffer = new_rsp + 0x30;

        emu.reg(x86_register::rsp, new_rsp);

        if constexpr (sizeof...(Args) > 0)
        {
            size_t offset = 0;
            (write_user_callback_arg(emu, arg_buffer, offset, args), ...);
        }

        emu.write_memory(new_rsp + 0x20, &arg_buffer, sizeof(arg_buffer));
        emu.write_memory(new_rsp + 0x28, &arg_length, sizeof(arg_length));
        emu.write_memory(new_rsp + 0x2C, &callback_index, sizeof(callback_index));
    }

    template <typename StateT, typename... Args>
        requires(std::derived_from<std::remove_reference_t<StateT>, completion_state> ||
                 std::same_as<std::remove_reference_t<StateT>, std::nullptr_t>)
    void dispatch_user_callback(const syscall_context& c, const callback_id completion_id, const uint32_t callback_index,
                                StateT&& state_obj, const Args&... args)
    {
        push_callback_frame(c, completion_id, std::forward<StateT>(state_obj));

        prepare_call_stack(c.emu, callback_index, args...);

        c.emu.reg(x86_register::rip, c.proc.ki_user_callback_dispatcher);
        c.run_callback = true;
    }

    template <typename... Args>
    void dispatch_user_callback(const syscall_context& c, const callback_id completion_id, const uint32_t callback_index,
                                const Args&... args)
    {
        dispatch_user_callback(c, completion_id, callback_index, nullptr, args...);
    }

} // namespace sogen
