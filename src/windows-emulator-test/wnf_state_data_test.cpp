#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtQueryWnfStateData(const syscall_context&, emulator_object<uint64_t> state_name, emulator_pointer type_id,
                                        emulator_pointer explicit_scope, emulator_object<ULONG> change_stamp, emulator_pointer buffer,
                                        emulator_object<ULONG> buffer_size);
}

namespace sogen::test
{
    namespace
    {
        constexpr ULONG untouched = 0xAAAAAAAA;
        constexpr uint64_t registered_name = 0x00840B3AA3BC0875;
        constexpr uint64_t version_zero_name = 0x41C64E6DA3BC0074;
        constexpr uint64_t bogus_name = 0x0123456789ABCDEF;
        constexpr uint64_t data_scope_six_name = registered_name ^ (6ULL << 6);

        struct present_pointers
        {
            bool name{true};
            bool change_stamp{true};
            bool buffer_size{true};
            bool buffer{true};
        };

        struct query_result
        {
            NTSTATUS status{};
            ULONG change_stamp{};
            ULONG buffer_size{};
            ULONG buffer_head{};
        };

        class WnfStateDataTest : public testing::Test
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
                this->storage = this->emu->memory.allocate_memory(0x2000, memory_permission::read_write);
            }

            syscall_context context()
            {
                return {.win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
            }

            uint64_t buffer_address() const
            {
                return this->storage + 0x1000;
            }

            query_result query(const uint64_t name, const ULONG size, const present_pointers present = {})
            {
                const emulator_object<uint64_t> state_name{this->emu->memory, this->storage};
                const emulator_object<ULONG> change_stamp{this->emu->memory, this->storage + 0x10};
                const emulator_object<ULONG> buffer_size{this->emu->memory, this->storage + 0x20};
                const emulator_object<ULONG> buffer_head{this->emu->memory, this->buffer_address()};
                state_name.write(name);
                change_stamp.write(untouched);
                buffer_size.write(size);
                buffer_head.write(untouched);

                const auto status = syscalls::handle_NtQueryWnfStateData(
                    this->context(), present.name ? state_name : emulator_object<uint64_t>{this->emu->memory}, 0, 0,
                    present.change_stamp ? change_stamp : emulator_object<ULONG>{this->emu->memory},
                    present.buffer ? this->buffer_address() : 0,
                    present.buffer_size ? buffer_size : emulator_object<ULONG>{this->emu->memory});

                return {.status = status,
                        .change_stamp = change_stamp.read(),
                        .buffer_size = buffer_size.read(),
                        .buffer_head = buffer_head.read()};
            }

            std::unique_ptr<windows_emulator> emu{};
            uint64_t storage{};
        };
    }

    TEST_F(WnfStateDataTest, WellFormedNameReadsAsNeverPublished)
    {
        for (const ULONG size : {0x1000u, 0x44u, 1u, 0u})
        {
            const auto result = this->query(registered_name, size);
            EXPECT_EQ(STATUS_SUCCESS, result.status) << size;
            EXPECT_EQ(0u, result.change_stamp) << size;
            EXPECT_EQ(0u, result.buffer_size) << size;
            EXPECT_EQ(untouched, result.buffer_head) << size;
        }
    }

    TEST_F(WnfStateDataTest, NullBufferIsAcceptedWhenItsSizeIsZero)
    {
        const auto result = this->query(registered_name, 0, {.buffer = false});
        EXPECT_EQ(STATUS_SUCCESS, result.status);
        EXPECT_EQ(0u, result.change_stamp);
        EXPECT_EQ(0u, result.buffer_size);
    }

    TEST_F(WnfStateDataTest, MalformedNameIsRejectedBeforeTheOutputPointers)
    {
        for (const uint64_t name : {version_zero_name, bogus_name, data_scope_six_name})
        {
            const auto result = this->query(name, 0x1000);
            EXPECT_EQ(STATUS_INVALID_PARAMETER, result.status) << std::hex << name;
            EXPECT_EQ(untouched, result.change_stamp) << std::hex << name;
            EXPECT_EQ(0x1000u, result.buffer_size) << std::hex << name;

            EXPECT_EQ(STATUS_INVALID_PARAMETER, this->query(name, 0x1000, {.change_stamp = false}).status) << std::hex << name;
            EXPECT_EQ(STATUS_INVALID_PARAMETER, this->query(name, 0x1000, {.buffer_size = false}).status) << std::hex << name;
        }
    }

    TEST_F(WnfStateDataTest, MissingPointersAreAccessViolationsAndWriteNothing)
    {
        const auto no_name = this->query(registered_name, 0x1000, {.name = false});
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, no_name.status);
        EXPECT_EQ(untouched, no_name.change_stamp);
        EXPECT_EQ(0x1000u, no_name.buffer_size);

        const auto no_stamp = this->query(registered_name, 0x1000, {.change_stamp = false});
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, no_stamp.status);
        EXPECT_EQ(0x1000u, no_stamp.buffer_size);

        const auto no_size = this->query(registered_name, 0x1000, {.buffer_size = false});
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, no_size.status);
        EXPECT_EQ(untouched, no_size.change_stamp);

        const auto no_buffer = this->query(registered_name, 0x1000, {.buffer = false});
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, no_buffer.status);
        EXPECT_EQ(untouched, no_buffer.change_stamp);
        EXPECT_EQ(0x1000u, no_buffer.buffer_size);
    }
}
