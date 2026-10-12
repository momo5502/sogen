#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtSetInformationWorkerFactory(const syscall_context& c, handle worker_factory_handle, WORKERFACTORYINFOCLASS info_class,
                                                  uint64_t information, ULONG information_length);
}

namespace sogen::test
{
    namespace
    {
        class WorkerFactoryTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                emulator_interfaces interfaces{};
                interfaces.ui = std::make_unique<null_ui_backend>();
                interfaces.audio = std::make_unique<null_audio_backend>();
                interfaces.console = std::make_unique<null_console_backend>();
                this->emu = std::make_unique<windows_emulator>(
                    create_x86_64_emulator_from_environment(),
                    emulator_settings{.disable_logging = true, .emulation_root = get_emulator_root(), .load_registry = false},
                    emulator_callbacks{}, std::move(interfaces));
                this->storage = this->emu->memory.allocate_memory(0x1000, memory_permission::read_write);
                worker_factory factory{};
                factory.stack_reserve = 0x100000;
                factory.stack_commit = 0x1000;
                this->factory_handle = this->emu->process.worker_factories.store(std::move(factory));
            }

            NTSTATUS set_stack(uint64_t address, ULONG size)
            {
                const syscall_context c{
                    .win_emu = *this->emu, .emu = this->emu->emu(), .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
                return syscalls::handle_NtSetInformationWorkerFactory(c, this->factory_handle, WorkerFactoryStackInformation, address,
                                                                      size);
            }

            std::unique_ptr<windows_emulator> emu{};
            uint64_t storage{};
            handle factory_handle{};
        };
    }

    TEST_F(WorkerFactoryTest, UpdatesStackSizesIncludingZeroAndUnalignedSizes)
    {
        for (const auto sizes : {std::array<uint64_t, 2>{0x200000, 0x2000}, std::array<uint64_t, 2>{0, 0},
                                 std::array<uint64_t, 2>{0x1000, 0x2000}, std::array<uint64_t, 2>{1, 1}})
        {
            emulator_object<std::array<uint64_t, 2>>{this->emu->memory, this->storage}.write(sizes);
            ASSERT_EQ(STATUS_SUCCESS, this->set_stack(this->storage, sizeof(sizes)));
            const auto* factory = this->emu->process.worker_factories.get(this->factory_handle);
            ASSERT_NE(nullptr, factory);
            EXPECT_EQ(sizes[0], factory->stack_reserve);
            EXPECT_EQ(sizes[1], factory->stack_commit);
        }
    }

    TEST_F(WorkerFactoryTest, RejectsWrongLengthAndUnreadableBufferWithoutChangingStackSizes)
    {
        for (const ULONG length : {0u, 8u, 15u, 24u})
        {
            EXPECT_EQ(STATUS_INFO_LENGTH_MISMATCH, this->set_stack(this->storage, length));
        }
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->set_stack(0x12345678, 16));
        const auto* factory = this->emu->process.worker_factories.get(this->factory_handle);
        ASSERT_NE(nullptr, factory);
        EXPECT_EQ(0x100000u, factory->stack_reserve);
        EXPECT_EQ(0x1000u, factory->stack_commit);
    }
}
