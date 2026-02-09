#pragma once
#include <gdb_stub.hpp>
#include <scoped_hook.hpp>
#include <arch_emulator.hpp>

#include <utils/concurrency.hpp>

#include "x64_register_mapping.hpp"
#include "x64_target_descriptions.hpp"

struct breakpoint_key
{
    uint64_t addr{};
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
        return ((std::hash<uint64_t>()(k.addr) ^ (std::hash<size_t>()(k.size) << 1)) >> 1) ^
               (std::hash<size_t>()(static_cast<size_t>(k.type)) << 1);
    }
};

class x64_gdb_stub_handler : public gdb_stub::debugging_handler
{
  public:
    x64_gdb_stub_handler(x86_64_emulator& emu)
        : emu_(&emu)
    {
    }

    ~x64_gdb_stub_handler() override = default;

    gdb_stub::action run() override
    {
        try
        {
            this->emu_->start();
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
            this->emu_->start(1);
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
                auto hook_vector = this->create_hook(type, addr, size);

                hooks[{addr, size, type}] = scoped_hook(*this->emu_, std::move(hook_vector));

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

    uint32_t get_current_thread_id() override
    {
        return 1;
    }

    std::vector<uint32_t> get_thread_ids() override
    {
        return {this->get_current_thread_id()};
    }

  private:
    x86_64_emulator* emu_{};

    using hook_map = std::unordered_map<breakpoint_key, scoped_hook>;
    utils::concurrency::container<hook_map> hooks_{};

    std::vector<emulator_hook*> create_execute_hook(const uint64_t addr, const size_t size)
    {
        std::vector<emulator_hook*> hooks{};
        hooks.reserve(size);

        for (size_t i = 0; i < size; ++i)
        {
            auto* hook = this->emu_->hook_memory_execution(addr + i, [this](const uint64_t) {
                this->on_interrupt(); //
            });

            hooks.push_back(hook);
        }

        return hooks;
    }

    std::vector<emulator_hook*> create_read_hook(const uint64_t addr, const size_t size)
    {
        auto* hook = this->emu_->hook_memory_read(addr, size, [this](const uint64_t, const void*, const size_t) {
            this->on_interrupt(); //
        });

        return {hook};
    }

    std::vector<emulator_hook*> create_write_hook(const uint64_t addr, const size_t size)
    {
        auto* hook = this->emu_->hook_memory_write(addr, size, [this](const uint64_t, const void*, const size_t) {
            this->on_interrupt(); //
        });

        return {hook};
    }

    std::vector<emulator_hook*> create_hook(const gdb_stub::breakpoint_type type, const uint64_t addr, const size_t size)
    {
        using enum gdb_stub::breakpoint_type;

        switch (type)
        {
        case software:
        case hardware_exec:
            return this->create_execute_hook(addr, size);
        case hardware_read:
            return this->create_read_hook(addr, size);
        case hardware_write:
            return this->create_write_hook(addr, size);
        case hardware_read_write: {
            auto hooks1 = this->create_hook(hardware_read, addr, size);
            auto hooks2 = this->create_hook(hardware_write, addr, size);
            hooks1.insert(hooks1.end(), hooks2.begin(), hooks2.end());
            return hooks1;
        }
        default:
            throw std::runtime_error("Bad bp type");
        }
    }
};
