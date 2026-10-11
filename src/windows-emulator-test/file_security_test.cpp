#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>
#include <utils/io.hpp>

#ifdef OS_WINDOWS
namespace sogen::syscalls
{
    NTSTATUS handle_NtQuerySecurityObject(const syscall_context& c, handle h, SECURITY_INFORMATION security_information,
                                          emulator_pointer security_descriptor, ULONG length, emulator_object<ULONG> length_needed);
}

namespace sogen::test
{
    namespace
    {
        class FileSecurityTest : public testing::Test
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
                this->path = std::filesystem::current_path();
                file entry{};
                entry.host_path = this->path;
                this->directory = this->emu->process.files.store(std::move(entry));
            }

            NTSTATUS query_owner(uint32_t size)
            {
                const syscall_context context{
                    .win_emu = *this->emu, .emu = this->emu->emu(), .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
                return syscalls::handle_NtQuerySecurityObject(context, this->directory, OWNER_SECURITY_INFORMATION, this->storage + 0x100,
                                                              size, {this->emu->memory, this->storage});
            }

            std::unique_ptr<windows_emulator> emu{};
            std::filesystem::path path{};
            handle directory{};
            uint64_t storage{};
        };
    }

    TEST_F(FileSecurityTest, ReturnsActualHostOwner)
    {
        const auto expected = utils::io::get_file_owner(this->path);
        ASSERT_FALSE(expected.empty());
        ASSERT_EQ(STATUS_SUCCESS, this->query_owner(256));
        const auto descriptor = emulator_object<SECURITY_DESCRIPTOR_RELATIVE>{this->emu->memory, this->storage + 0x100}.read();
        EXPECT_EQ(SECURITY_DESCRIPTOR_REVISION, descriptor.Revision);
        EXPECT_NE(0, descriptor.Control & SE_SELF_RELATIVE);
        EXPECT_EQ(sizeof(descriptor), descriptor.Owner);
        std::vector<uint8_t> owner(expected.size());
        this->emu->memory.read_memory(this->storage + 0x100 + descriptor.Owner, owner.data(), owner.size());
        EXPECT_EQ(expected, owner);
        EXPECT_EQ(sizeof(descriptor) + owner.size(), (emulator_object<ULONG>{this->emu->memory, this->storage}.read()));
    }

    TEST_F(FileSecurityTest, ReportsSpaceRequiredForActualOwner)
    {
        const auto owner = utils::io::get_file_owner(this->path);
        ASSERT_FALSE(owner.empty());
        EXPECT_EQ(STATUS_BUFFER_TOO_SMALL, this->query_owner(0));
        EXPECT_EQ(sizeof(SECURITY_DESCRIPTOR_RELATIVE) + owner.size(), (emulator_object<ULONG>{this->emu->memory, this->storage}.read()));
    }

    TEST_F(FileSecurityTest, DoesNotInventOwnerWhenHostLookupFails)
    {
        this->emu->process.files.get(this->directory)->host_path.clear();
        EXPECT_EQ(STATUS_ACCESS_DENIED, this->query_owner(256));
    }
}
#endif
