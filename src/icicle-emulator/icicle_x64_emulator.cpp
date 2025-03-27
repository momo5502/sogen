#define ICICLE_EMULATOR_IMPL
#include "icicle_x64_emulator.hpp"

using icicle_emulator = struct icicle_emulator_;

extern "C"
{
    icicle_emulator* icicle_create_emulator();
    int32_t icicle_map_memory(icicle_emulator*, uint64_t address, uint64_t length, uint8_t permissions);
    int32_t icicle_unmap_memory(icicle_emulator*, uint64_t address, uint64_t length);
    void icicle_destroy_emulator(icicle_emulator*);
}

namespace icicle
{
    class icicle_x64_emulator : public x64_emulator
    {
      public:
        icicle_x64_emulator()
            : emu_(icicle_create_emulator())
        {
            if (!this->emu_)
            {
                throw std::runtime_error("Failed to create icicle emulator instance");
            }
        }

        ~icicle_x64_emulator() override
        {
            if (this->emu_)
            {
                icicle_destroy_emulator(this->emu_);
                this->emu_ = nullptr;
            }
        }

        void start(const uint64_t start, const uint64_t end, std::chrono::nanoseconds timeout,
                   const size_t count) override
        {
            if (timeout.count() < 0)
            {
                timeout = {};
            }
        }

        void stop() override
        {
        }

        size_t write_raw_register(const int reg, const void* value, const size_t size) override
        {
            throw std::runtime_error("Not implemented");
        }

        size_t read_raw_register(const int reg, void* value, const size_t size) override
        {
            throw std::runtime_error("Not implemented");
        }

        void map_mmio(const uint64_t address, const size_t size, mmio_read_callback read_cb,
                      mmio_write_callback write_cb) override
        {
            throw std::runtime_error("Not implemented");
        }

        void map_memory(const uint64_t address, const size_t size, memory_permission permissions) override
        {
            const auto res = icicle_map_memory(this->emu_, address, size, static_cast<uint8_t>(permissions));
            if (!res)
            {
                throw std::runtime_error("Failed to map memory");
            }
        }

        void unmap_memory(const uint64_t address, const size_t size) override
        {
            const auto res = icicle_unmap_memory(this->emu_, address, size);
            if (!res)
            {
                throw std::runtime_error("Failed to map memory");
            }
        }

        bool try_read_memory(const uint64_t address, void* data, const size_t size) const override
        {
            throw std::runtime_error("Not implemented");
        }

        void read_memory(const uint64_t address, void* data, const size_t size) const override
        {
            if (!this->try_read_memory(address, data, size))
            {
                throw std::runtime_error("Failed to read memory");
            }
        }

        void write_memory(const uint64_t address, const void* data, const size_t size) override
        {
            throw std::runtime_error("Not implemented");
        }

        void apply_memory_protection(const uint64_t address, const size_t size, memory_permission permissions) override
        {
            throw std::runtime_error("Not implemented");
        }

        emulator_hook* hook_instruction(int instruction_type, instruction_hook_callback callback) override
        {
            throw std::runtime_error("Not implemented");
        }

        emulator_hook* hook_basic_block(basic_block_hook_callback callback) override
        {
            throw std::runtime_error("Not implemented");
        }

        emulator_hook* hook_edge_generation(edge_generation_hook_callback callback) override
        {
            throw std::runtime_error("Not implemented");
        }

        emulator_hook* hook_interrupt(interrupt_hook_callback callback) override
        {
            throw std::runtime_error("Not implemented");
        }

        emulator_hook* hook_memory_violation(uint64_t address, size_t size,
                                             memory_violation_hook_callback callback) override
        {
            throw std::runtime_error("Not implemented");
        }

        emulator_hook* hook_memory_access(const uint64_t address, const size_t size, const memory_operation filter,
                                          complex_memory_hook_callback callback) override
        {
            if (filter == memory_permission::none)
            {
                return nullptr;
            }

            throw std::runtime_error("Not implemented");
        }

        void delete_hook(emulator_hook* hook) override
        {
            throw std::runtime_error("Not implemented");
        }

        void serialize_state(utils::buffer_serializer& buffer, const bool is_snapshot) const override
        {
            throw std::runtime_error("Not implemented");
        }

        void deserialize_state(utils::buffer_deserializer& buffer, const bool is_snapshot) override
        {
            throw std::runtime_error("Not implemented");
        }

        std::vector<std::byte> save_registers() override
        {
            throw std::runtime_error("Not implemented");
        }

        void restore_registers(const std::vector<std::byte>& register_data) override
        {
            throw std::runtime_error("Not implemented");
        }

        bool has_violation() const override
        {
            return false;
        }

      private:
        icicle_emulator* emu_{};
    };

    std::unique_ptr<x64_emulator> create_x64_emulator()
    {
        return std::make_unique<icicle_x64_emulator>();
    }
}
