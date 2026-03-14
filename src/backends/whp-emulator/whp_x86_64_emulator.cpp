#define WHP_EMULATOR_IMPL
#include "whp_x86_64_emulator.hpp"

namespace unicorn
{
    namespace
    {

        class whp_x86_64_emulator : public x86_64_emulator
        {
          public:
            whp_x86_64_emulator()
            {
            }

            ~whp_x86_64_emulator() override
            {
            }

            void start(const size_t count) override
            {
                throw std::runtime_error("Not implemented");
            }

            void stop() override
            {
                throw std::runtime_error("Not implemented");
            }

            void load_gdt(const pointer_type address, const uint32_t limit) override
            {
                throw std::runtime_error("Not implemented");
            }

            void set_segment_base(const x86_register base, const pointer_type value) override
            {
                throw std::runtime_error("Not implemented");
            }

            pointer_type get_segment_base(const x86_register base) override
            {
                throw std::runtime_error("Not implemented");
            }

            size_t write_raw_register(const int reg, const void* value, const size_t size) override
            {
                throw std::runtime_error("Not implemented");
            }

            size_t read_raw_register(const int reg, void* value, const size_t size) override
            {
                throw std::runtime_error("Not implemented");
            }

            bool read_descriptor_table(const int reg, descriptor_table_register& table) override
            {
                throw std::runtime_error("Not implemented");
            }

            void map_mmio(const uint64_t address, const size_t size, mmio_read_callback read_cb, mmio_write_callback write_cb) override
            {
                throw std::runtime_error("Not implemented");
            }

            void map_memory(const uint64_t address, const size_t size, memory_permission permissions) override
            {
                throw std::runtime_error("Not implemented");
            }

            void unmap_memory(const uint64_t address, const size_t size) override
            {
                throw std::runtime_error("Not implemented");
            }

            bool try_read_memory(const uint64_t address, void* data, const size_t size) const override
            {
                throw std::runtime_error("Not implemented");
            }

            void read_memory(const uint64_t address, void* data, const size_t size) const override
            {
                throw std::runtime_error("Not implemented");
            }

            bool try_write_memory(const uint64_t address, const void* data, const size_t size) override
            {
                throw std::runtime_error("Not implemented");
            }

            void write_memory(const uint64_t address, const void* data, const size_t size) override
            {
                throw std::runtime_error("Not implemented");
            }

            void apply_memory_protection(const uint64_t address, const size_t size, memory_permission permissions) override
            {
                throw std::runtime_error("Not implemented");
            }

            emulator_hook* hook_instruction(const int instruction_type, instruction_hook_callback callback) override
            {
                throw std::runtime_error("Not implemented");
            }

            emulator_hook* hook_basic_block(basic_block_hook_callback callback) override
            {
                throw std::runtime_error("Not implemented");
            }

            emulator_hook* hook_interrupt(interrupt_hook_callback callback) override
            {
                throw std::runtime_error("Not implemented");
            }

            emulator_hook* hook_memory_violation(memory_violation_hook_callback callback) override
            {
                throw std::runtime_error("Not implemented");
            }

            emulator_hook* hook_memory_execution(const uint64_t address, const uint64_t size, memory_execution_hook_callback callback)
            {
                throw std::runtime_error("Not implemented");
            }

            emulator_hook* hook_memory_execution(memory_execution_hook_callback callback) override
            {
                throw std::runtime_error("Not implemented");
            }

            emulator_hook* hook_memory_execution(const uint64_t address, memory_execution_hook_callback callback) override
            {
                throw std::runtime_error("Not implemented");
            }

            emulator_hook* hook_memory_read(const uint64_t address, const uint64_t size, memory_access_hook_callback callback) override
            {
                throw std::runtime_error("Not implemented");
            }

            emulator_hook* hook_memory_write(const uint64_t address, const uint64_t size, memory_access_hook_callback callback) override
            {
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

            std::vector<std::byte> save_registers() const override
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

            std::string get_name() const override
            {
                return "Windows Hypervisor Platform";
            }

          private:
        };
    }

    std::unique_ptr<x86_64_emulator> create_x86_64_emulator()
    {
        return std::make_unique<whp_x86_64_emulator>();
    }
}
