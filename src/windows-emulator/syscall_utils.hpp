#pragma once

#include "windows_emulator.hpp"
#include "arch/syscall_abi.hpp"
#include <algorithm>
#include <bit>
#include <ctime>
#include <platform/primitives.hpp>
#include "devices/named_pipe.hpp"

namespace sogen
{

    struct completion_state;

    struct user_callback_result
    {
        uint64_t value{};
        uint32_t output_size{};
        uint64_t output{};
    };

    static_assert(sizeof(user_callback_result) == 24);
    static_assert(offsetof(user_callback_result, output) == 16);

    struct syscall_context
    {
        windows_emulator& win_emu;
        // The virtual CPU that issued this syscall. Register and memory access must go
        // through this CPU, not windows_emulator::emu() (which always resolves vCPU 0).
        guest_cpu& emu;
        vcpu_context& vcpu;
        process_context& proc;
        mutable bool write_status{true};
        mutable bool retrigger_syscall{false};

        mutable bool run_callback{false};
        bool is_callback_completion{false};
        completion_state* current_completion_state{};
        user_callback_result previous_callback_result{};

        emulator_thread& thread() const
        {
            return this->vcpu.thread();
        }

        x86_64_cpu& x64_cpu() const
        {
            auto* cpu = dynamic_cast<x86_64_cpu*>(&this->emu);
            if (!cpu)
            {
                throw std::runtime_error("This operation is only implemented for x86-64 guest CPUs");
            }

            return *cpu;
        }

        template <typename T>
        T get_callback_result() const
        {
            if (!this->is_callback_completion)
            {
                throw std::runtime_error("Attempted to get callback result in a non-completion context");
            }
            return static_cast<T>(this->previous_callback_result.value);
        }

        const user_callback_result& get_callback_result_object() const
        {
            if (!this->is_callback_completion)
            {
                throw std::runtime_error("Attempted to get callback result in a non-completion context");
            }
            return this->previous_callback_result;
        }

        template <typename T>
        T& get_completion_state() const
        {
            if (!this->is_callback_completion)
            {
                throw std::runtime_error("Attempted to get callback state in a non-completion context");
            }
            if (!this->current_completion_state)
            {
                throw std::runtime_error("No state object was assigned to this completion");
            }
            return dynamic_cast<T&>(*this->current_completion_state);
        }
    };

    // The ZeroBits of NtAllocateVirtualMemory/NtMapViewOfSection as the highest address an allocator-chosen range may end at.
    // Up to 32 it counts high-order bits of the address that must be zero. Above that it is a mask, of which only the
    // leading zero bits count (Wine's tests confirm this on Windows with 0x1aaaaaaa). The WoW64 thunks pass 0xFFFEFFFF
    // for every request of 32-bit code, which keeps that code's memory where its pointers reach.
    inline uint64_t highest_address_for_zero_bits(const uint64_t zero_bits)
    {
        constexpr uint64_t unrestricted = MAX_ALLOCATION_END_EXCL - 1;
        const auto high_zero_bits = zero_bits <= 32 ? static_cast<int>(zero_bits) : std::countl_zero(zero_bits);
        return std::min(~uint64_t{0} >> high_zero_bits, unrestricted);
    }

    inline uint64_t get_syscall_argument(const syscall_context& context, const size_t index)
    {
        return context.win_emu.abi().syscall_argument(context.emu, index);
    }

    inline bool is_uppercase(const char character)
    {
        return toupper(character) == character;
    }

    inline bool is_syscall(const std::string_view name)
    {
        return name.starts_with("Nt") && name.size() > 3 && is_uppercase(name[2]);
    }

    inline bool is_named_pipe_path(const std::u16string_view& filename)
    {
        return utils::string::starts_with_ignore_case(filename, {u"\\Device\\NamedPipe\\"}) ||
               utils::string::starts_with_ignore_case(filename, {u"\\??\\Pipe\\"});
    }

    inline std::optional<uint32_t> extract_syscall_id(const exported_symbol& symbol, std::span<const std::byte> data)
    {
        return x86_64_syscall_abi_instance().scan_syscall_id(symbol, data);
    }

    inline std::map<uint64_t, std::string> find_syscalls(const exported_symbols& exports, std::span<const std::byte> data,
                                                         const arch_syscall_abi& abi = x86_64_syscall_abi_instance())
    {
        std::map<uint64_t, std::string> syscalls{};

        for (const auto& symbol : exports)
        {
            const auto id = abi.scan_syscall_id(symbol, data);
            if (id)
            {
                auto& entry = syscalls[*id];

                if (!entry.empty())
                {
                    throw std::runtime_error("Syscall with id " + std::to_string(*id) + ", which is mapping to " + symbol.name +
                                             ", was already mapped to " + entry);
                }

                entry = symbol.name;
            }
        }

        return syscalls;
    }

    inline void map_syscalls(std::map<uint64_t, syscall_handler_entry>& handlers, std::map<uint64_t, std::string> syscalls)
    {
        for (auto& [id, name] : syscalls)
        {
            auto& entry = handlers[id];

            if (!entry.name.empty())
            {
                throw std::runtime_error("Syscall with id " + std::to_string(id) + ", which is mapping to " + name +
                                         ", was previously mapped to " + entry.name);
            }

            entry.name = std::move(name);
            entry.handler = nullptr;
        }
    }

    template <typename T>
        requires(std::is_integral_v<T> || std::is_enum_v<T>)
    T resolve_argument(const syscall_context& context, const size_t index)
    {
        const auto arg = get_syscall_argument(context, index);
        return static_cast<T>(arg);
    }

    template <typename T>
        requires(std::is_same_v<std::remove_cvref_t<T>, handle>)
    handle resolve_argument(const syscall_context& context, const size_t index)
    {
        handle h{};
        h.bits = resolve_argument<uint64_t>(context, index);
        return h;
    }

    template <typename T>
        requires(std::is_same_v<T, emulator_object<typename T::value_type>>)
    T resolve_argument(const syscall_context& context, const size_t index)
    {
        const auto arg = get_syscall_argument(context, index);
        return T(context.emu.memory(), arg);
    }

    template <typename T>
    T resolve_indexed_argument(const syscall_context& context, size_t& index)
    {
        return resolve_argument<T>(context, index++);
    }

    inline void write_syscall_result(const syscall_context& c, const uint64_t result, const uint64_t initial_ip)
    {
        if (c.write_status && !c.retrigger_syscall && !c.run_callback)
        {
            c.win_emu.abi().return_value(c.emu, result);
        }

        const auto new_ip = c.emu.read_instruction_pointer();
        if ((initial_ip != new_ip || c.retrigger_syscall || c.run_callback) && !c.is_callback_completion)
        {
            c.win_emu.abi().rewind_syscall(c.emu, new_ip);
        }
    }

    template <typename Result>
    void forward_syscall(const syscall_context& c, Result (*handler)())
    {
        const auto ip = c.emu.read_instruction_pointer();

        const auto ret = handler();
        write_syscall_result(c, static_cast<uint64_t>(ret), ip);
    }

    template <typename Result, typename... Args>
    void forward_syscall(const syscall_context& c, Result (*handler)(const syscall_context&, Args...))
    {
        const auto ip = c.emu.read_instruction_pointer();

        size_t index = 0;
        std::tuple<const syscall_context&, Args...> func_args{
            c, resolve_indexed_argument<std::remove_cv_t<std::remove_reference_t<Args>>>(c, index)...};

        (void)index;

        const auto ret = std::apply(handler, std::move(func_args));
        write_syscall_result(c, ret, ip);
    }

    template <auto Handler>
    syscall_handler make_syscall_handler()
    {
        return +[](const syscall_context& c) { forward_syscall(c, Handler); };
    }

    template <typename T, typename Traits>
    void write_attribute(memory_interface& emu, const PS_ATTRIBUTE<Traits>& attribute, const T& value)
    {
        if (attribute.ReturnLength)
        {
            emulator_object<typename Traits::SIZE_T>{emu, attribute.ReturnLength}.write(sizeof(T));
        }

        if (attribute.Size >= sizeof(T))
        {
            emulator_object<T>{emu, attribute.Value}.write(value);
        }
    }

    template <typename ResponseType, typename Action, typename ReturnLengthSetter>
    NTSTATUS handle_query_internal(guest_cpu& emu, const uint64_t buffer, const uint32_t length,
                                   const ReturnLengthSetter& return_length_setter, const Action& action)
    {
        constexpr auto required_size = sizeof(ResponseType);
        return_length_setter(required_size);

        if (length < required_size)
        {
            return STATUS_BUFFER_TOO_SMALL;
        }

        ResponseType obj{};
        NTSTATUS result = STATUS_SUCCESS;

        using action_result = std::invoke_result_t<Action, ResponseType&>;

        if constexpr (std::is_same_v<NTSTATUS, action_result>)
        {
            result = action(obj);
        }
        else
        {
            action(obj);
        }

        if (result == STATUS_SUCCESS)
        {
            emu.write_memory(buffer, obj);
        }

        return result;
    }

    template <typename ResponseType, typename Action, typename LengthType>
        requires(std::is_integral_v<LengthType>)
    NTSTATUS handle_query(guest_cpu& emu, const uint64_t buffer, const uint32_t length, const emulator_object<LengthType> return_length,
                          const Action& action)
    {
        const auto length_setter = [&](const size_t required_size) {
            if (return_length)
            {
                return_length.try_write(static_cast<LengthType>(required_size));
            }
        };

        return handle_query_internal<ResponseType>(emu, buffer, length, length_setter, action);
    }

    template <typename ResponseType, typename Action>
    NTSTATUS handle_query(guest_cpu& emu, const uint64_t buffer, const uint32_t length,
                          const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block, const Action& action)
    {
        IO_STATUS_BLOCK<EmulatorTraits<Emu64>> status_block{};

        const auto length_setter = [&](const EmulatorTraits<Emu64>::ULONG_PTR required_size) {
            status_block.Information = required_size; //
        };

        status_block.Status = handle_query_internal<ResponseType>(emu, buffer, length, length_setter, action);

        if (io_status_block)
        {
            io_status_block.write(status_block);
        }

        return status_block.Status;
    }

} // namespace sogen
