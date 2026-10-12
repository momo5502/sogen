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
    NTSTATUS handle_NtImpersonateAnonymousToken(const syscall_context& c, handle thread_handle);
    NTSTATUS handle_NtOpenThreadToken(const syscall_context& c, handle thread_handle, ACCESS_MASK desired_access, BOOLEAN open_as_self,
                                      emulator_object<handle> token_handle);
    NTSTATUS handle_NtSetInformationThread(const syscall_context& c, handle thread_handle, THREADINFOCLASS info_class,
                                           uint64_t thread_information, uint32_t thread_information_length);
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
                const auto thread =
                    this->emu->process.threads.store(emulator_thread{this->emu->memory, this->emu->process, 0, 0, 0x100000, 0, 8, true});
                this->emu->vcpu(0).active_thread = this->emu->process.threads.get(thread);
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

    TEST_F(TokenInformationTest, ThreadWithoutImpersonationHasNoToken)
    {
        emulator_object<handle> output{this->emu->memory, this->storage + 0x200};
        output.write(DUMMY_IMPERSONATION_TOKEN);
        EXPECT_EQ(STATUS_NO_TOKEN, syscalls::handle_NtOpenThreadToken(this->context(), CURRENT_THREAD, 0, true, output));
        EXPECT_EQ(DUMMY_IMPERSONATION_TOKEN, output.read());
    }

    TEST_F(TokenInformationTest, AnonymousImpersonationUsesAnonymousIdentityAndCannotBeOpened)
    {
        EXPECT_EQ(0x179cu, offsetof(TEB64, IsImpersonating));
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtImpersonateAnonymousToken(this->context(), CURRENT_THREAD));
        EXPECT_EQ(ANONYMOUS_IMPERSONATION_TOKEN, this->emu->vcpu(0).active_thread->impersonation_token);
        EXPECT_EQ(STATUS_CANT_OPEN_ANONYMOUS,
                  syscalls::handle_NtOpenThreadToken(this->context(), CURRENT_THREAD, 0, true, {this->emu->memory, this->storage + 0x200}));
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtQueryInformationToken(this->context(), CURRENT_THREAD_EFFECTIVE_TOKEN, TokenUser,
                                                                           this->storage + 0x100, 512, {this->emu->memory, this->storage}));
        const auto user = emulator_object<TOKEN_USER64>{this->emu->memory, this->storage + 0x100}.read();
        constexpr std::array<uint8_t, 12> expected_sid{1, 1, 0, 0, 0, 0, 0, 5, 7, 0, 0, 0};
        EXPECT_EQ(expected_sid, (emulator_object<std::array<uint8_t, 12>>{this->emu->memory, user.User.Sid}.read()));
        ASSERT_EQ(STATUS_SUCCESS,
                  syscalls::handle_NtQueryInformationToken(this->context(), CURRENT_THREAD_EFFECTIVE_TOKEN, TokenImpersonationLevel,
                                                           this->storage + 0x100, 4, {this->emu->memory, this->storage}));
        EXPECT_EQ(SecurityAnonymous, (emulator_object<SECURITY_IMPERSONATION_LEVEL>{this->emu->memory, this->storage + 0x100}.read()));
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtQueryInformationToken(this->context(), CURRENT_THREAD_EFFECTIVE_TOKEN, TokenGroups,
                                                                           this->storage + 0x100, 4, {this->emu->memory, this->storage}));
        EXPECT_EQ(0u, (emulator_object<ULONG>{this->emu->memory, this->storage + 0x100}.read()));
    }

    TEST_F(TokenInformationTest, SetThreadTokenReadsInputAndCanRestoreOrClearImpersonation)
    {
        emulator_object<handle> input{this->emu->memory, this->storage + 0x200};
        for (const auto token : {DUMMY_IMPERSONATION_TOKEN, ANONYMOUS_IMPERSONATION_TOKEN, handle{}})
        {
            input.write(token);
            ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtSetInformationThread(this->context(), CURRENT_THREAD, ThreadImpersonationToken,
                                                                              input.value(), sizeof(handle)));
            EXPECT_EQ(token, input.read());
            EXPECT_EQ(token, this->emu->vcpu(0).active_thread->impersonation_token);
            EXPECT_EQ(token != 0, this->emu->vcpu(0).active_thread->teb64->read().IsImpersonating != 0);
        }
        EXPECT_EQ(STATUS_NO_TOKEN, syscalls::handle_NtOpenThreadToken(this->context(), CURRENT_THREAD, 0, true, input));
    }

    TEST_F(TokenInformationTest, RejectsInvalidThreadAndPrimaryTokenWithoutChangingState)
    {
        EXPECT_EQ(STATUS_INVALID_HANDLE, syscalls::handle_NtImpersonateAnonymousToken(this->context(), handle{}));
        emulator_object<handle> input{this->emu->memory, this->storage + 0x200};
        input.write(CURRENT_PROCESS_TOKEN);
        EXPECT_EQ(STATUS_BAD_TOKEN_TYPE, syscalls::handle_NtSetInformationThread(this->context(), CURRENT_THREAD, ThreadImpersonationToken,
                                                                                 input.value(), sizeof(handle)));
        EXPECT_EQ(CURRENT_PROCESS_TOKEN, input.read());
        EXPECT_EQ(0u, this->emu->vcpu(0).active_thread->impersonation_token.bits);
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

    TEST_F(TokenInformationTest, DesktopAppContainerQueryReturnsNullSid)
    {
        EXPECT_EQ(STATUS_BUFFER_TOO_SMALL, this->query(TokenAppContainerSid, 0));
        EXPECT_EQ(8u, (emulator_object<ULONG>{this->emu->memory, this->storage}.read()));
        emulator_object<uint64_t>{this->emu->memory, this->storage + 0x100}.write(UINT64_MAX);
        ASSERT_EQ(STATUS_SUCCESS, this->query(TokenAppContainerSid, 8));
        EXPECT_EQ(0u, (emulator_object<uint64_t>{this->emu->memory, this->storage + 0x100}.read()));
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

    TEST_F(TokenInformationTest, DesktopTokenDoesNotHavePackageClaims)
    {
        const std::array<std::u16string, 2> names{u"WIN://SYSAPPID", u"WIN://PKG"};
        auto address = this->storage + 0x200;
        for (size_t index = 0; index < names.size(); ++index)
        {
            const auto length = static_cast<USHORT>(names[index].size() * sizeof(char16_t));
            this->emu->memory.write_memory(address, names[index].data(), length);
            emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>>{this->emu->memory, this->storage + 0x100}.write(
                {.Length = length, .MaximumLength = length, .Buffer = address}, index);
            address += length;
        }
        for (const auto size : {0UL, 1024UL})
        {
            emulator_object<ULONG>{this->emu->memory, this->storage}.write(0x12345678);
            EXPECT_EQ(STATUS_NOT_FOUND, syscalls::handle_NtQuerySecurityAttributesToken(
                                            this->context(), CURRENT_PROCESS_TOKEN, {this->emu->memory, this->storage + 0x100}, 2,
                                            this->storage + 0x400, size, {this->emu->memory, this->storage}));
            EXPECT_EQ(0, (emulator_object<ULONG>{this->emu->memory, this->storage}.read()));
        }
    }

    TEST_F(TokenInformationTest, RejectsInconsistentAttributeArrayAndCount)
    {
        for (const auto count : {0UL, 1UL})
        {
            EXPECT_EQ(STATUS_INVALID_PARAMETER,
                      syscalls::handle_NtQuerySecurityAttributesToken(this->context(), CURRENT_PROCESS_TOKEN,
                                                                      {this->emu->memory, count == 0 ? this->storage + 0x100 : 0}, count,
                                                                      this->storage + 0x400, 1024, {this->emu->memory, this->storage}));
        }
    }

}
