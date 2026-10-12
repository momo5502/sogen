#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtGetWriteWatch(const syscall_context& c, handle process_handle, ULONG flags, uint64_t base_address,
                                    uint64_t region_size, uint64_t addresses, emulator_object<uint64_t> count,
                                    emulator_object<ULONG> granularity);
    NTSTATUS handle_NtResetWriteWatch(const syscall_context& c, handle process_handle, uint64_t base_address, uint64_t region_size);
}

namespace sogen::test
{
    namespace
    {
        class WriteWatchTest : public testing::Test
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
                this->base = this->emu->memory.allocate_memory(0x4000, memory_permission::read_write);
                ASSERT_NE(0, this->base);
                ASSERT_TRUE(this->emu->memory.enable_write_watch(this->base));
            }

            std::vector<uint64_t> query(uint64_t address, size_t size, size_t capacity = 4, bool reset = false)
            {
                std::vector<uint64_t> pages{};
                EXPECT_TRUE(this->emu->memory.query_write_watch(address, size, capacity, reset, pages));
                return pages;
            }

            std::unique_ptr<windows_emulator> emu{};
            uint64_t base{};
        };
    }

    TEST_F(WriteWatchTest, TracksSameValueWritesAndWritesBypassingMemoryManager)
    {
        EXPECT_TRUE(this->query(this->base, 0x4000).empty());
        this->emu->emu().write_memory(this->base, uint8_t{});
        this->emu->memory.write_memory(this->base + 0x2000, uint8_t{1});
        EXPECT_EQ((std::vector<uint64_t>{this->base, this->base + 0x2000}), this->query(this->base, 0x4000));
        ASSERT_TRUE(this->emu->memory.reset_write_watch(this->base, 0x4000));
        this->emu->emu().write_memory(this->base, uint8_t{});
        EXPECT_EQ((std::vector<uint64_t>{this->base}), this->query(this->base, 0x4000));
    }

    TEST_F(WriteWatchTest, TruncatedResetOnlyClearsReturnedPages)
    {
        this->emu->emu().write_memory(this->base, uint8_t{});
        this->emu->emu().write_memory(this->base + 0x2000, uint8_t{});
        EXPECT_EQ((std::vector<uint64_t>{this->base}), this->query(this->base, 0x4000, 1, true));
        EXPECT_EQ((std::vector<uint64_t>{this->base + 0x2000}), this->query(this->base, 0x4000));
    }

    TEST_F(WriteWatchTest, SubrangesRoundToPagesAndResetIndependently)
    {
        const std::array<uint8_t, 2> bytes{};
        this->emu->emu().write_memory(this->base + 0xfff, bytes.data(), bytes.size());
        EXPECT_EQ((std::vector<uint64_t>{this->base, this->base + 0x1000}), this->query(this->base, 0x4000));
        ASSERT_TRUE(this->emu->memory.reset_write_watch(this->base + 1, 1));
        EXPECT_EQ((std::vector<uint64_t>{this->base + 0x1000}), this->query(this->base, 0x4000));
        EXPECT_EQ((std::vector<uint64_t>{this->base + 0x1000}), this->query(this->base + 0x1001, 1));
    }

    TEST_F(WriteWatchTest, WriteFaultsRespectGuestProtectionAndRearmOnReset)
    {
        ASSERT_TRUE(this->emu->memory.handle_write_watch_fault(this->base));
        EXPECT_FALSE(this->emu->memory.handle_write_watch_fault(this->base));
        EXPECT_EQ(memory_permission::read_write, this->emu->memory.get_region_info(this->base).permissions.common);
        ASSERT_TRUE(this->emu->memory.reset_write_watch(this->base, 0x1000));
        ASSERT_TRUE(this->emu->memory.protect_memory(this->base, 0x1000, memory_permission::read));
        EXPECT_FALSE(this->emu->memory.handle_write_watch_fault(this->base));
        ASSERT_TRUE(this->emu->memory.protect_memory(this->base, 0x1000,
                                                     nt_memory_permission{memory_permission::read_write, memory_permission_ext::guard}));
        EXPECT_FALSE(this->emu->memory.handle_write_watch_fault(this->base));
        ASSERT_TRUE(this->emu->memory.protect_memory(this->base, 0x1000, memory_permission::read_write));
        EXPECT_TRUE(this->emu->memory.handle_write_watch_fault(this->base));
    }

    TEST_F(WriteWatchTest, DecommitDiscardsDirtyStateAndRecommitStartsClean)
    {
        this->emu->emu().write_memory(this->base, uint8_t{1});
        ASSERT_TRUE(this->emu->memory.decommit_memory(this->base, 0x1000));
        EXPECT_TRUE(this->query(this->base, 0x4000).empty());
        ASSERT_TRUE(this->emu->memory.commit_memory(this->base, 0x1000, memory_permission::read_write));
        EXPECT_TRUE(this->query(this->base, 0x4000).empty());
        EXPECT_TRUE(this->emu->memory.handle_write_watch_fault(this->base));
    }

    TEST_F(WriteWatchTest, RejectsUnwatchedAndOutOfBoundsRanges)
    {
        std::vector<uint64_t> pages{};
        EXPECT_FALSE(this->emu->memory.query_write_watch(this->base, 0, 4, false, pages));
        EXPECT_FALSE(this->emu->memory.query_write_watch(this->base + 1, 0x4000, 4, false, pages));
        EXPECT_FALSE(this->emu->memory.reset_write_watch(this->base + 1, 0x4000));
        ASSERT_TRUE(this->emu->memory.release_memory(this->base, 0));
        EXPECT_FALSE(this->emu->memory.query_write_watch(this->base, 0x4000, 4, false, pages));
        ASSERT_TRUE(this->emu->memory.allocate_memory(this->base, 0x4000, memory_permission::read_write));
        EXPECT_FALSE(this->emu->memory.query_write_watch(this->base, 0x4000, 4, false, pages));
    }

    TEST_F(WriteWatchTest, SerializationPreservesDirtyPagesWithoutCountingRestoreWrites)
    {
        this->emu->emu().write_memory(this->base + 0x2000, uint8_t{1});
        utils::buffer_serializer buffer{};
        this->emu->memory.serialize_memory_state(buffer, false);
        utils::buffer_deserializer reader{buffer.get_buffer()};
        this->emu->memory.unmap_all_memory();
        this->emu->memory.deserialize_memory_state(reader, false);
        EXPECT_EQ((std::vector<uint64_t>{this->base + 0x2000}), this->query(this->base, 0x4000));
        EXPECT_TRUE(this->emu->memory.handle_write_watch_fault(this->base));
    }

    TEST_F(WriteWatchTest, SyscallReportsGranularityAndValidatesFlagsAndCapacity)
    {
        const auto storage = this->emu->memory.allocate_memory(0x1000, memory_permission::read_write);
        const emulator_object<uint64_t> count{this->emu->memory, storage};
        const emulator_object<ULONG> granularity{this->emu->memory, storage + 8};
        const syscall_context context{
            .win_emu = *this->emu, .emu = this->emu->emu(), .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
        const auto query = [&](const ULONG flags, const uint64_t size) {
            return syscalls::handle_NtGetWriteWatch(context, CURRENT_PROCESS, flags, this->base, size, storage + 0x100, count, granularity);
        };
        count.write(0);
        EXPECT_EQ(STATUS_INVALID_PARAMETER_5, query(0, 0x4000));
        count.write(4);
        EXPECT_EQ(STATUS_INVALID_PARAMETER_2, query(2, 0x4000));
        EXPECT_EQ(STATUS_INVALID_PARAMETER_4, query(0, 0));
        EXPECT_EQ(STATUS_INVALID_PARAMETER_1, query(0, 0x4001));
        this->emu->emu().write_memory(this->base + 0x2000, uint8_t{1});
        ASSERT_EQ(STATUS_SUCCESS, query(WRITE_WATCH_FLAG_RESET, 0x4000));
        EXPECT_EQ(1, count.read());
        EXPECT_EQ(0x1000, granularity.read());
        EXPECT_EQ(this->base + 0x2000, (emulator_object<uint64_t>{this->emu->memory, storage + 0x100}.read()));
        EXPECT_TRUE(this->query(this->base, 0x4000).empty());
        EXPECT_EQ(STATUS_INVALID_PARAMETER_3, syscalls::handle_NtResetWriteWatch(context, CURRENT_PROCESS, this->base, 0));
        ASSERT_TRUE(this->emu->memory.release_memory(storage, 0));
    }
}
