#pragma once
#include <x64_emulator.hpp>
#include "scoped_hook.hpp"
#include <utils/concurrency.hpp>
#include <gdb-stub/gdb_stub.hpp>

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

inline memory_operation map_breakpoint_type(const gdb_stub::breakpoint_type type)
{
    using enum gdb_stub::breakpoint_type;

    switch (type)
    {
    case software:
    case hardware_exec:
        return memory_operation::exec;
    case hardware_read:
        return memory_permission::read;
    case hardware_write:
        return memory_permission::write;
    case hardware_read_write:
        return memory_permission::read_write;
    default:
        throw std::runtime_error("Bad bp type");
    }
}

struct breakpoint_key
{
    size_t addr{};
    size_t size{};
    gdb_stub::breakpoint_type type{};

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

class x64_gdb_stub_handler : public gdb_stub::gdb_stub_handler
{
  public:
    x64_gdb_stub_handler(x64_emulator& emu)
        : emu_(&emu)
    {
    }

    ~x64_gdb_stub_handler() override = default;

    gdb_stub::gdb_action run() override
    {
        try
        {
            this->emu_->start_from_ip();
        }
        catch (const std::exception& e)
        {
            puts(e.what());
        }

        return gdb_stub::gdb_action::resume;
    }

    gdb_stub::gdb_action singlestep() override
    {
        try
        {
            this->emu_->start_from_ip({}, 1);
        }
        catch (const std::exception& e)
        {
            puts(e.what());
        }

        return gdb_stub::gdb_action::resume;
    }

    size_t get_register_count() override
    {
        return gdb_registers.size();
    }

    size_t get_max_register_size() override
    {
        return 256 / 8;
    }

    bool read_register(const size_t reg, void* data, const size_t max_length) override
    {
        try
        {
            if (reg >= gdb_registers.size())
            {
                return false;
            }

            this->emu_->read_register(gdb_registers[reg], data, max_length);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool write_register(const size_t reg, const void* data, const size_t size) override
    {
        try
        {
            if (reg >= gdb_registers.size())
            {
                return false;
            }

            this->emu_->write_register(gdb_registers[reg], data, size);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool read_memory(const uint64_t address, void* data, const size_t length) override
    {
        return this->emu_->try_read_memory(address, data, length);
    }

    bool write_memory(const uint64_t address, const void* data, const size_t length) override
    {
        try
        {
            this->emu_->write_memory(address, data, length);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool set_breakpoint(const gdb_stub::breakpoint_type type, const uint64_t addr, const size_t size) override
    {
        try
        {
            return this->hooks_.access<bool>([&](hook_map& hooks) {
                hooks[{addr, size, type}] = scoped_hook(
                    *this->emu_, this->emu_->hook_memory_access(addr, size, map_breakpoint_type(type),
                                                                [this](uint64_t, size_t, uint64_t, memory_operation) {
                                                                    this->on_interrupt(); //
                                                                }));

                return true;
            });
        }
        catch (...)
        {
            return false;
        }
    }

    bool delete_breakpoint(const gdb_stub::breakpoint_type type, const uint64_t addr, const size_t size) override
    {
        try
        {
            return this->hooks_.access<bool>([&](hook_map& hooks) {
                const auto entry = hooks.find({addr, size, type});
                if (entry == hooks.end())
                {
                    return false;
                }

                hooks.erase(entry);

                return true;
            });
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

    using hook_map = std::unordered_map<breakpoint_key, scoped_hook>;
    utils::concurrency::container<hook_map> hooks_{};
};
