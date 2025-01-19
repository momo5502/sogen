#pragma once
#include <x64_emulator.hpp>
#include "scoped_hook.hpp"
#include <utils/concurrency.hpp>
#include <gdb-stub/gdb_stub.hpp>

#include "x64_target_descriptions.hpp"
#include <optional>

struct register_entry
{
    x64_register reg;
    std::optional<size_t> expected_size;
    std::optional<size_t> offset;

    register_entry(const x64_register reg = x64_register::invalid,
                   const std::optional<size_t> expected_size = std::nullopt,
                   const std::optional<size_t> offset = std::nullopt)
        : reg(reg),
          expected_size(expected_size),
          offset(offset)
    {
    }
};

inline std::vector<register_entry> gdb_registers{
    x64_register::rax,
    x64_register::rbx,
    x64_register::rcx,
    x64_register::rdx,
    x64_register::rsi,
    x64_register::rdi,
    x64_register::rbp,
    x64_register::rsp,
    x64_register::r8,
    x64_register::r9,
    x64_register::r10,
    x64_register::r11,
    x64_register::r12,
    x64_register::r13,
    x64_register::r14,
    x64_register::r15,
    x64_register::rip,
    x64_register::eflags,
    {x64_register::cs, 4},
    {x64_register::ss, 4},
    {x64_register::ds, 4},
    {x64_register::es, 4},
    {x64_register::fs, 4},
    {x64_register::gs, 4},
    x64_register::st0,
    x64_register::st1,
    x64_register::st2,
    x64_register::st3,
    x64_register::st4,
    x64_register::st5,
    x64_register::st6,
    x64_register::st7,

    {x64_register::fpcw, 4},  // fctrl
    {x64_register::fpsw, 4},  // fstat
    {x64_register::fptag, 4}, // ftag
    {x64_register::fcs, 4},   // fiseg
    {x64_register::fip, 4},   // fioff
    {x64_register::fds, 4},   // foseg
    {x64_register::fdp, 4},   // fooff
    {x64_register::fop, 4},   // fop

    x64_register::xmm0,
    x64_register::xmm1,
    x64_register::xmm2,
    x64_register::xmm3,
    x64_register::xmm4,
    x64_register::xmm5,
    x64_register::xmm6,
    x64_register::xmm7,
    x64_register::xmm8,
    x64_register::xmm9,
    x64_register::xmm10,
    x64_register::xmm11,
    x64_register::xmm12,
    x64_register::xmm13,
    x64_register::xmm14,
    x64_register::xmm15,
    x64_register::mxcsr,
    x64_register::fs_base,
    x64_register::gs_base,
    {x64_register::ymm0, std::nullopt, 16},
    {x64_register::ymm1, std::nullopt, 16},
    {x64_register::ymm2, std::nullopt, 16},
    {x64_register::ymm3, std::nullopt, 16},
    {x64_register::ymm4, std::nullopt, 16},
    {x64_register::ymm5, std::nullopt, 16},
    {x64_register::ymm6, std::nullopt, 16},
    {x64_register::ymm7, std::nullopt, 16},
    {x64_register::ymm8, std::nullopt, 16},
    {x64_register::ymm9, std::nullopt, 16},
    {x64_register::ymm10, std::nullopt, 16},
    {x64_register::ymm11, std::nullopt, 16},
    {x64_register::ymm12, std::nullopt, 16},
    {x64_register::ymm13, std::nullopt, 16},
    {x64_register::ymm14, std::nullopt, 16},
    {x64_register::ymm15, std::nullopt, 16},
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

class x64_gdb_stub_handler : public gdb_stub::debugging_handler
{
  public:
    x64_gdb_stub_handler(x64_emulator& emu)
        : emu_(&emu)
    {
    }

    ~x64_gdb_stub_handler() override = default;

    gdb_stub::action run() override
    {
        try
        {
            this->emu_->start_from_ip();
        }
        catch (const std::exception& e)
        {
            puts(e.what());
        }

        return gdb_stub::action::resume;
    }

    gdb_stub::action singlestep() override
    {
        try
        {
            this->emu_->start_from_ip({}, 1);
        }
        catch (const std::exception& e)
        {
            puts(e.what());
        }

        return gdb_stub::action::resume;
    }

    size_t get_register_count() override
    {
        return gdb_registers.size();
    }

    size_t get_max_register_size() override
    {
        return 512 / 8;
    }

    size_t read_register(const size_t reg, void* data, const size_t max_length) override
    {
        try
        {
            if (reg >= gdb_registers.size())
            {
                return 0;
            }

            const auto real_reg = gdb_registers[reg];

            auto size = this->emu_->read_register(real_reg.reg, data, max_length);

            if (real_reg.offset)
            {
                size -= *real_reg.offset;
                memcpy(data, static_cast<uint8_t*>(data) + *real_reg.offset, size);
            }

            const auto result_size = real_reg.expected_size.value_or(size);

            if (result_size > size)
            {
                memset(static_cast<uint8_t*>(data) + size, 0, result_size - size);
            }

            return result_size;
        }
        catch (...)
        {
            return 0;
        }
    }

    size_t write_register(const size_t reg, const void* data, const size_t size) override
    {
        try
        {
            if (reg >= gdb_registers.size())
            {
                return 0;
            }

            const auto real_reg = gdb_registers[reg];

            size_t written_size = 0;

            if (real_reg.offset)
            {
                std::vector<std::byte> full_data{};
                full_data.resize(this->get_max_register_size());

                written_size = this->emu_->read_register(real_reg.reg, full_data.data(), full_data.size());
                if (written_size < *real_reg.offset)
                {
                    return 0;
                }

                memcpy(full_data.data() + *real_reg.offset, data, written_size - *real_reg.offset);
                this->emu_->write_register(real_reg.reg, full_data.data(), written_size);
                written_size -= *real_reg.offset;
            }
            else
            {
                written_size = this->emu_->write_register(real_reg.reg, data, size);
            }

            return real_reg.expected_size.value_or(written_size);
        }
        catch (...)
        {
            return 0;
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

    std::string get_target_description(const std::string_view file) override
    {
        const auto entry = x64_target_descriptions.find(file);
        if (entry == x64_target_descriptions.end())
        {
            return {};
        }

        return entry->second;
    }

  private:
    x64_emulator* emu_{};

    using hook_map = std::unordered_map<breakpoint_key, scoped_hook>;
    utils::concurrency::container<hook_map> hooks_{};
};
