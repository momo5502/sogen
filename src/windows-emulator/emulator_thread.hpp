#pragma once

#include "handles.hpp"
#include "emulator_utils.hpp"
#include "memory_manager.hpp"

#include <utils/moved_marker.hpp>

struct process_context;

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

    emulator_thread(memory_manager& memory, const process_context& context, uint64_t start_address, uint64_t argument,
                    uint64_t stack_size, uint32_t id);

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

    uint64_t stack_base{};
    uint64_t stack_size{};
    uint64_t start_address{};
    uint64_t argument{};
    uint64_t executed_instructions{0};

    uint32_t id{};

    std::u16string name{};

    std::optional<NTSTATUS> exit_status{};
    std::vector<handle> await_objects{};
    bool await_any{false};
    bool waiting_for_alert{false};
    bool alerted{false};
    std::optional<std::chrono::steady_clock::time_point> await_time{};

    std::optional<NTSTATUS> pending_status{};

    std::optional<emulator_allocator> gs_segment;
    std::optional<emulator_object<TEB64>> teb;

    std::vector<std::byte> last_registers{};

    void mark_as_ready(NTSTATUS status);

    bool is_await_time_over() const
    {
        return this->await_time.has_value() && this->await_time.value() < std::chrono::steady_clock::now();
    }

    bool is_terminated() const;

    bool is_thread_ready(process_context& process);

    void save(x64_emulator& emu)
    {
        this->last_registers = emu.save_registers();
    }

    void restore(x64_emulator& emu) const
    {
        emu.restore_registers(this->last_registers);
    }

    void setup_if_necessary(x64_emulator& emu, const process_context& context)
    {
        if (!this->executed_instructions)
        {
            this->setup_registers(emu, context);
        }

        if (this->pending_status.has_value())
        {
            const auto status = *this->pending_status;
            this->pending_status = {};

            emu.reg<uint64_t>(x64_register::rax, static_cast<uint64_t>(status));
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

        buffer.write_string(this->name);

        buffer.write_optional(this->exit_status);
        buffer.write_vector(this->await_objects);
        buffer.write(this->await_any);

        buffer.write(this->waiting_for_alert);
        buffer.write(this->alerted);

        buffer.write_optional(this->await_time);
        buffer.write_optional(this->pending_status);
        buffer.write_optional(this->gs_segment);
        buffer.write_optional(this->teb);

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

        buffer.read_string(this->name);

        buffer.read_optional(this->exit_status);
        buffer.read_vector(this->await_objects);
        buffer.read(this->await_any);

        buffer.read(this->waiting_for_alert);
        buffer.read(this->alerted);

        buffer.read_optional(this->await_time);
        buffer.read_optional(this->pending_status);
        buffer.read_optional(this->gs_segment, [this] { return emulator_allocator(*this->memory_ptr); });
        buffer.read_optional(this->teb, [this] { return emulator_object<TEB64>(*this->memory_ptr); });

        buffer.read_vector(this->last_registers);
    }

    void leak_memory()
    {
        this->marker.mark_as_moved();
    }

  private:
    void setup_registers(x64_emulator& emu, const process_context& context) const;

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

            this->memory_ptr->release_memory(this->stack_base, this->stack_size);
            this->stack_base = 0;
        }

        if (this->gs_segment)
        {
            this->gs_segment->release(*this->memory_ptr);
            this->gs_segment = {};
        }
    }
};
