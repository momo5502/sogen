#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtReadVirtualMemory(const syscall_context& c, handle process, emulator_pointer address, emulator_pointer buffer,
                                        uint64_t size, emulator_object<uint64_t> transferred);
    NTSTATUS handle_NtWriteVirtualMemory(const syscall_context& c, handle process, emulator_pointer address, emulator_pointer buffer,
                                         uint64_t size, emulator_object<uint64_t> transferred);
}

namespace sogen::test
{
    namespace
    {
        class VirtualMemoryCopyTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                emulator_interfaces interfaces{};
                interfaces.audio = std::make_unique<null_audio_backend>();
                interfaces.console = std::make_unique<null_console_backend>();
                interfaces.ui = std::make_unique<null_ui_backend>();
                this->emu = std::make_unique<windows_emulator>(
                    create_x86_64_emulator_from_environment(),
                    emulator_settings{.disable_logging = true, .emulation_root = get_emulator_root(), .load_registry = false},
                    emulator_callbacks{}, std::move(interfaces));
                this->source = this->emu->memory.allocate_memory(0x1000, memory_permission::read_write);
                this->destination = this->emu->memory.allocate_memory(0x1000, memory_permission::read_write);
                this->storage = this->emu->memory.allocate_memory(0x1000, memory_permission::read_write);
            }

            syscall_context context()
            {
                return {.win_emu = *this->emu, .emu = this->emu->emu(), .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
            }

            std::unique_ptr<windows_emulator> emu{};
            uint64_t source{};
            uint64_t destination{};
            uint64_t storage{};
        };
    }

    TEST_F(VirtualMemoryCopyTest, ReadAndWriteReportFullWidthByteCounts)
    {
        const emulator_object<uint64_t> count{this->emu->memory, this->storage};
        const uint64_t value = 0x1122334455667788;
        this->emu->memory.write_memory(this->source, value);
        count.write(UINT64_MAX);
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtReadVirtualMemory(this->context(), CURRENT_PROCESS, this->source, this->destination,
                                                                       sizeof(value), count));
        EXPECT_EQ(sizeof(value), count.read());
        EXPECT_EQ(value, this->emu->memory.read_memory<uint64_t>(this->destination));
        count.write(UINT64_MAX);
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtWriteVirtualMemory(this->context(), CURRENT_PROCESS, this->destination, this->source,
                                                                        sizeof(value), count));
        EXPECT_EQ(sizeof(value), count.read());
    }

    TEST_F(VirtualMemoryCopyTest, RequestsAboveFourGiBDoNotTruncate)
    {
        const emulator_object<uint64_t> count{this->emu->memory, this->storage};
        constexpr uint64_t length = (1ULL << 32) + 8;
        EXPECT_EQ(STATUS_PARTIAL_COPY,
                  syscalls::handle_NtReadVirtualMemory(this->context(), CURRENT_PROCESS, this->source, this->destination, length, count));
        EXPECT_EQ(0x1000u, count.read());
        EXPECT_EQ(STATUS_PARTIAL_COPY,
                  syscalls::handle_NtWriteVirtualMemory(this->context(), CURRENT_PROCESS, this->destination, this->source, length, count));
        EXPECT_EQ(0x1000u, count.read());
    }

    TEST_F(VirtualMemoryCopyTest, ZeroLengthCopiesClearFullWidthCount)
    {
        const emulator_object<uint64_t> count{this->emu->memory, this->storage};
        count.write(UINT64_MAX);
        EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtReadVirtualMemory(this->context(), CURRENT_PROCESS, 0, 0, 0, count));
        EXPECT_EQ(0u, count.read());
        count.write(UINT64_MAX);
        EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtWriteVirtualMemory(this->context(), CURRENT_PROCESS, 0, 0, 0, count));
        EXPECT_EQ(0u, count.read());
    }
}
