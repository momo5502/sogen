#pragma once

#include "cpu_interface.hpp"
#include "memory_interface.hpp"
#include "serialization.hpp"

#include <cassert>
#include <utility>

namespace sogen
{

    struct emulator_stack_allocation
    {
        uint64_t address{};
        size_t size{};

        emulator_stack_allocation() = default;

        ~emulator_stack_allocation()
        {
            assert(address == 0 && "Emulator stack leak: allocation was not freed before destruction");
        }

        emulator_stack_allocation(const emulator_stack_allocation&) = delete;
        emulator_stack_allocation& operator=(const emulator_stack_allocation&) = delete;

        emulator_stack_allocation(emulator_stack_allocation&& other) noexcept
            : address(std::exchange(other.address, 0)),
              size(std::exchange(other.size, 0))
        {
        }

        emulator_stack_allocation& operator=(emulator_stack_allocation&& other) noexcept
        {
            if (this != &other)
            {
                address = std::exchange(other.address, 0);
                size = std::exchange(other.size, 0);
            }
            return *this;
        }

        void serialize(utils::buffer_serializer& buffer) const
        {
            buffer.write(address);
            buffer.write(size);
        }

        void deserialize(utils::buffer_deserializer& buffer)
        {
            buffer.read(address);
            buffer.read(size);
        }
    };

    // A single virtual CPU's view of the machine: typed register and stack access
    // native to this CPU, memory access delegated to the shared machine.
    template <typename Traits>
    class typed_cpu : public cpu_interface
    {
      public:
        using registers = Traits::register_type;
        using pointer_type = Traits::pointer_type;

        static constexpr size_t pointer_size = sizeof(pointer_type);
        static constexpr registers stack_pointer = Traits::stack_pointer;
        static constexpr registers instruction_pointer = Traits::instruction_pointer;

        virtual memory_interface& memory() = 0;
        virtual const memory_interface& memory() const = 0;

        // Guest memory is shared by every vCPU, so a CPU can stand in wherever a
        // memory_interface is expected (emulator_object, read_string, ...). Only
        // register state is per-CPU.
        operator memory_interface&()
        {
            return this->memory();
        }

        operator const memory_interface&() const
        {
            return this->memory();
        }

        size_t write_register(registers reg, const void* value, const size_t size)
        {
            return this->write_raw_register(static_cast<int>(reg), value, size);
        }

        size_t read_register(registers reg, void* value, const size_t size)
        {
            return this->read_raw_register(static_cast<int>(reg), value, size);
        }

        template <typename T = pointer_type>
        T reg(const registers regid)
        {
            T value{};
            this->read_register(regid, &value, sizeof(value));
            return value;
        }

        template <typename T = pointer_type, typename S>
        void reg(const registers regid, const S& maybe_value)
        {
            T value = static_cast<T>(maybe_value);
            this->write_register(regid, &value, sizeof(value));
        }

        pointer_type read_instruction_pointer()
        {
            return this->reg(instruction_pointer);
        }

        pointer_type read_stack_pointer()
        {
            return this->reg(stack_pointer);
        }

        void read_memory(const uint64_t address, void* data, const size_t size) const
        {
            this->memory().read_memory(address, data, size);
        }

        bool try_read_memory(const uint64_t address, void* data, const size_t size) const
        {
            return this->memory().try_read_memory(address, data, size);
        }

        void write_memory(const uint64_t address, const void* data, const size_t size)
        {
            this->memory().write_memory(address, data, size);
        }

        bool try_write_memory(const uint64_t address, const void* data, const size_t size)
        {
            return this->memory().try_write_memory(address, data, size);
        }

        template <typename T>
        T read_memory(const uint64_t address) const
        {
            return this->memory().template read_memory<T>(address);
        }

        template <typename T>
        T read_memory(const void* address) const
        {
            return this->memory().template read_memory<T>(address);
        }

        std::vector<std::byte> read_memory(const uint64_t address, const size_t size) const
        {
            return this->memory().read_memory(address, size);
        }

        std::vector<std::byte> read_memory(const void* address, const size_t size) const
        {
            return this->memory().read_memory(address, size);
        }

        template <typename T>
        void write_memory(const uint64_t address, const T& value)
        {
            this->memory().write_memory(address, value);
        }

        template <typename T>
        void write_memory(void* address, const T& value)
        {
            this->memory().write_memory(address, value);
        }

        void write_memory(void* address, const void* data, const size_t size)
        {
            this->memory().write_memory(address, data, size);
        }

        void move_memory(const uint64_t dst, const uint64_t src, const size_t size)
        {
            this->memory().move_memory(dst, src, size);
        }

        pointer_type read_stack(const size_t index)
        {
            pointer_type result{};
            const auto sp = this->read_stack_pointer();

            this->read_memory(sp + (index * pointer_size), &result, sizeof(result));

            return result;
        }

        void write_stack(const size_t index, const pointer_type& value)
        {
            const auto sp = this->read_stack_pointer();
            this->write_memory(sp + (index * pointer_size), &value, sizeof(value));
        }

        void push_stack(const pointer_type& value)
        {
            const auto sp = this->read_stack_pointer() - pointer_size;
            this->reg(stack_pointer, sp);
            this->write_memory(sp, &value, sizeof(value));
        }

        pointer_type pop_stack()
        {
            pointer_type result{};
            const auto sp = this->read_stack_pointer();
            this->read_memory(sp, &result, sizeof(result));
            this->reg(stack_pointer, sp + pointer_size);

            return result;
        }

        template <typename T>
            requires std::is_trivially_copyable_v<T>
        emulator_stack_allocation push_stack(const T& data)
        {
            uint64_t old_rsp = read_stack_pointer();
            uint64_t new_rsp = (old_rsp - sizeof(T)) & ~0xF;

            reg(stack_pointer, new_rsp);
            write_memory(new_rsp, &data, sizeof(T));

            emulator_stack_allocation alloc{};
            alloc.address = new_rsp;
            alloc.size = static_cast<size_t>(old_rsp - new_rsp);

            return alloc;
        }

        void pop_stack(emulator_stack_allocation allocation)
        {
            uint64_t current_rsp = read_stack_pointer();
            assert(current_rsp == allocation.address && "Invalid stack deallocation");

            reg(stack_pointer, current_rsp + allocation.size);
            allocation = {};
        }

      private:
        size_t read_raw_register(int reg, void* value, size_t size) override = 0;
        size_t write_raw_register(int reg, const void* value, size_t size) override = 0;
    };

} // namespace sogen
