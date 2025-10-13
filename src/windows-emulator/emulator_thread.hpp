#pragma once

#include "handles.hpp"
#include "emulator_utils.hpp"
#include "memory_manager.hpp"

#include <utils/moved_marker.hpp>

struct process_context;

struct pending_apc
{
    uint32_t flags{};
    uint64_t apc_routine{};
    uint64_t apc_argument1{};
    uint64_t apc_argument2{};
    uint64_t apc_argument3{};

    void serialize(utils::buffer_serializer& buffer) const
    {
        buffer.write(this->flags);
        buffer.write(this->apc_routine);
        buffer.write(this->apc_argument1);
        buffer.write(this->apc_argument2);
        buffer.write(this->apc_argument3);
    }

    void deserialize(utils::buffer_deserializer& buffer)
    {
        buffer.read(this->flags);
        buffer.read(this->apc_routine);
        buffer.read(this->apc_argument1);
        buffer.read(this->apc_argument2);
        buffer.read(this->apc_argument3);
    }
};

class emulator_thread : public ref_counted_object
{
  public:
    emulator_thread(memory_manager& memory)
        : memory_ptr(&memory)
    {
    }

    emulator_thread(utils::buffer_deserializer& buffer)
        : emulator_thread(buffer.read<memory_manager_wrapper>().get())
    {
    }

    emulator_thread(memory_manager& memory, const process_context& context, uint64_t start_address, uint64_t argument, uint64_t stack_size,
                    bool suspended, uint32_t id);

    emulator_thread(const emulator_thread&) = delete;
    emulator_thread& operator=(const emulator_thread&) = delete;

    emulator_thread(emulator_thread&& obj) noexcept = default;
    emulator_thread& operator=(emulator_thread&& obj) noexcept = default;

    ~emulator_thread() override
    {
        this->release();
    }

    utils::moved_marker marker{};

    memory_manager* memory_ptr{};

    uint64_t stack_base{};                    // Native 64-bit stack base
    uint64_t stack_size{};                    // Native 64-bit stack size
    std::optional<uint64_t> wow64_stack_base; // WOW64 32-bit stack base
    std::optional<uint64_t> wow64_stack_size; // WOW64 32-bit stack size
    uint64_t start_address{};
    uint64_t argument{};
    uint64_t executed_instructions{0};

    uint32_t id{};

    uint64_t current_ip{0};
    uint64_t previous_ip{0};

    std::u16string name{};

    std::optional<NTSTATUS> exit_status{};
    std::vector<handle> await_objects{};
    bool await_any{false};
    bool waiting_for_alert{false};
    bool alerted{false};
    uint32_t suspended{0};
    std::optional<std::chrono::steady_clock::time_point> await_time{};

    bool apc_alertable{false};
    std::vector<pending_apc> pending_apcs{};

    std::optional<NTSTATUS> pending_status{};

    std::optional<emulator_allocator> gs_segment;
    std::optional<emulator_object<TEB64>> teb64;                          // Native 64-bit TEB
    std::optional<emulator_object<TEB32>> teb32;                          // WOW64 32-bit TEB
    std::optional<emulator_allocator> wow64_context_segment;              // For WOW64 context (CONTEXT64) allocation
    std::optional<emulator_object<WOW64_CPURESERVED>> wow64_cpu_reserved; // Persistent WOW64 thread context for ThreadWow64Context queries

    std::vector<std::byte> last_registers{};

    void mark_as_ready(NTSTATUS status);

    bool is_await_time_over(utils::clock& clock) const
    {
        return this->await_time.has_value() && this->await_time.value() < clock.steady_now();
    }

    bool is_terminated() const;

    bool is_thread_ready(process_context& process, utils::clock& clock);

    void save(x86_64_emulator& emu)
    {
        this->last_registers = emu.save_registers();
    }

    void restore(x86_64_emulator& emu) const
    {
        emu.restore_registers(this->last_registers);
    }

    void setup_if_necessary(x86_64_emulator& emu, const process_context& context)
    {
        if (!this->executed_instructions)
        {
            this->setup_registers(emu, context);
        }

        if (this->pending_status.has_value())
        {
            const auto status = *this->pending_status;
            this->pending_status = {};

            emu.reg<uint64_t>(x86_register::rax, static_cast<uint64_t>(status));
        }
    }

    void serialize_object(utils::buffer_serializer& buffer) const override
    {
        if (this->marker.was_moved())
        {
            throw std::runtime_error("Object was moved!");
        }

        buffer.write(this->stack_base);
        buffer.write(this->stack_size);
        buffer.write(this->start_address);
        buffer.write(this->argument);
        buffer.write(this->executed_instructions);
        buffer.write(this->id);
        buffer.write(this->current_ip);
        buffer.write(this->previous_ip);

        buffer.write_string(this->name);

        buffer.write_optional(this->exit_status);
        buffer.write_vector(this->await_objects);
        buffer.write(this->await_any);

        buffer.write(this->waiting_for_alert);
        buffer.write(this->alerted);

        buffer.write(this->suspended);
        buffer.write_optional(this->await_time);

        buffer.write(this->apc_alertable);
        buffer.write_vector(this->pending_apcs);

        buffer.write_optional(this->pending_status);
        buffer.write_optional(this->gs_segment);
        buffer.write_optional(this->teb64);
        buffer.write_optional(this->wow64_stack_base);
        buffer.write_optional(this->wow64_stack_size);
        buffer.write_optional(this->teb32);
        buffer.write_optional(this->wow64_context_segment);
        buffer.write_optional(this->wow64_cpu_reserved);

        buffer.write_vector(this->last_registers);
    }

    void deserialize_object(utils::buffer_deserializer& buffer) override
    {
        if (this->marker.was_moved())
        {
            throw std::runtime_error("Object was moved!");
        }

        this->release();

        buffer.read(this->stack_base);
        buffer.read(this->stack_size);
        buffer.read(this->start_address);
        buffer.read(this->argument);
        buffer.read(this->executed_instructions);
        buffer.read(this->id);
        buffer.read(this->current_ip);
        buffer.read(this->previous_ip);

        buffer.read_string(this->name);

        buffer.read_optional(this->exit_status);
        buffer.read_vector(this->await_objects);
        buffer.read(this->await_any);

        buffer.read(this->waiting_for_alert);
        buffer.read(this->alerted);

        buffer.read(this->suspended);
        buffer.read_optional(this->await_time);

        buffer.read(this->apc_alertable);
        buffer.read_vector(this->pending_apcs);

        buffer.read_optional(this->pending_status);
        buffer.read_optional(this->gs_segment, [this] { return emulator_allocator(*this->memory_ptr); });
        buffer.read_optional(this->teb64, [this] { return emulator_object<TEB64>(*this->memory_ptr); });
        buffer.read_optional(this->wow64_stack_base);
        buffer.read_optional(this->wow64_stack_size);
        buffer.read_optional(this->teb32, [this] { return emulator_object<TEB32>(*this->memory_ptr); });
        buffer.read_optional(this->wow64_context_segment, [this] { return emulator_allocator(*this->memory_ptr); });
        buffer.read_optional(this->wow64_cpu_reserved, [this] { return emulator_object<WOW64_CPURESERVED>(*this->memory_ptr); });

        buffer.read_vector(this->last_registers);
    }

    void leak_memory()
    {
        this->marker.mark_as_moved();
    }

    static bool deleter(emulator_thread& t)
    {
        return ref_counted_object::deleter(t) && t.is_terminated();
    }

  private:
    void setup_registers(x86_64_emulator& emu, const process_context& context) const;

    void release()
    {
        if (this->marker.was_moved())
        {
            return;
        }

        if (this->stack_base)
        {
            if (!this->memory_ptr)
            {
                throw std::runtime_error("Emulator was never assigned!");
            }

            this->memory_ptr->release_memory(this->stack_base, static_cast<size_t>(this->stack_size));
            this->stack_base = 0;
        }

        if (this->gs_segment)
        {
            this->gs_segment->release(*this->memory_ptr);
            this->gs_segment = {};
        }
    }
};
