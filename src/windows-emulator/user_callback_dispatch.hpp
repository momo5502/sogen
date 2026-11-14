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
    void write_user_callback_arg(guest_cpu& emu, const uint64_t arg_buffer, size_t& offset, const T& arg)
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
        frame.trap_stack_pointer = c.trap_stack_pointer;

        auto& thread = c.thread();
        thread.callback_return_rax.reset();
        thread.callback_stack.emplace_back(std::move(frame));
    }

    template <typename... Args>
    void prepare_arm64_call_stack(arm64_cpu& cpu, const emulator_thread& thread, const uint32_t callback_index, const Args&... args)
    {
        const uint32_t arg_length = user_callback_args_size<Args...>();
        const uint64_t current_sp = cpu.read_stack_pointer();
        const uint64_t new_sp = align_down(current_sp - align_up(arg_length, 0x10) - 0x10, 0x10);
        const uint64_t arg_buffer = new_sp + 0x10;

        thread.grow_stack_for_frame(new_sp, current_sp);

        // KiUserCallbackDispatcher reads the arg buffer pointer, arg length and callback index from [sp].
        cpu.reg(arm64_register::sp, new_sp);

        if constexpr (sizeof...(Args) > 0)
        {
            size_t offset = 0;
            (write_user_callback_arg(cpu, arg_buffer, offset, args), ...);
        }

        cpu.write_memory(new_sp, &arg_buffer, sizeof(arg_buffer));
        cpu.write_memory(new_sp + 0x8, &arg_length, sizeof(arg_length));
        cpu.write_memory(new_sp + 0xC, &callback_index, sizeof(callback_index));
    }

    // KeUserModeCallback's frame, with the arguments right after it. KiUserCallbackDispatcher reads the buffer, its length
    // and the callback index; its unwind data reads the syscall's return RIP and RSP from the machine frame.
    struct user_callout_frame
    {
        std::array<uint64_t, 4> home{};
        uint64_t buffer{};
        uint32_t length{};
        uint32_t api_number{};
        MACHINE_FRAME machine_frame{};
    };

    static_assert(offsetof(user_callout_frame, machine_frame) == 0x30);
    static_assert(sizeof(user_callout_frame) == 0x58);

    template <typename... Args>
    void prepare_call_stack(x86_64_cpu& emu, const emulator_thread& thread, const uint64_t trap_rip, const uint64_t trap_rsp,
                            const uint32_t callback_index, const Args&... args)
    {
        const uint32_t arg_length = user_callback_args_size<Args...>();
        const uint64_t current_rsp = emu.read_stack_pointer();
        const uint64_t new_rsp = align_down(current_rsp - (align_up(arg_length, 0x10) + sizeof(user_callout_frame)), 0x10);

        user_callout_frame frame{};
        frame.buffer = new_rsp + sizeof(user_callout_frame);
        frame.length = arg_length;
        frame.api_number = callback_index;
        frame.machine_frame.Rip = trap_rip;
        frame.machine_frame.Rsp = trap_rsp;

        thread.grow_stack_for_frame(new_rsp, current_rsp);
        emu.reg(x86_register::rsp, new_rsp);
        emu.write_memory(new_rsp, &frame, sizeof(frame));

        if constexpr (sizeof...(Args) > 0)
        {
            size_t offset = 0;
            (write_user_callback_arg(emu, frame.buffer, offset, args), ...);
        }
    }

    template <typename StateT, typename... Args>
        requires(std::derived_from<std::remove_reference_t<StateT>, completion_state> ||
                 std::same_as<std::remove_reference_t<StateT>, std::nullptr_t>)
    void dispatch_user_callback(const syscall_context& c, const callback_id completion_id, const uint32_t callback_index,
                                StateT&& state_obj, const Args&... args)
    {
        push_callback_frame(c, completion_id, std::forward<StateT>(state_obj));

        if (auto* arm64 = dynamic_cast<arm64_cpu*>(&c.emu))
        {
            prepare_arm64_call_stack(*arm64, c.thread(), callback_index, args...);
            arm64->reg(arm64_register::pc, c.proc.ki_user_callback_dispatcher);
        }
        else
        {
            // The syscall instruction is still at RIP: the kernel's trap frame holds the instruction after it.
            auto& cpu = c.x64_cpu();
            const auto trap_rip = cpu.read_instruction_pointer() + c.win_emu.abi().syscall_abi().syscall_instruction_size();
            prepare_call_stack(cpu, c.thread(), trap_rip, c.trap_stack_pointer, callback_index, args...);
            cpu.reg(x86_register::rip, c.proc.ki_user_callback_dispatcher);
        }

        c.run_callback = true;
    }

    template <typename... Args>
    void dispatch_user_callback(const syscall_context& c, const callback_id completion_id, const uint32_t callback_index,
                                const Args&... args)
    {
        dispatch_user_callback(c, completion_id, callback_index, nullptr, args...);
    }

} // namespace sogen
