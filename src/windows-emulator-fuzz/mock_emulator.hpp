// SPIKE — minimal x86_64_emulator backend for headless fuzzing.
//
// A guest-memory arena with NO code execution. memory_manager does the region bookkeeping and
// calls map_memory/read_memory/write_memory here; everything CPU/hook/execution-related is a
// stub because a device-fuzz harness never runs the guest. Deterministic, no native deps
// (no unicorn), ASAN-friendly.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include <arch_emulator.hpp>

namespace sogen::mock
{
    class mock_x86_64_emulator : public x86_64_emulator
    {
      public:
        // --- emulator ---
        std::string get_name() const override
        {
            return "mock";
        }

        bool supports_multiple_vcpus() const override
        {
            return false;
        }

        void serialize_state(utils::buffer_serializer&, bool) const override
        {
        }

        void deserialize_state(utils::buffer_deserializer&, bool) override
        {
        }

        // --- memory (the only real logic) ---
        void read_memory(uint64_t address, void* data, size_t size) const override
        {
            if (!this->try_read_memory(address, data, size))
            {
                throw std::runtime_error("mock: read from unmapped guest memory");
            }
        }

        bool try_read_memory(uint64_t address, void* data, size_t size) const override
        {
            const region* r = nullptr;
            size_t off = 0;
            if (!this->locate(address, size, r, off))
            {
                return false;
            }
            std::memcpy(data, r->data.data() + off, size);
            return true;
        }

        void write_memory(uint64_t address, const void* data, size_t size) override
        {
            if (!this->try_write_memory(address, data, size))
            {
                throw std::runtime_error("mock: write to unmapped guest memory");
            }
        }

        bool try_write_memory(uint64_t address, const void* data, size_t size) override
        {
            const region* r = nullptr;
            size_t off = 0;
            if (!this->locate(address, size, r, off))
            {
                return false;
            }
            // r came from a const locator but the region is owned by this (non-const) call.
            std::memcpy(const_cast<std::byte*>(r->data.data()) + off, data, size);
            return true;
        }

        // --- cpu ---
        bool read_descriptor_table(int, descriptor_table_register&) override
        {
            return false;
        }

        void start(size_t = 0) override
        {
            throw std::runtime_error("mock: cannot execute code");
        }

        void stop() override
        {
        }

        size_t read_raw_register(int reg, void* value, size_t size) override
        {
            std::memset(value, 0, size);
            const auto it = this->registers_.find(reg);
            if (it != this->registers_.end())
            {
                std::memcpy(value, it->second.data(), std::min(size, it->second.size()));
            }
            return size;
        }

        size_t write_raw_register(int reg, const void* value, size_t size) override
        {
            const auto* bytes = static_cast<const std::byte*>(value);
            this->registers_[reg].assign(bytes, bytes + size);
            return size;
        }

        std::vector<std::byte> save_registers() const override
        {
            return {};
        }

        void restore_registers(const std::vector<std::byte>&) override
        {
        }

        bool has_violation() const override
        {
            return false;
        }

        bool supports_instruction_counting() const override
        {
            return false;
        }

        bool is_stop_thread_safe() const override
        {
            return true;
        }

        // --- x86 segment / gdt ---
        void set_segment_base(register_type reg, pointer_type value) override
        {
            this->segment_bases_[static_cast<int>(reg)] = value;
        }

        pointer_type get_segment_base(register_type reg) override
        {
            const auto it = this->segment_bases_.find(static_cast<int>(reg));
            return it == this->segment_bases_.end() ? 0 : it->second;
        }

        void load_gdt(pointer_type, uint32_t) override
        {
        }

        // --- hooks: never fire (no execution), so they do nothing. The interface requires overrides;
        //     return null and ignore deletes. setup_hooks() discards the returned handles. ---
        emulator_hook* hook_memory_execution(memory_execution_hook_callback) override
        {
            return nullptr;
        }

        emulator_hook* hook_memory_execution(uint64_t, memory_execution_hook_callback) override
        {
            return nullptr;
        }

        emulator_hook* hook_memory_range_execution(uint64_t, uint64_t, memory_execution_hook_callback) override
        {
            return nullptr;
        }

        emulator_hook* hook_memory_read(uint64_t, uint64_t, memory_access_hook_callback) override
        {
            return nullptr;
        }

        emulator_hook* hook_memory_write(uint64_t, uint64_t, memory_access_hook_callback) override
        {
            return nullptr;
        }

        emulator_hook* hook_instruction(int, instruction_hook_callback) override
        {
            return nullptr;
        }

        emulator_hook* hook_interrupt(interrupt_hook_callback) override
        {
            return nullptr;
        }

        emulator_hook* hook_memory_violation(memory_violation_hook_callback) override
        {
            return nullptr;
        }

        emulator_hook* hook_basic_block(basic_block_hook_callback) override
        {
            return nullptr;
        }

        void delete_hook(emulator_hook*) override
        {
        }

      private:
        struct region
        {
            size_t size;
            std::vector<std::byte> data;
        };

        std::map<uint64_t, region> regions_;
        std::unordered_map<int, std::vector<std::byte>> registers_;
        std::unordered_map<int, pointer_type> segment_bases_;

        // Find the mapped region containing [address, address+size); returns the region and the offset.
        bool locate(uint64_t address, size_t size, const region*& out_region, size_t& out_offset) const
        {
            if (size == 0)
            {
                // Point any zero-length access at the first region if present; harmless.
                if (this->regions_.empty())
                {
                    return false;
                }
            }
            auto it = this->regions_.upper_bound(address);
            if (it == this->regions_.begin())
            {
                return false;
            }
            --it;
            const uint64_t base = it->first;
            if (address < base)
            {
                return false;
            }
            const size_t off = static_cast<size_t>(address - base);
            if (off > it->second.size || size > it->second.size - off)
            {
                return false;
            }
            out_region = &it->second;
            out_offset = off;
            return true;
        }

        // --- memory_interface private virtuals (region bookkeeping is memory_manager's job) ---
        void map_mmio(uint64_t, size_t, mmio_read_callback, mmio_write_callback) override
        {
            throw std::runtime_error("mock: mmio not supported");
        }

        void map_memory(uint64_t address, size_t size, memory_permission) override
        {
            this->regions_[address] = region{size, std::vector<std::byte>(size)};
        }

        void unmap_memory(uint64_t address, size_t) override
        {
            this->regions_.erase(address);
        }

        void apply_memory_protection(uint64_t, size_t, memory_permission) override
        {
        }
    };

    inline std::unique_ptr<x86_64_emulator> make_mock_emulator()
    {
        return std::make_unique<mock_x86_64_emulator>();
    }
}
