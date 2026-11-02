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
    NTSTATUS handle_NtPrivilegeCheck(const syscall_context& c, handle client_token, emulator_pointer required_privileges,
                                     emulator_object<BOOLEAN> result);
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
                return {.win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
            }

            NTSTATUS privilege_check(const handle token, const ULONG count, const ULONG control, const ULONG luid,
                                     const uint64_t set_address)
            {
                this->emu->memory.write_memory(set_address, std::array<ULONG, 5>{count, control, luid, 0, 0});
                this->emu->memory.write_memory(this->storage, BOOLEAN{0xAA});
                return syscalls::handle_NtPrivilegeCheck(this->context(), token, set_address, {this->emu->memory, this->storage});
            }

            NTSTATUS privilege_check(const handle token, const ULONG count, const ULONG control, const ULONG luid)
            {
                return this->privilege_check(token, count, control, luid, this->storage + 0x100);
            }

            BOOLEAN privilege_check_result()
            {
                return this->emu->memory.read_memory<BOOLEAN>(this->storage);
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

    TEST_F(TokenInformationTest, PrivilegeCheckGrantsOnlyEmptyAllNecessarySets)
    {
        constexpr ULONG all_necessary = 1;
        constexpr ULONG se_tcb = 7;
        for (const auto token : {CURRENT_PROCESS_TOKEN, DUMMY_IMPERSONATION_TOKEN, ANONYMOUS_IMPERSONATION_TOKEN})
        {
            ASSERT_EQ(STATUS_SUCCESS, this->privilege_check(token, 0, all_necessary, 0));
            EXPECT_EQ(TRUE, this->privilege_check_result());
            ASSERT_EQ(STATUS_SUCCESS, this->privilege_check(token, 0, 0, 0));
            EXPECT_EQ(FALSE, this->privilege_check_result());
            for (const auto luid : std::array<ULONG, 3>{se_tcb, 0, 0x7FFF})
            {
                for (const auto control : std::array<ULONG, 3>{0, all_necessary, 2})
                {
                    ASSERT_EQ(STATUS_SUCCESS, this->privilege_check(token, 1, control, luid));
                    EXPECT_EQ(FALSE, this->privilege_check_result());
                }
            }
        }
    }

    TEST_F(TokenInformationTest, PrivilegeCheckRejectsNonTokenHandlesAndUnreadableArguments)
    {
        constexpr ULONG se_change_notify = 23;
        const auto event_handle = this->emu->process.events.store(event{});
        const std::array<std::pair<handle, NTSTATUS>, 4> handles{{
            {handle{}, STATUS_INVALID_HANDLE},
            {make_handle(0x1234), STATUS_INVALID_HANDLE},
            {CURRENT_THREAD_EFFECTIVE_TOKEN, STATUS_INVALID_HANDLE},
            {event_handle, STATUS_OBJECT_TYPE_MISMATCH},
        }};
        for (const auto& [token, status] : handles)
        {
            EXPECT_EQ(status, this->privilege_check(token, 1, 0, se_change_notify));
            EXPECT_EQ(0xAA, this->privilege_check_result());
        }

        const auto end_of_storage = this->storage + 0x1000 - 0x14;
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->privilege_check(DUMMY_IMPERSONATION_TOKEN, 0x100, 0, se_change_notify, end_of_storage));
        EXPECT_EQ(0xAA, this->privilege_check_result());

        const auto set = this->storage + 0x100;
        const emulator_object<BOOLEAN> result{this->emu->memory, this->storage};
        const emulator_object<BOOLEAN> no_result{this->emu->memory, 0};
        for (const auto token : {DUMMY_IMPERSONATION_TOKEN, ANONYMOUS_IMPERSONATION_TOKEN})
        {
            EXPECT_EQ(STATUS_ACCESS_VIOLATION, syscalls::handle_NtPrivilegeCheck(this->context(), token, 0, result));
            EXPECT_EQ(STATUS_ACCESS_VIOLATION, syscalls::handle_NtPrivilegeCheck(this->context(), token, set, no_result));
        }

        const auto garbage = make_handle(0x1234);
        EXPECT_EQ(STATUS_INVALID_HANDLE, syscalls::handle_NtPrivilegeCheck(this->context(), garbage, 0, result));
        EXPECT_EQ(STATUS_INVALID_HANDLE, syscalls::handle_NtPrivilegeCheck(this->context(), garbage, set, no_result));
        EXPECT_EQ(STATUS_INVALID_HANDLE, syscalls::handle_NtPrivilegeCheck(this->context(), garbage, 0, no_result));
    }

    TEST_F(TokenInformationTest, PrivilegeCheckProbesTheWholeSetBeforeRejectingMoreThanSixtySixEntries)
    {
        constexpr uint64_t header_size = 8;
        constexpr uint64_t entry_size = 12;
        constexpr size_t page_size = 0x1000;
        const emulator_object<BOOLEAN> result{this->emu->memory, this->storage};
        const emulator_object<BOOLEAN> no_result{this->emu->memory, 0};
        const auto check = [&](const ULONG count, const uint64_t set, const emulator_object<BOOLEAN> target) {
            this->emu->memory.write_memory(set, std::array<ULONG, 2>{count, 0});
            result.write(BOOLEAN{0xAA});
            return syscalls::handle_NtPrivilegeCheck(this->context(), DUMMY_IMPERSONATION_TOKEN, set, target);
        };
        const auto set_with_readable_entries = [&](const uint64_t mapped_end, const uint64_t entries) {
            return mapped_end - header_size - (entries * entry_size);
        };

        const auto storage_end = this->storage + page_size;
        EXPECT_EQ(STATUS_SUCCESS, check(66, set_with_readable_entries(storage_end, 66), result));
        EXPECT_EQ(FALSE, this->privilege_check_result());

        for (const ULONG count : {67u, 0x100u})
        {
            EXPECT_EQ(STATUS_INVALID_PARAMETER, check(count, set_with_readable_entries(storage_end, count), result)) << count;
            EXPECT_EQ(0xAA, this->privilege_check_result()) << count;
        }

        EXPECT_EQ(STATUS_ACCESS_VIOLATION, check(66, set_with_readable_entries(storage_end, 65), result));
        EXPECT_EQ(0xAA, this->privilege_check_result());

        for (const ULONG count : {0u, 1u, 67u, 0x100000u, 0x7FFFFFFFu, 0xFFFFFFFFu})
        {
            EXPECT_EQ(STATUS_ACCESS_VIOLATION, check(count, set_with_readable_entries(storage_end, 0), result)) << count;
            EXPECT_EQ(0xAA, this->privilege_check_result()) << count;
        }

        EXPECT_EQ(STATUS_ACCESS_VIOLATION, check(67, set_with_readable_entries(storage_end, 67), no_result));
        EXPECT_EQ(0xAA, this->privilege_check_result());

        constexpr ULONG large_count = 0x100000;
        constexpr uint64_t large_size = (header_size + (large_count * entry_size) + 0xFFF) & ~uint64_t{0xFFF};
        const auto large = this->emu->memory.allocate_memory(static_cast<size_t>(large_size), memory_permission::read_write);
        ASSERT_NE(0u, large);
        EXPECT_EQ(STATUS_INVALID_PARAMETER, check(large_count, large, result));
        EXPECT_EQ(0xAA, this->privilege_check_result());

        const auto read_only = this->emu->memory.allocate_memory(page_size, memory_permission::read_write);
        ASSERT_NE(0u, read_only);
        const auto read_only_end = read_only + page_size;
        for (const ULONG count : {1u, 66u, 67u})
        {
            this->emu->memory.write_memory(set_with_readable_entries(read_only_end, count), std::array<ULONG, 2>{count, 0});
        }
        ASSERT_TRUE(this->emu->memory.protect_memory(read_only, page_size, memory_permission::read));
        for (const ULONG count : {1u, 66u, 67u})
        {
            result.write(BOOLEAN{0xAA});
            EXPECT_EQ(STATUS_ACCESS_VIOLATION, syscalls::handle_NtPrivilegeCheck(this->context(), DUMMY_IMPERSONATION_TOKEN,
                                                                                 set_with_readable_entries(read_only_end, count), result))
                << count;
            EXPECT_EQ(0xAA, this->privilege_check_result()) << count;
        }
    }

}
