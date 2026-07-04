#pragma once

#include "cpu_interface.hpp"
#include "memory_interface.hpp"
#include "serialization.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <source_location>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace sogen
{

    class emulator_stack_leak_collector
    {
      public:
        struct leak_record
        {
            uint64_t address{};
            uint64_t previous_stack_pointer{};
            std::source_location origin{};
        };

        emulator_stack_leak_collector()
            : previous_(std::exchange(active_, this))
        {
        }

        ~emulator_stack_leak_collector()
        {
            active_ = previous_;
        }

        emulator_stack_leak_collector(const emulator_stack_leak_collector&) = delete;
        emulator_stack_leak_collector& operator=(const emulator_stack_leak_collector&) = delete;
        emulator_stack_leak_collector(emulator_stack_leak_collector&&) = delete;
        emulator_stack_leak_collector& operator=(emulator_stack_leak_collector&&) = delete;

        static void report_leak(const leak_record& leak) noexcept
        {
            if (active_)
            {
                if (active_->leak_count_ < active_->leaks_.size())
                {
                    active_->leaks_[active_->leak_count_] = leak;
                }
                ++active_->leak_count_;
            }
        }

        void throw_if_leaked() const
        {
            if (leak_count_ != 0)
            {
                std::ostringstream message{};
                message << leak_count_ << " emulator stack allocation(s) were not released";

                const auto recorded_count = std::min(leak_count_, leaks_.size());
                for (size_t i = 0; i < recorded_count; ++i)
                {
                    const auto& leak = leaks_[i];
                    message << "\n  guest stack 0x" << std::hex << leak.address << "..0x" << leak.previous_stack_pointer << std::dec;
                    if (leak.origin.line() != 0)
                    {
                        message << " allocated at " << leak.origin.file_name() << ':' << leak.origin.line() << " in "
                                << get_short_function_name(leak.origin);
                    }
                    else
                    {
                        message << " restored from serialized state";
                    }
                }

                if (leak_count_ > recorded_count)
                {
                    message << "\n  ... and " << (leak_count_ - recorded_count) << " more";
                }

                throw std::runtime_error(message.str());
            }
        }

      private:
        static std::string_view get_short_function_name(const std::source_location& origin)
        {
            std::string_view name{origin.function_name()};
            if (const auto parameters = name.find('('); parameters != std::string_view::npos)
            {
                name = name.substr(0, parameters);
            }

            const auto end = name.find_last_not_of(' ');
            if (end == std::string_view::npos)
            {
                return {};
            }
            name = name.substr(0, end + 1);

            if (const auto scope = name.rfind("::"); scope != std::string_view::npos)
            {
                return name.substr(scope + 2);
            }
            if (const auto space = name.find_last_of(' '); space != std::string_view::npos)
            {
                return name.substr(space + 1);
            }
            return name;
        }

        inline static thread_local emulator_stack_leak_collector* active_{};
        static constexpr size_t max_recorded_leaks = 16;

        emulator_stack_leak_collector* previous_{};
        std::array<leak_record, max_recorded_leaks> leaks_{};
        size_t leak_count_{};
    };

    struct emulator_stack_allocation
    {
        emulator_stack_allocation() = default;

        ~emulator_stack_allocation()
        {
            if (*this)
            {
                emulator_stack_leak_collector::report_leak({address_, previous_stack_pointer_, origin_});
            }
        }

        emulator_stack_allocation(const emulator_stack_allocation&) = delete;
        emulator_stack_allocation& operator=(const emulator_stack_allocation&) = delete;

        emulator_stack_allocation(emulator_stack_allocation&& other) noexcept
            : address_(std::exchange(other.address_, 0)),
              previous_stack_pointer_(std::exchange(other.previous_stack_pointer_, 0)),
              origin_(std::exchange(other.origin_, std::source_location{}))
        {
        }

        emulator_stack_allocation& operator=(emulator_stack_allocation&& other)
        {
            if (this != &other)
            {
                if (*this)
                {
                    throw std::logic_error("Attempted to overwrite an active emulator stack allocation");
                }

                address_ = std::exchange(other.address_, 0);
                previous_stack_pointer_ = std::exchange(other.previous_stack_pointer_, 0);
                origin_ = std::exchange(other.origin_, std::source_location{});
            }
            return *this;
        }

        explicit operator bool() const
        {
            return previous_stack_pointer_ != 0;
        }

        uint64_t address() const
        {
            return address_;
        }

        void serialize(utils::buffer_serializer& buffer) const
        {
            buffer.write(address_);
            buffer.write(previous_stack_pointer_);
        }

        void deserialize(utils::buffer_deserializer& buffer)
        {
            if (*this)
            {
                throw std::logic_error("Attempted to deserialize over an active emulator stack allocation");
            }

            uint64_t address{};
            uint64_t previous_stack_pointer{};
            buffer.read(address);
            buffer.read(previous_stack_pointer);

            const bool is_active = previous_stack_pointer != 0;
            if ((!is_active && address != 0) ||
                (is_active && (previous_stack_pointer <= address || (address & (stack_alignment - 1)) != 0)))
            {
                throw std::runtime_error("Invalid serialized emulator stack allocation");
            }

            address_ = address;
            previous_stack_pointer_ = previous_stack_pointer;
            origin_ = {};
        }

      private:
        template <typename>
        friend class typed_cpu;

        static constexpr uint64_t stack_alignment = 16;

        emulator_stack_allocation(const uint64_t address, const uint64_t previous_stack_pointer, const std::source_location origin)
            : address_(address),
              previous_stack_pointer_(previous_stack_pointer),
              origin_(origin)
        {
        }

        void release()
        {
            address_ = 0;
            previous_stack_pointer_ = 0;
            origin_ = {};
        }

        uint64_t address_{};
        uint64_t previous_stack_pointer_{};
        std::source_location origin_{};
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
        [[nodiscard]] emulator_stack_allocation push_stack(const T& data,
                                                           const std::source_location origin = std::source_location::current())
        {
            const auto old_rsp = read_stack_pointer();
            if (sizeof(T) > static_cast<uint64_t>(old_rsp))
            {
                throw std::underflow_error("Emulator stack allocation underflow");
            }

            const auto new_rsp = static_cast<pointer_type>((old_rsp - sizeof(T)) & ~(emulator_stack_allocation::stack_alignment - 1));
            write_memory(new_rsp, &data, sizeof(T));
            reg(stack_pointer, new_rsp);

            return emulator_stack_allocation{new_rsp, old_rsp, origin};
        }

        void pop_stack(emulator_stack_allocation&& allocation)
        {
            if (!allocation)
            {
                throw std::logic_error("Attempted to release an inactive emulator stack allocation");
            }

            const auto current_rsp = read_stack_pointer();
            if (current_rsp != allocation.address_ || allocation.previous_stack_pointer_ <= allocation.address_ ||
                allocation.previous_stack_pointer_ > std::numeric_limits<pointer_type>::max())
            {
                throw std::runtime_error("Invalid emulator stack deallocation order");
            }

            reg(stack_pointer, allocation.previous_stack_pointer_);
            allocation.release();
        }

      private:
        size_t read_raw_register(int reg, void* value, size_t size) override = 0;
        size_t write_raw_register(int reg, const void* value, size_t size) override = 0;
    };

} // namespace sogen
