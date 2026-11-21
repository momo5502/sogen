#include "std_include.hpp"

namespace sogen::syscalls
{
    NTSTATUS handle_NtQuerySystemInformation(const syscall_context& c, uint32_t info_class, uint64_t system_information,
                                             uint32_t system_information_length, emulator_object<uint32_t> return_length);
}

namespace sogen::test
{
    namespace
    {
        constexpr size_t peer_slot = 7;
        constexpr uint64_t peer_value = 0x5EED5EED5EED5EED;

        // Stands in for a guest thread on another vCPU: right after the emulator reads the TEB, that thread stores to one of
        // its TLS slots. Every other access goes straight to the emulator's memory.
        class peer_writing_memory final : public memory_interface
        {
          public:
            peer_writing_memory(memory_interface& memory, const uint64_t slot_address)
                : memory_(&memory),
                  slot_address_(slot_address)
            {
            }

            void read_memory(const uint64_t address, void* data, const size_t size) const override
            {
                this->memory_->read_memory(address, data, size);
                this->peer_writes_slot();
            }

            bool try_read_memory(const uint64_t address, void* data, const size_t size) const override
            {
                const auto read = this->memory_->try_read_memory(address, data, size);
                this->peer_writes_slot();
                return read;
            }

            void write_memory(const uint64_t address, const void* data, const size_t size) override
            {
                this->memory_->write_memory(address, data, size);
            }

            bool try_write_memory(const uint64_t address, const void* data, const size_t size) override
            {
                return this->memory_->try_write_memory(address, data, size);
            }

          private:
            memory_interface* memory_{};
            uint64_t slot_address_{};

            // The TEB object only reads and writes guest memory; mapping goes through the emulator's memory manager.
            void map_mmio(uint64_t, size_t, mmio_read_callback, mmio_write_callback) override
            {
                throw std::logic_error("not a memory manager");
            }

            void map_memory(uint64_t, size_t, memory_permission) override
            {
                throw std::logic_error("not a memory manager");
            }

            void unmap_memory(uint64_t, size_t) override
            {
                throw std::logic_error("not a memory manager");
            }

            void apply_memory_protection(uint64_t, size_t, memory_permission) override
            {
                throw std::logic_error("not a memory manager");
            }

            void peer_writes_slot() const
            {
                this->memory_->write_memory(this->slot_address_, &peer_value, sizeof(peer_value));
            }
        };

        class TebFieldAccessTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                emulator_interfaces interfaces{};
                interfaces.audio = std::make_unique<null_audio_backend>();
                interfaces.console = std::make_unique<null_console_backend>();
                interfaces.ui = std::make_unique<null_ui_backend>();
                this->emu =
                    std::make_unique<windows_emulator>(create_x86_64_emulator_from_environment(),
                                                       emulator_settings{.disable_logging = true, .emulation_root = get_emulator_root()},
                                                       emulator_callbacks{}, std::move(interfaces));
                this->querier = this->add_thread(8);
                this->peer = this->add_thread(12);
                this->emu->vcpu(0).active_thread = this->emu->process.threads.get(this->querier);

                const auto storage = this->emu->memory.allocate_memory(0x40000, memory_permission::read_write);
                this->allocator.emplace(this->emu->memory, storage, 0x40000);

                // The process list names the process after its image.
                this->executable.name = "test.exe";
                this->emu->mod_manager.executable = &this->executable;
            }

            void TearDown() override
            {
                this->emu->mod_manager.executable = nullptr;
            }

            syscall_context context()
            {
                return {.win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
            }

            handle add_thread(const uint32_t id)
            {
                return this->emu->process.threads.store(
                    emulator_thread{this->emu->memory, this->emu->process, 0, 0, 0x100000, 0, id, true});
            }

            std::unique_ptr<windows_emulator> emu{};
            std::optional<emulator_allocator> allocator{};
            mapped_module executable{};
            handle querier{};
            handle peer{};
        };

        TEST_F(TebFieldAccessTest, ProcessListQueryKeepsWhatAPeerThreadStoresInItsTeb)
        {
            auto& peer_thread = *this->emu->process.threads.get(this->peer);
            const auto teb_address = peer_thread.teb64->value();
            const auto slot_address = teb_address + offsetof(TEB64, TlsSlots) + peer_slot * sizeof(std::uint64_t);

            peer_writing_memory peer_memory{this->emu->memory, slot_address};
            peer_thread.teb64 = emulator_object<TEB64>{peer_memory, teb_address};

            constexpr uint32_t buffer_size = 0x20000;
            const auto buffer = this->allocator->reserve(buffer_size, 0x10);
            const auto return_length = this->allocator->reserve<uint32_t>();
            ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtQuerySystemInformation(this->context(), SystemProcessInformation, buffer,
                                                                                buffer_size, return_length));

            std::uint64_t slot{};
            this->emu->memory.read_memory(slot_address, &slot, sizeof(slot));
            EXPECT_EQ(slot, peer_value);
        }
    }
}
