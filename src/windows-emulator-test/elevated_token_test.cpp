#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>
#include <token.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtQueryInformationToken(const syscall_context& c, handle token_handle, TOKEN_INFORMATION_CLASS information_class,
                                            uint64_t information, ULONG information_length, emulator_object<ULONG> return_length);
    NTSTATUS handle_NtAccessCheck(const syscall_context& c, uint64_t security_descriptor, handle requested_token,
                                  ACCESS_MASK desired_access, emulator_object<EMU_GENERIC_MAPPING> generic_mapping, uint64_t privilege_set,
                                  emulator_object<ULONG> privilege_set_length, emulator_object<ACCESS_MASK> granted_access,
                                  emulator_object<NTSTATUS> access_status);
    NTSTATUS handle_NtImpersonateAnonymousToken(const syscall_context& c, handle thread_handle);
}

namespace sogen::test
{
    namespace
    {
        using sid_bytes = std::vector<uint8_t>;

        const sid_bytes everyone_sid{1, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0};                          // S-1-1-0
        const sid_bytes local_sid{1, 1, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0};                             // S-1-2-0
        const sid_bytes console_logon_sid{1, 1, 0, 0, 0, 0, 0, 2, 1, 0, 0, 0};                     // S-1-2-1
        constexpr std::array<uint8_t, 12> interactive_sid{1, 1, 0, 0, 0, 0, 0, 5, 4, 0, 0, 0};     // S-1-5-4
        const sid_bytes authenticated_users_sid{1, 1, 0, 0, 0, 0, 0, 5, 11, 0, 0, 0};              // S-1-5-11
        const sid_bytes this_organization_sid{1, 1, 0, 0, 0, 0, 0, 5, 15, 0, 0, 0};                // S-1-5-15
        const sid_bytes local_account_sid{1, 1, 0, 0, 0, 0, 0, 5, 113, 0, 0, 0};                   // S-1-5-113
        const sid_bytes local_administrator_account_sid{1, 1, 0, 0, 0, 0, 0, 5, 114, 0, 0, 0};     // S-1-5-114
        const sid_bytes users_sid{1, 2, 0, 0, 0, 0, 0, 5, 32, 0, 0, 0, 0x21, 0x02, 0, 0};          // S-1-5-32-545
        const sid_bytes ntlm_authentication_sid{1, 2, 0, 0, 0, 0, 0, 5, 64, 0, 0, 0, 10, 0, 0, 0}; // S-1-5-64-10
        const sid_bytes logon_session_sid_prefix{1, 3, 0, 0, 0, 0, 0, 5, 5, 0, 0, 0};              // S-1-5-5-x-y
        const sid_bytes medium_integrity_sid{1, 1, 0, 0, 0, 0, 0, 16, 0x00, 0x20, 0, 0};           // S-1-16-8192
        const sid_bytes high_integrity_sid{1, 1, 0, 0, 0, 0, 0, 16, 0x00, 0x30, 0, 0};             // S-1-16-12288

        // S-1-5-21-1-2-3-1001, a local account, and S-1-5-21-1-2-3-513, its domain's users group.
        const sid_bytes local_user_sid{1, 5, 0, 0, 0, 0, 0, 5, 21, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 3, 0, 0, 0, 0xE9, 0x03, 0, 0};
        const sid_bytes local_users_group_sid{1, 5, 0, 0, 0, 0, 0, 5, 21, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 3, 0, 0, 0, 0x01, 0x02, 0, 0};
        const sid_bytes anonymous_sid{1, 1, 0, 0, 0, 0, 0, 5, 7, 0, 0, 0}; // S-1-5-7

        constexpr ULONG se_group_default = 0x7; // SE_GROUP_MANDATORY | SE_GROUP_ENABLED_BY_DEFAULT | SE_GROUP_ENABLED
        constexpr ULONG se_group_owner = 0x8;
        constexpr ULONG se_group_integrity = 0x60; // SE_GROUP_INTEGRITY | SE_GROUP_INTEGRITY_ENABLED
        constexpr ULONG se_group_logon_id = 0xC0000000;

        struct token_group
        {
            sid_bytes sid;
            ULONG attributes;

            bool operator==(const token_group&) const = default;
        };

        constexpr uint8_t object_inherit_ace = 0x1;
        constexpr ACCESS_MASK generic_read = 0x80000000;
        constexpr ACCESS_MASK generic_all = 0x10000000;
        constexpr ACCESS_MASK file_generic_read = 0x00120089;
        constexpr EMU_GENERIC_MAPPING file_generic_mapping{
            .GenericRead = file_generic_read,
            .GenericWrite = 0x00120116,
            .GenericExecute = 0x001200A0,
            .GenericAll = 0x001F01FF,
        };

        struct test_ace
        {
            uint8_t type{ACCESS_ALLOWED_ACE_TYPE};
            uint8_t flags{};
            ACCESS_MASK mask{};
            std::span<const uint8_t> sid{};
        };

        class ElevatedTokenTest : public testing::TestWithParam<bool>
        {
          protected:
            void SetUp() override
            {
                emulator_interfaces interfaces{};
                interfaces.ui = std::make_unique<null_ui_backend>();
                interfaces.audio = std::make_unique<null_audio_backend>();
                interfaces.console = std::make_unique<null_console_backend>();
                this->emu = std::make_unique<windows_emulator>(create_x86_64_emulator_from_environment(),
                                                               emulator_settings{.disable_logging = true,
                                                                                 .emulation_root = get_emulator_root(),
                                                                                 .load_registry = false,
                                                                                 .elevated = this->elevated()},
                                                               emulator_callbacks{}, std::move(interfaces));
                this->storage = this->emu->memory.allocate_memory(0x2000, memory_permission::read_write);
                const auto thread =
                    this->emu->process.threads.store(emulator_thread{this->emu->memory, this->emu->process, 0, 0, 0x100000, 0, 8, true});
                this->emu->vcpu(0).active_thread = this->emu->process.threads.get(thread);
            }

            bool elevated() const
            {
                return GetParam();
            }

            syscall_context context()
            {
                return {.win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
            }

            NTSTATUS query(const handle token, const TOKEN_INFORMATION_CLASS information_class, const ULONG size)
            {
                return syscalls::handle_NtQueryInformationToken(this->context(), token, information_class, this->storage + 0x100, size,
                                                                {this->emu->memory, this->storage});
            }

            ULONG required_size(const handle token, const TOKEN_INFORMATION_CLASS information_class)
            {
                EXPECT_EQ(STATUS_BUFFER_TOO_SMALL, this->query(token, information_class, 0));
                return emulator_object<ULONG>{this->emu->memory, this->storage}.read();
            }

            std::vector<token_group> token_groups(const handle token)
            {
                EXPECT_EQ(STATUS_SUCCESS, this->query(token, TokenGroups, this->required_size(token, TokenGroups)));

                const auto count = emulator_object<ULONG>{this->emu->memory, this->storage + 0x100}.read();
                std::vector<token_group> groups{};
                for (ULONG index = 0; index < count; ++index)
                {
                    const auto entry =
                        emulator_object<SID_AND_ATTRIBUTES64>{this->emu->memory, this->storage + 0x100 + offsetof(TOKEN_GROUPS64, Groups) +
                                                                                     (index * sizeof(SID_AND_ATTRIBUTES64))}
                            .read();
                    const auto sub_authority_count = this->emu->memory.read_memory<uint8_t>(entry.Sid + 1);
                    sid_bytes sid(offsetof(SID, SubAuthority) + (sub_authority_count * sizeof(DWORD)));
                    this->emu->memory.read_memory(entry.Sid, sid.data(), sid.size());
                    groups.push_back({.sid = std::move(sid), .attributes = entry.Attributes});
                }

                return groups;
            }

            // TokenOwner and TokenPrimaryGroup: a pointer to the SID that follows it.
            sid_bytes token_sid(const handle token, const TOKEN_INFORMATION_CLASS information_class)
            {
                const auto size = this->required_size(token, information_class);
                EXPECT_EQ(STATUS_SUCCESS, this->query(token, information_class, size));

                sid_bytes sid(size - sizeof(uint64_t));
                this->emu->memory.read_memory(this->emu->memory.read_memory<uint64_t>(this->storage + 0x100), sid.data(), sid.size());
                return sid;
            }

            // A self-relative descriptor owned by LocalSystem whose DACL holds the ACEs in order.
            uint64_t build_descriptor(const std::initializer_list<test_ace> aces)
            {
                const auto descriptor_address = this->storage + 0x1000;
                const auto owner_offset = static_cast<uint32_t>(sizeof(SECURITY_DESCRIPTOR_RELATIVE));
                const auto group_offset = owner_offset + static_cast<uint32_t>(local_system_sid.size());
                const auto dacl_offset = group_offset + static_cast<uint32_t>(local_system_sid.size());

                SECURITY_DESCRIPTOR_RELATIVE descriptor{};
                descriptor.Revision = 1;
                descriptor.Control = SE_SELF_RELATIVE | SE_DACL_PRESENT;
                descriptor.Owner = owner_offset;
                descriptor.Group = group_offset;
                descriptor.Dacl = dacl_offset;
                emulator_object<SECURITY_DESCRIPTOR_RELATIVE>{this->emu->memory, descriptor_address}.write(descriptor);
                this->emu->memory.write_memory(descriptor_address + owner_offset, local_system_sid.data(), local_system_sid.size());
                this->emu->memory.write_memory(descriptor_address + group_offset, local_system_sid.data(), local_system_sid.size());

                auto ace_address = descriptor_address + dacl_offset + sizeof(ACL);
                for (const auto& entry : aces)
                {
                    ACCESS_ALLOWED_ACE ace{};
                    ace.Header.AceType = entry.type;
                    ace.Header.AceFlags = entry.flags;
                    ace.Header.AceSize = static_cast<uint16_t>(offsetof(ACCESS_ALLOWED_ACE, SidStart) + entry.sid.size());
                    ace.Mask = entry.mask;
                    emulator_object<ACCESS_ALLOWED_ACE>{this->emu->memory, ace_address}.write(ace);
                    this->emu->memory.write_memory(ace_address + offsetof(ACCESS_ALLOWED_ACE, SidStart), entry.sid.data(),
                                                   entry.sid.size());
                    ace_address += ace.Header.AceSize;
                }

                ACL acl{};
                acl.AclRevision = 2;
                acl.AclSize = static_cast<uint16_t>(ace_address - (descriptor_address + dacl_offset));
                acl.AceCount = static_cast<uint16_t>(aces.size());
                emulator_object<ACL>{this->emu->memory, descriptor_address + dacl_offset}.write(acl);

                return descriptor_address;
            }

            // NtAccessCheck's own status; the access status it writes stays at storage + 0x840.
            NTSTATUS call_access_check(const handle token, const uint64_t descriptor, const ACCESS_MASK desired_access)
            {
                const emulator_object<EMU_GENERIC_MAPPING> mapping{this->emu->memory, this->storage + 0x800};
                mapping.write(file_generic_mapping);

                const emulator_object<ULONG> privilege_set_length{this->emu->memory, this->storage + 0x820};
                const emulator_object<ACCESS_MASK> granted{this->emu->memory, this->storage + 0x830};
                const emulator_object<NTSTATUS> status{this->emu->memory, this->storage + 0x840};
                return syscalls::handle_NtAccessCheck(this->context(), descriptor, token, desired_access, mapping, 0, privilege_set_length,
                                                      granted, status);
            }

            NTSTATUS check_access(const handle token, const std::initializer_list<test_ace> aces, const ACCESS_MASK desired_access)
            {
                EXPECT_EQ(STATUS_SUCCESS, this->call_access_check(token, this->build_descriptor(aces), desired_access));
                return emulator_object<NTSTATUS>{this->emu->memory, this->storage + 0x840}.read();
            }

            NTSTATUS check_access(const handle token, const std::span<const uint8_t> sid, const ACCESS_MASK mask)
            {
                return this->check_access(token, {test_ace{.mask = mask, .sid = sid}}, mask);
            }

            std::unique_ptr<windows_emulator> emu{};
            uint64_t storage{};
        };
    }

    TEST_P(ElevatedTokenTest, GroupsAreThoseOfAnInteractiveConsoleLogon)
    {
        this->emu->process.sid = local_user_sid;
        auto groups = this->token_groups(CURRENT_PROCESS_TOKEN);

        const auto logon_session = std::ranges::find_if(groups, [](const token_group& group) {
            return group.sid.size() == logon_session_sid_prefix.size() + (2 * sizeof(DWORD)) &&
                   std::ranges::equal(std::span{group.sid}.first(logon_session_sid_prefix.size()), logon_session_sid_prefix);
        });
        ASSERT_NE(logon_session, groups.end());
        EXPECT_EQ(se_group_default | se_group_logon_id, logon_session->attributes);
        groups.erase(logon_session);

        std::vector<token_group> expected{{local_users_group_sid, se_group_default}, {everyone_sid, se_group_default}};
        if (this->elevated())
        {
            expected.push_back({local_administrator_account_sid, se_group_default});
            expected.push_back({{administrators_sid.begin(), administrators_sid.end()}, se_group_default | se_group_owner});
        }

        expected.insert(expected.end(), {{users_sid, se_group_default},
                                         {{interactive_sid.begin(), interactive_sid.end()}, se_group_default},
                                         {console_logon_sid, se_group_default},
                                         {authenticated_users_sid, se_group_default},
                                         {this_organization_sid, se_group_default},
                                         {local_account_sid, se_group_default},
                                         {local_sid, se_group_default},
                                         {ntlm_authentication_sid, se_group_default},
                                         {this->elevated() ? high_integrity_sid : medium_integrity_sid, se_group_integrity}});
        EXPECT_EQ(expected, groups);
    }

    // A local account's list starts with its domain's users group; LocalSystem, outside an account domain, has none.
    TEST_P(ElevatedTokenTest, StatisticsAgreeWithTheGroupList)
    {
        for (const auto& user : {local_user_sid, sid_bytes{local_system_sid.begin(), local_system_sid.end()}})
        {
            this->emu->process.sid = user;
            const auto group_count = this->token_groups(CURRENT_PROCESS_TOKEN).size();

            ASSERT_EQ(STATUS_SUCCESS, this->query(CURRENT_PROCESS_TOKEN, TokenStatistics, sizeof(TOKEN_STATISTICS)));
            const auto stats = emulator_object<TOKEN_STATISTICS>{this->emu->memory, this->storage + 0x100}.read();
            EXPECT_EQ(group_count, stats.GroupCount);
        }
    }

    TEST_P(ElevatedTokenTest, AccessChecksMatchEveryEnabledGroup)
    {
        this->emu->process.sid = local_user_sid;
        for (const auto& group : {everyone_sid, authenticated_users_sid, users_sid, local_sid, local_users_group_sid})
        {
            EXPECT_EQ(STATUS_SUCCESS, this->check_access(CURRENT_PROCESS_TOKEN, group, file_generic_read));
        }

        const auto& integrity_label = this->elevated() ? high_integrity_sid : medium_integrity_sid;
        EXPECT_EQ(STATUS_ACCESS_DENIED, this->check_access(CURRENT_PROCESS_TOKEN, integrity_label, file_generic_read));
        EXPECT_EQ(STATUS_ACCESS_DENIED,
                  this->check_access(CURRENT_PROCESS_TOKEN,
                                     {{.type = ACCESS_DENIED_ACE_TYPE, .mask = file_generic_read, .sid = everyone_sid},
                                      {.mask = file_generic_read, .sid = interactive_sid}},
                                     file_generic_read));
    }

    TEST_P(ElevatedTokenTest, OwnerAndPrimaryGroupAreThoseLsaGivesTheLogon)
    {
        this->emu->process.sid = local_user_sid;
        const sid_bytes administrators{administrators_sid.begin(), administrators_sid.end()};
        EXPECT_EQ(this->elevated() ? administrators : local_user_sid, this->token_sid(CURRENT_PROCESS_TOKEN, TokenOwner));
        EXPECT_EQ(local_users_group_sid, this->token_sid(CURRENT_PROCESS_TOKEN, TokenPrimaryGroup));

        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtImpersonateAnonymousToken(this->context(), CURRENT_THREAD));
        EXPECT_EQ(anonymous_sid, this->token_sid(CURRENT_THREAD_EFFECTIVE_TOKEN, TokenOwner));
        EXPECT_EQ(anonymous_sid, this->token_sid(CURRENT_THREAD_EFFECTIVE_TOKEN, TokenPrimaryGroup));
    }

    TEST_P(ElevatedTokenTest, AdministratorsMembershipFollowsTheElevatedFlag)
    {
        const auto expected = this->elevated() ? STATUS_SUCCESS : STATUS_ACCESS_DENIED;
        EXPECT_EQ(expected, this->check_access(CURRENT_PROCESS_TOKEN, administrators_sid, 1));
    }

    TEST_P(ElevatedTokenTest, AnonymousImpersonationIsNeverElevated)
    {
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtImpersonateAnonymousToken(this->context(), CURRENT_THREAD));

        ASSERT_EQ(STATUS_SUCCESS, this->query(CURRENT_THREAD_EFFECTIVE_TOKEN, TokenElevation, sizeof(TOKEN_ELEVATION)));
        EXPECT_EQ(0u, (emulator_object<TOKEN_ELEVATION>{this->emu->memory, this->storage + 0x100}.read().TokenIsElevated));

        const auto size = this->required_size(CURRENT_THREAD_EFFECTIVE_TOKEN, TokenIntegrityLevel);
        ASSERT_EQ(STATUS_SUCCESS, this->query(CURRENT_THREAD_EFFECTIVE_TOKEN, TokenIntegrityLevel, size));
        const auto label = emulator_object<TOKEN_MANDATORY_LABEL64>{this->emu->memory, this->storage + 0x100}.read();

        // S-1-16-8192, medium integrity: the anonymous token must not pick up the process elevation.
        EXPECT_EQ(0x20u, this->emu->memory.read_memory<uint8_t>(label.Label.Sid + 9));

        EXPECT_EQ(STATUS_ACCESS_DENIED, this->check_access(CURRENT_THREAD_EFFECTIVE_TOKEN, administrators_sid, 1));
    }

    TEST_P(ElevatedTokenTest, AceMasksAreComparedWithoutGenericMapping)
    {
        EXPECT_EQ(STATUS_ACCESS_DENIED,
                  this->check_access(CURRENT_PROCESS_TOKEN, {{.mask = generic_read, .sid = interactive_sid}}, file_generic_read));
        EXPECT_EQ(STATUS_SUCCESS, this->check_access(CURRENT_PROCESS_TOKEN,
                                                     {{.type = ACCESS_DENIED_ACE_TYPE, .mask = generic_all, .sid = interactive_sid},
                                                      {.mask = file_generic_read, .sid = interactive_sid}},
                                                     file_generic_read));
    }

    TEST_P(ElevatedTokenTest, InheritOnlyAcesAreSkipped)
    {
        constexpr uint8_t inherit_only = object_inherit_ace | INHERIT_ONLY_ACE;
        EXPECT_EQ(STATUS_ACCESS_DENIED,
                  this->check_access(CURRENT_PROCESS_TOKEN, {{.flags = inherit_only, .mask = file_generic_read, .sid = interactive_sid}},
                                     file_generic_read));
        EXPECT_EQ(
            STATUS_SUCCESS,
            this->check_access(CURRENT_PROCESS_TOKEN,
                               {{.type = ACCESS_DENIED_ACE_TYPE, .flags = inherit_only, .mask = file_generic_read, .sid = interactive_sid},
                                {.mask = file_generic_read, .sid = interactive_sid}},
                               file_generic_read));
    }

    // LSA makes the account domain's users group a local account's primary group, the group TokenGroups lists first; a
    // token whose user is outside an account domain keeps its user's SID there.
    TEST_P(ElevatedTokenTest, PrimaryGroupIsTheGroupListedFirst)
    {
        this->emu->process.sid = local_user_sid;
        EXPECT_EQ(local_users_group_sid, this->token_sid(CURRENT_PROCESS_TOKEN, TokenPrimaryGroup));
        EXPECT_EQ(local_users_group_sid, this->token_groups(CURRENT_PROCESS_TOKEN).front().sid);

        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtImpersonateAnonymousToken(this->context(), CURRENT_THREAD));
        EXPECT_EQ(anonymous_sid, this->token_sid(CURRENT_THREAD_EFFECTIVE_TOKEN, TokenPrimaryGroup));
    }

    // SeCaptureSecurityDescriptor's RtlValidAcl fails a DACL with STATUS_INVALID_ACL when an ACE is too short for the SID its
    // header describes (AceSize >= 16 + 4 * SubAuthorityCount), before any access is checked.
    TEST_P(ElevatedTokenTest, AnAceTooShortForItsSidMakesTheAclInvalid)
    {
        constexpr std::array<uint8_t, 8> sid_header_of_six{1, 6, 0, 0, 0, 0, 0, 5}; // S-1-5 and six sub-authorities not there
        constexpr std::array<uint16_t, 2> ace_sizes{16, 12};
        for (const auto ace_size : ace_sizes)
        {
            const auto descriptor = this->build_descriptor({});
            const auto dacl = descriptor + emulator_object<SECURITY_DESCRIPTOR_RELATIVE>{this->emu->memory, descriptor}.read().Dacl;

            ACCESS_ALLOWED_ACE ace{};
            ace.Header.AceType = ACCESS_ALLOWED_ACE_TYPE;
            ace.Header.AceSize = ace_size;
            ace.Mask = file_generic_read;
            emulator_object<ACCESS_ALLOWED_ACE>{this->emu->memory, dacl + sizeof(ACL)}.write(ace);
            this->emu->memory.write_memory(dacl + sizeof(ACL) + offsetof(ACCESS_ALLOWED_ACE, SidStart), sid_header_of_six.data(),
                                           sid_header_of_six.size());

            ACL acl{};
            acl.AclRevision = 2;
            acl.AclSize = static_cast<uint16_t>(sizeof(ACL) + ace_size);
            acl.AceCount = 1;
            emulator_object<ACL>{this->emu->memory, dacl}.write(acl);

            EXPECT_EQ(STATUS_INVALID_ACL, this->call_access_check(CURRENT_PROCESS_TOKEN, descriptor, file_generic_read)) << ace_size;
        }
    }

    // Only allow and deny ACEs must hold a SID: an ACE of a kind Windows does not know may be just its header and mask, and
    // 19041's AccessCheck passes over it to the next ACE.
    TEST_P(ElevatedTokenTest, AnAceOfAnotherKindNeedsNoSid)
    {
        constexpr uint8_t unknown_ace_type = 0x7F;
        EXPECT_EQ(STATUS_SUCCESS, this->check_access(CURRENT_PROCESS_TOKEN,
                                                     {{.type = unknown_ace_type, .mask = file_generic_read},
                                                      {.mask = file_generic_read, .sid = interactive_sid}},
                                                     file_generic_read));
    }

    INSTANTIATE_TEST_SUITE_P(Elevation, ElevatedTokenTest, testing::Values(false, true),
                             [](const testing::TestParamInfo<bool>& info) { return info.param ? "Elevated" : "Default"; });
}
