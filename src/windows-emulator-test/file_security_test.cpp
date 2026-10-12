#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtQuerySecurityObject(const syscall_context& c, handle h, SECURITY_INFORMATION security_information,
                                          emulator_pointer security_descriptor, ULONG length, emulator_object<ULONG> length_needed);
    NTSTATUS handle_NtQueryInformationToken(const syscall_context& c, handle token_handle, TOKEN_INFORMATION_CLASS information_class,
                                            uint64_t information, ULONG information_length, emulator_object<ULONG> return_length);
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
                this->emu->process.sid = {0x01, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x15, 0x00, 0x00, 0x00, 0x6F, 0x00,
                                          0x00, 0x00, 0xDE, 0x00, 0x00, 0x00, 0x4D, 0x01, 0x00, 0x00, 0xE9, 0x03, 0x00, 0x00};
                file entry{};
                entry.host_path = std::filesystem::current_path();
                this->directory = this->emu->process.files.store(std::move(entry));
            }

            NTSTATUS query(uint32_t size, SECURITY_INFORMATION information = OWNER_SECURITY_INFORMATION)
            {
                const syscall_context context{
                    .win_emu = *this->emu, .emu = this->emu->emu(), .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
                return syscalls::handle_NtQuerySecurityObject(context, this->directory, information, this->storage + 0x100, size,
                                                              {this->emu->memory, this->storage});
            }

            std::unique_ptr<windows_emulator> emu{};
            handle directory{};
            uint64_t storage{};
        };
    }

    TEST_F(FileSecurityTest, ReturnsGuestOwnerIndependentOfHostIdentity)
    {
        const auto& expected = this->emu->process.sid;
        ASSERT_EQ(STATUS_SUCCESS, this->query(256));
        const auto descriptor = emulator_object<SECURITY_DESCRIPTOR_RELATIVE>{this->emu->memory, this->storage + 0x100}.read();
        EXPECT_EQ(SECURITY_DESCRIPTOR_REVISION, descriptor.Revision);
        EXPECT_NE(0, descriptor.Control & SE_SELF_RELATIVE);
        EXPECT_EQ(sizeof(descriptor), descriptor.Owner);
        std::vector<uint8_t> owner(expected.size());
        this->emu->memory.read_memory(this->storage + 0x100 + descriptor.Owner, owner.data(), owner.size());
        EXPECT_EQ(expected, owner);
        EXPECT_EQ(sizeof(descriptor) + owner.size(), (emulator_object<ULONG>{this->emu->memory, this->storage}.read()));
    }

    TEST_F(FileSecurityTest, ReportsSpaceRequiredForGuestOwner)
    {
        EXPECT_EQ(STATUS_BUFFER_TOO_SMALL, this->query(0));
        EXPECT_EQ(sizeof(SECURITY_DESCRIPTOR_RELATIVE) + this->emu->process.sid.size(),
                  (emulator_object<ULONG>{this->emu->memory, this->storage}.read()));
    }

    TEST_F(FileSecurityTest, OwnerMatchesTokenUser)
    {
        ASSERT_EQ(STATUS_SUCCESS, this->query(256));
        const syscall_context context{
            .win_emu = *this->emu, .emu = this->emu->emu(), .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtQueryInformationToken(context, CURRENT_PROCESS_TOKEN, TokenUser, this->storage + 0x300,
                                                                           256, {this->emu->memory, this->storage}));
        const auto descriptor = emulator_object<SECURITY_DESCRIPTOR_RELATIVE>{this->emu->memory, this->storage + 0x100}.read();
        const auto token = emulator_object<TOKEN_USER64>{this->emu->memory, this->storage + 0x300}.read();
        std::vector<uint8_t> owner(this->emu->process.sid.size());
        std::vector<uint8_t> user(owner.size());
        this->emu->memory.read_memory(this->storage + 0x100 + descriptor.Owner, owner.data(), owner.size());
        this->emu->memory.read_memory(token.User.Sid, user.data(), user.size());
        EXPECT_EQ(user, owner);
    }

    TEST_F(FileSecurityTest, NonFileObjectsHaveTheSameGuestOwner)
    {
        this->directory = CURRENT_PROCESS;
        ASSERT_EQ(STATUS_SUCCESS, this->query(256));
        const auto descriptor = emulator_object<SECURITY_DESCRIPTOR_RELATIVE>{this->emu->memory, this->storage + 0x100}.read();
        std::vector<uint8_t> owner(this->emu->process.sid.size());
        this->emu->memory.read_memory(this->storage + 0x100 + descriptor.Owner, owner.data(), owner.size());
        EXPECT_EQ(this->emu->process.sid, owner);
    }

    TEST_F(FileSecurityTest, VirtualFilesDoNotRequireHostSecurityAccess)
    {
        this->emu->process.files.get(this->directory)->host_path.clear();
        ASSERT_EQ(STATUS_SUCCESS, this->query(256));
        const auto descriptor = emulator_object<SECURITY_DESCRIPTOR_RELATIVE>{this->emu->memory, this->storage + 0x100}.read();
        std::vector<uint8_t> owner(this->emu->process.sid.size());
        this->emu->memory.read_memory(this->storage + 0x100 + descriptor.Owner, owner.data(), owner.size());
        EXPECT_EQ(this->emu->process.sid, owner);
    }

    TEST_F(FileSecurityTest, ReportsGuestPrimaryGroupWithCorrectOffsets)
    {
        const auto& sid = this->emu->process.sid;
        EXPECT_EQ(STATUS_BUFFER_TOO_SMALL, this->query(0, OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION));
        const auto required = emulator_object<ULONG>{this->emu->memory, this->storage}.read();
        EXPECT_EQ(sizeof(SECURITY_DESCRIPTOR_RELATIVE) + 2 * sid.size(), required);
        ASSERT_EQ(STATUS_SUCCESS, this->query(required, OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION));
        const auto descriptor = emulator_object<SECURITY_DESCRIPTOR_RELATIVE>{this->emu->memory, this->storage + 0x100}.read();
        EXPECT_EQ(sizeof(descriptor), descriptor.Owner);
        EXPECT_EQ(sizeof(descriptor) + sid.size(), descriptor.Group);
        std::vector<uint8_t> group(sid.size());
        this->emu->memory.read_memory(this->storage + 0x100 + descriptor.Group, group.data(), group.size());
        EXPECT_EQ(sid, group);
    }
}
