#pragma once
#include <x64_emulator.hpp>
#include "gdb_stub.hpp"
#include "scoped_hook.hpp"

inline std::vector gdb_registers{
    x64_register::rax, x64_register::rbx, x64_register::rcx, x64_register::rdx, x64_register::rsi, x64_register::rdi,
    x64_register::rbp, x64_register::rsp, x64_register::r8,  x64_register::r9,  x64_register::r10, x64_register::r11,
    x64_register::r12, x64_register::r13, x64_register::r14, x64_register::r15, x64_register::rip, x64_register::rflags,
    /*x64_register::cs,
    x64_register::ss,
    x64_register::ds,
    x64_register::es,
    x64_register::fs,
    x64_register::gs,*/
};

inline memory_operation map_breakpoint_type(const breakpoint_type type)
{
    switch (type)
    {
    case breakpoint_type::software:
    case breakpoint_type::hardware_exec:
        return memory_operation::exec;
    case breakpoint_type::hardware_read:
        return memory_permission::read;
    case breakpoint_type::hardware_write:
        return memory_permission::write;
    case breakpoint_type::hardware_read_write:
        return memory_permission::read_write;
    default:
        throw std::runtime_error("Bad bp type");
    }
}

struct breakpoint_key
{
    size_t addr{};
    size_t size{};
    breakpoint_type type{};

    bool operator==(const breakpoint_key& other) const
    {
        return this->addr == other.addr && this->size == other.size && this->type == other.type;
    }
};

template <>
struct std::hash<breakpoint_key>
{
    std::size_t operator()(const breakpoint_key& k) const noexcept
    {
        return ((std::hash<size_t>()(k.addr) ^ (std::hash<size_t>()(k.size) << 1)) >> 1) ^
               (std::hash<size_t>()(static_cast<size_t>(k.type)) << 1);
    }
};

class x64_gdb_stub_handler : public gdb_stub_handler
{
  public:
    x64_gdb_stub_handler(x64_emulator& emu)
        : emu_(&emu)
    {
    }

    ~x64_gdb_stub_handler() override = default;

    gdb_action cont() override
    {
        try
        {
            this->emu_->start_from_ip();
        }
        catch (const std::exception& e)
        {
            puts(e.what());
        }

        return gdb_action::resume;
    }

    gdb_action stepi() override
    {
        try
        {
            this->emu_->start_from_ip({}, 1);
        }
        catch (const std::exception& e)
        {
            puts(e.what());
        }

        return gdb_action::resume;
    }

    bool read_reg(const int regno, size_t* value) override
    {
        *value = 0;

        try
        {
            if (static_cast<size_t>(regno) >= gdb_registers.size())
            {
                return true;
            }

            this->emu_->read_register(gdb_registers[regno], value, sizeof(*value));
            return true;
        }
        catch (...)
        {
            return true;
        }
    }

    bool write_reg(const int regno, const size_t value) override
    {
        try
        {
            if (static_cast<size_t>(regno) >= gdb_registers.size())
            {
                return true;
            }

            this->emu_->write_register(gdb_registers[regno], &value, sizeof(value));
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool read_mem(const size_t addr, const size_t len, void* val) override
    {
        return this->emu_->try_read_memory(addr, val, len);
    }

    bool write_mem(const size_t addr, const size_t len, void* val) override
    {
        try
        {
            this->emu_->write_memory(addr, val, len);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool set_bp(const breakpoint_type type, const size_t addr, const size_t size) override
    {
        try
        {
            this->hooks_[{addr, size, type}] = scoped_hook(
                *this->emu_, this->emu_->hook_memory_access(
                                 addr, size, map_breakpoint_type(type),
                                 [this](uint64_t, size_t, uint64_t, memory_operation) { this->on_interrupt(); }));

            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool del_bp(const breakpoint_type type, const size_t addr, const size_t size) override
    {
        try
        {
            const auto entry = this->hooks_.find({addr, size, type});
            if (entry == this->hooks_.end())
            {
                return false;
            }

            this->hooks_.erase(entry);

            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    void on_interrupt() override
    {
        this->emu_->stop();
    }

  private:
    x64_emulator* emu_{};
    std::unordered_map<breakpoint_key, scoped_hook> hooks_{};
};
