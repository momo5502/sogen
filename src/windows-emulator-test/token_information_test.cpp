#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtQueryInformationToken(const syscall_context& c, handle token_handle, TOKEN_INFORMATION_CLASS information_class,
                                            uint64_t information, ULONG information_length, emulator_object<ULONG> return_length);
    NTSTATUS handle_NtQuerySecurityAttributesToken(const syscall_context& c, handle token_handle,
                                                   emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> attributes,
                                                   ULONG number_of_attributes, uint64_t buffer, ULONG buffer_length,
                                                   emulator_object<ULONG> return_length);
}

namespace sogen::test
{
    namespace
    {
        class TokenInformationTest : public testing::Test
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
            }

            syscall_context context()
            {
                return {.win_emu = *this->emu, .emu = this->emu->emu(), .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
            }

            NTSTATUS query(TOKEN_INFORMATION_CLASS information_class, ULONG size)
            {
                return syscalls::handle_NtQueryInformationToken(this->context(), CURRENT_PROCESS_TOKEN, information_class,
                                                                this->storage + 0x100, size, {this->emu->memory, this->storage});
            }

            std::unique_ptr<windows_emulator> emu{};
            uint64_t storage{};
        };
    }

    TEST_F(TokenInformationTest, ReportsEmptyPrivilegeListWithRequiredSize)
    {
        EXPECT_EQ(STATUS_BUFFER_TOO_SMALL, this->query(TokenPrivileges, 0));
        EXPECT_EQ(sizeof(ULONG), (emulator_object<ULONG>{this->emu->memory, this->storage}.read()));
        ASSERT_EQ(STATUS_SUCCESS, this->query(TokenPrivileges, sizeof(ULONG)));
        EXPECT_EQ(0, (emulator_object<ULONG>{this->emu->memory, this->storage + 0x100}.read()));
    }

    TEST_F(TokenInformationTest, EmptyClaimsHaveSupportedVersion)
    {
        for (const auto information_class : {TokenUserClaimAttributes, TokenDeviceClaimAttributes, TokenRestrictedUserClaimAttributes,
                                             TokenRestrictedDeviceClaimAttributes, TokenSecurityAttributes})
        {
            EXPECT_EQ(STATUS_BUFFER_TOO_SMALL, this->query(information_class, 0));
            ASSERT_EQ(STATUS_SUCCESS, this->query(information_class, sizeof(TOKEN_SECURITY_ATTRIBUTES_INFORMATION)));
            const auto claims = emulator_object<TOKEN_SECURITY_ATTRIBUTES_INFORMATION>{this->emu->memory, this->storage + 0x100}.read();
            EXPECT_EQ(1, claims.Version);
            EXPECT_EQ(0, claims.AttributeCount);
            EXPECT_EQ(0, claims.Attribute.pAttributeV1);
        }
    }

    TEST_F(TokenInformationTest, EnumeratesEmptySecurityAttributes)
    {
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtQuerySecurityAttributesToken(
                                      this->context(), CURRENT_PROCESS_TOKEN, {this->emu->memory, 0}, 0, this->storage + 0x100,
                                      sizeof(TOKEN_SECURITY_ATTRIBUTES_INFORMATION), {this->emu->memory, this->storage}));
        const auto claims = emulator_object<TOKEN_SECURITY_ATTRIBUTES_INFORMATION>{this->emu->memory, this->storage + 0x100}.read();
        EXPECT_EQ(1, claims.Version);
        EXPECT_EQ(0, claims.AttributeCount);
    }

}
