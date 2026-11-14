#include "../std_include.hpp"
#include "../emulator_utils.hpp"
#include "../syscall_utils.hpp"
#include "../token.hpp"

#include <limits>

namespace sogen
{

    namespace syscalls
    {
        namespace
        {
            constexpr uint32_t token_authentication_id = 0x1000;
            constexpr uint32_t token_id = 0x1001;
            constexpr uint32_t token_modified_id = 0x1002;

            // Windows 11 26200 answers classes up to 0x3A, past the last one TOKEN_INFORMATION_CLASS names.
            constexpr ULONG last_token_information_class = 0x3A;

            constexpr std::array<uint8_t, 12> everyone_sid{1, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0};                          // S-1-1-0
            constexpr std::array<uint8_t, 12> local_sid{1, 1, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0};                             // S-1-2-0
            constexpr std::array<uint8_t, 12> console_logon_sid{1, 1, 0, 0, 0, 0, 0, 2, 1, 0, 0, 0};                     // S-1-2-1
            constexpr std::array<uint8_t, 12> interactive_sid{1, 1, 0, 0, 0, 0, 0, 5, 4, 0, 0, 0};                       // S-1-5-4
            constexpr std::array<uint8_t, 12> anonymous_sid{1, 1, 0, 0, 0, 0, 0, 5, 7, 0, 0, 0};                         // S-1-5-7
            constexpr std::array<uint8_t, 12> authenticated_users_sid{1, 1, 0, 0, 0, 0, 0, 5, 11, 0, 0, 0};              // S-1-5-11
            constexpr std::array<uint8_t, 12> this_organization_sid{1, 1, 0, 0, 0, 0, 0, 5, 15, 0, 0, 0};                // S-1-5-15
            constexpr std::array<uint8_t, 12> local_account_sid{1, 1, 0, 0, 0, 0, 0, 5, 113, 0, 0, 0};                   // S-1-5-113
            constexpr std::array<uint8_t, 12> local_administrator_account_sid{1, 1, 0, 0, 0, 0, 0, 5, 114, 0, 0, 0};     // S-1-5-114
            constexpr std::array<uint8_t, 16> users_sid{1, 2, 0, 0, 0, 0, 0, 5, 32, 0, 0, 0, 0x21, 0x02, 0, 0};          // S-1-5-32-545
            constexpr std::array<uint8_t, 16> ntlm_authentication_sid{1, 2, 0, 0, 0, 0, 0, 5, 64, 0, 0, 0, 10, 0, 0, 0}; // S-1-5-64-10
            constexpr std::array<uint8_t, 12> medium_integrity_sid{1, 1, 0, 0, 0, 0, 0, 16, 0x00, 0x20, 0, 0};           // S-1-16-8192
            constexpr std::array<uint8_t, 12> high_integrity_sid{1, 1, 0, 0, 0, 0, 0, 16, 0x00, 0x30, 0, 0};             // S-1-16-12288

            // S-1-5-5-0-4099, the logon session SID winlogon hands LsaLogonUser for the interactive logon.
            constexpr std::array<uint8_t, 20> logon_session_sid{1, 3, 0, 0, 0, 0, 0, 5, 5, 0, 0, 0, 0, 0, 0, 0, 0x03, 0x10, 0, 0};

            constexpr ULONG se_group_enabled = 0x4;
            constexpr ULONG se_group_default = 0x7; // SE_GROUP_MANDATORY | SE_GROUP_ENABLED_BY_DEFAULT | SE_GROUP_ENABLED
            constexpr ULONG se_group_owner = 0x8;
            constexpr ULONG se_group_integrity = 0x60; // SE_GROUP_INTEGRITY | SE_GROUP_INTEGRITY_ENABLED
            constexpr ULONG se_group_logon_id = 0xC0000000;

            struct token_group
            {
                std::vector<uint8_t> sid;
                ULONG attributes;
            };

            constexpr uint32_t domain_group_rid_users = 513;

            // A user of an account domain (S-1-5-21-...), whose accounts SAM gives the domain's users group as primary group
            // ("None" on a workgroup machine).
            bool is_account_domain_user(const std::span<const uint8_t> user_sid)
            {
                constexpr uint32_t security_nt_non_unique = 21;
                constexpr SID_IDENTIFIER_AUTHORITY nt_authority{.Value = {0, 0, 0, 0, 0, 5}};

                SID header{};
                if (user_sid.size() < sizeof(header))
                {
                    return false;
                }

                memcpy(&header, user_sid.data(), sizeof(header));
                return header.SubAuthorityCount >= 2 &&
                       user_sid.size() == offsetof(SID, SubAuthority) + header.SubAuthorityCount * sizeof(DWORD) &&
                       std::ranges::equal(header.IdentifierAuthority.Value, nt_authority.Value) &&
                       header.SubAuthority[0] == security_nt_non_unique;
            }

            // DOMAIN_GROUP_RID_USERS of the user's account domain.
            std::optional<std::vector<uint8_t>> account_domain_users_sid(const std::span<const uint8_t> user_sid)
            {
                if (!is_account_domain_user(user_sid))
                {
                    return std::nullopt;
                }

                std::vector<uint8_t> sid{user_sid.begin(), user_sid.end()};
                memcpy(sid.data() + sid.size() - sizeof(domain_group_rid_users), &domain_group_rid_users, sizeof(domain_group_rid_users));
                return sid;
            }

            // Whether sid is account_domain_users_sid(user_sid), without building it.
            bool is_account_domain_users_sid(const std::span<const uint8_t> user_sid, const std::span<const uint8_t> sid)
            {
                if (sid.size() != user_sid.size() || !is_account_domain_user(user_sid))
                {
                    return false;
                }

                const auto rid_offset = sid.size() - sizeof(domain_group_rid_users);
                uint32_t rid{};
                memcpy(&rid, sid.data() + rid_offset, sizeof(rid));
                return rid == domain_group_rid_users && std::ranges::equal(sid.first(rid_offset), user_sid.first(rid_offset));
            }

            // An interactive console logon of a local account as 19041 builds it, in TokenGroups' order: LsapAuAddStandardIds
            // adds Everyone, INTERACTIVE, CONSOLE LOGON and Authenticated Users, LsapAuSetPassedIds the primary group, This
            // Organization and Local account, SAM the aliases, winlogon the logon session SID and LOCAL, MSV1_0 its package
            // SID. With TokenElevationTypeDefault an elevated token is a full administrator's and any other a standard user's.
            // These are the groups after the account domain's users group, which comes first.
            template <typename Visit>
            void visit_standard_token_groups(const bool elevated, Visit&& visit)
            {
                visit(everyone_sid, se_group_default);
                if (elevated)
                {
                    visit(local_administrator_account_sid, se_group_default);
                    visit(administrators_sid, se_group_default | se_group_owner);
                }

                visit(users_sid, se_group_default);
                visit(interactive_sid, se_group_default);
                visit(console_logon_sid, se_group_default);
                visit(authenticated_users_sid, se_group_default);
                visit(this_organization_sid, se_group_default);
                visit(local_account_sid, se_group_default);
                visit(logon_session_sid, se_group_default | se_group_logon_id);
                visit(local_sid, se_group_default);
                visit(ntlm_authentication_sid, se_group_default);
                visit(elevated ? std::span<const uint8_t>{high_integrity_sid} : std::span<const uint8_t>{medium_integrity_sid},
                      se_group_integrity);
            }

            std::vector<token_group> get_token_groups(const windows_emulator& win_emu, const std::span<const uint8_t> user_sid)
            {
                std::vector<token_group> groups{};
                if (auto primary_group = account_domain_users_sid(user_sid))
                {
                    groups.push_back(token_group{.sid = std::move(*primary_group), .attributes = se_group_default});
                }

                visit_standard_token_groups(win_emu.runs_elevated(), [&](const std::span<const uint8_t> sid, const ULONG attributes) {
                    groups.push_back(token_group{.sid = {sid.begin(), sid.end()}, .attributes = attributes});
                });
                return groups;
            }

            // As many groups as get_token_groups lists, without building them.
            size_t count_token_groups(const windows_emulator& win_emu, const std::span<const uint8_t> user_sid)
            {
                size_t count = is_account_domain_user(user_sid) ? 1 : 0;
                visit_standard_token_groups(win_emu.runs_elevated(), [&](const std::span<const uint8_t>, const ULONG) { ++count; });
                return count;
            }

            handle resolve_token(const syscall_context& c, const handle token)
            {
                if (token == CURRENT_THREAD_TOKEN || token == CURRENT_THREAD_EFFECTIVE_TOKEN)
                {
                    const auto impersonation = c.vcpu.active_thread ? c.vcpu.active_thread->impersonation_token : handle{};
                    if (impersonation != 0 || token == CURRENT_THREAD_TOKEN)
                    {
                        return impersonation;
                    }
                    return CURRENT_PROCESS_TOKEN;
                }
                return token;
            }

            // The SID of an ACE that is ace_size bytes long, or none when it does not fit there: SeCaptureSecurityDescriptor's
            // RtlValidAcl wants every ACE to hold its whole SID and fails the descriptor with STATUS_INVALID_ACL otherwise.
            std::optional<std::vector<uint8_t>> read_ace_sid(const memory_manager& memory, const uint64_t ace_address,
                                                             const size_t ace_size)
            {
                constexpr size_t sid_offset = offsetof(ACCESS_ALLOWED_ACE, SidStart);
                constexpr size_t sid_header_size = offsetof(SID, SubAuthority);
                if (ace_size < sid_offset + sid_header_size)
                {
                    return std::nullopt;
                }

                SID header{};
                memory.read_memory(ace_address + sid_offset, &header, sid_header_size);

                std::vector<uint8_t> sid(sid_header_size + (header.SubAuthorityCount * sizeof(DWORD)));
                if (sid.size() > ace_size - sid_offset)
                {
                    return std::nullopt;
                }

                memory.read_memory(ace_address + sid_offset, sid.data(), sid.size());
                return sid;
            }

            ACCESS_MASK map_generic_access(ACCESS_MASK access, const EMU_GENERIC_MAPPING& mapping)
            {
                constexpr ACCESS_MASK generic_read = 0x80000000;
                constexpr ACCESS_MASK generic_write = 0x40000000;
                constexpr ACCESS_MASK generic_execute = 0x20000000;
                constexpr ACCESS_MASK generic_all = 0x10000000;

                if (access & generic_read)
                {
                    access = (access & ~generic_read) | mapping.GenericRead;
                }
                if (access & generic_write)
                {
                    access = (access & ~generic_write) | mapping.GenericWrite;
                }
                if (access & generic_execute)
                {
                    access = (access & ~generic_execute) | mapping.GenericExecute;
                }
                if (access & generic_all)
                {
                    access = (access & ~generic_all) | mapping.GenericAll;
                }
                return access;
            }
        }

        TOKEN_TYPE get_token_type(const handle token_handle)
        {
            const auto impersonation = token_handle == DUMMY_IMPERSONATION_TOKEN || token_handle == ANONYMOUS_IMPERSONATION_TOKEN;
            return impersonation ? TokenImpersonation : TokenPrimary;
        }

        NTSTATUS handle_NtDuplicateToken(const syscall_context& c, const handle requested_token, ACCESS_MASK /*desired_access*/,
                                         const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>>
                                         /*object_attributes*/,
                                         const BOOLEAN /*effective_only*/, const TOKEN_TYPE type,
                                         const emulator_object<handle> new_token_handle)
        {
            const auto existing_token_handle = resolve_token(c, requested_token);
            if (existing_token_handle != CURRENT_PROCESS_TOKEN && existing_token_handle != DUMMY_IMPERSONATION_TOKEN &&
                existing_token_handle != ANONYMOUS_IMPERSONATION_TOKEN)
            {
                return existing_token_handle != 0 ? STATUS_INVALID_HANDLE : STATUS_NO_TOKEN;
            }
            if (type != TokenPrimary && type != TokenImpersonation)
            {
                return STATUS_INVALID_PARAMETER;
            }
            if (existing_token_handle == ANONYMOUS_IMPERSONATION_TOKEN && type == TokenPrimary)
            {
                return STATUS_NOT_SUPPORTED;
            }
            if (get_token_type(existing_token_handle) == type)
            {
                new_token_handle.write(existing_token_handle);
            }
            else if (type == TokenPrimary)
            {
                new_token_handle.write(CURRENT_PROCESS_TOKEN);
            }
            else
            {
                new_token_handle.write(DUMMY_IMPERSONATION_TOKEN);
            }

            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtQueryInformationToken(const syscall_context& c, const handle requested_token,
                                                const TOKEN_INFORMATION_CLASS token_information_class, const uint64_t token_information,
                                                const ULONG token_information_length, const emulator_object<ULONG> return_length)
        {
            if (token_information_class < TokenUser || static_cast<ULONG>(token_information_class) > last_token_information_class)
            {
                c.win_emu.log.error("Unsupported NtQueryInformationToken info class: 0x%X\n",
                                    static_cast<uint32_t>(token_information_class));
                return STATUS_INVALID_INFO_CLASS;
            }

            const auto token_handle = resolve_token(c, requested_token);
            if (token_handle == 0 && requested_token == CURRENT_THREAD_TOKEN)
            {
                return STATUS_NO_TOKEN;
            }
            if (token_handle != CURRENT_PROCESS_TOKEN && token_handle != CURRENT_THREAD_TOKEN &&
                token_handle != CURRENT_THREAD_EFFECTIVE_TOKEN && token_handle != DUMMY_IMPERSONATION_TOKEN &&
                token_handle != ANONYMOUS_IMPERSONATION_TOKEN)
            {
                return STATUS_NOT_SUPPORTED;
            }

            if (token_information_class == TokenAppContainerSid)
            {
                constexpr auto required_size = sizeof(EmulatorTraits<Emu64>::PVOID);
                return_length.write_if_valid(required_size);
                if (token_information_length < required_size)
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }
                emulator_object<EmulatorTraits<Emu64>::PVOID>{c.emu, token_information}.write(0);
                return STATUS_SUCCESS;
            }

            const auto anonymous = token_handle == ANONYMOUS_IMPERSONATION_TOKEN;
            const auto sid = anonymous ? std::span<const uint8_t>{anonymous_sid} : std::span<const uint8_t>{c.proc.sid};

            if (token_information_class == TokenPrivileges)
            {
                constexpr auto required_size = sizeof(ULONG);
                return_length.write(required_size);
                if (token_information_length < required_size)
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }
                emulator_object<ULONG>{c.emu, token_information}.write(0);
                return STATUS_SUCCESS;
            }

            if (token_information_class == TokenUser)
            {
                const auto required_size = sizeof(TOKEN_USER64) + sid.size();
                return_length.write(static_cast<ULONG>(required_size));

                if (required_size > token_information_length)
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                TOKEN_USER64 user{};
                user.User.Attributes = 0;
                user.User.Sid = token_information + sizeof(TOKEN_USER64);

                emulator_object<TOKEN_USER64>{c.emu, token_information}.write(user);
                c.emu.write_memory(token_information + sizeof(TOKEN_USER64), sid.data(), sid.size());
                return STATUS_SUCCESS;
            }

            if (token_information_class == TokenGroups)
            {
                if (anonymous)
                {
                    return_length.write(sizeof(ULONG));
                    if (token_information_length < sizeof(ULONG))
                    {
                        return STATUS_BUFFER_TOO_SMALL;
                    }
                    c.emu.write_memory(token_information, ULONG{0});
                    return STATUS_SUCCESS;
                }
                const auto entries = get_token_groups(c.win_emu, sid);

                constexpr auto group_entry_size = sizeof(SID_AND_ATTRIBUTES64);
                auto required_size = sizeof(TOKEN_GROUPS64) + (entries.size() - 1) * group_entry_size;
                for (const auto& entry : entries)
                {
                    required_size += entry.sid.size();
                }

                return_length.write(static_cast<ULONG>(required_size));

                if (required_size > token_information_length)
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                auto sid_cursor = token_information + sizeof(TOKEN_GROUPS64) + (entries.size() - 1) * group_entry_size;

                TOKEN_GROUPS64 groups{};
                groups.GroupCount = static_cast<ULONG>(entries.size());
                groups.Groups[0].Attributes = entries[0].attributes;
                groups.Groups[0].Sid = sid_cursor;
                emulator_object<TOKEN_GROUPS64>{c.emu, token_information}.write(groups);

                for (size_t i = 0; i < entries.size(); ++i)
                {
                    if (i > 0)
                    {
                        SID_AND_ATTRIBUTES64 group{};
                        group.Attributes = entries[i].attributes;
                        group.Sid = sid_cursor;
                        c.emu.write_memory(token_information + sizeof(TOKEN_GROUPS64) + (i - 1) * group_entry_size, group);
                    }

                    c.emu.write_memory(sid_cursor, entries[i].sid.data(), entries[i].sid.size());
                    sid_cursor += entries[i].sid.size();
                }

                return STATUS_SUCCESS;
            }

            if (token_information_class == TokenOwner)
            {
                const auto required_size = sizeof(TOKEN_OWNER64) + sid.size();
                return_length.write(static_cast<ULONG>(required_size));

                if (required_size > token_information_length)
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                TOKEN_OWNER64 owner{};
                owner.Owner = token_information + sizeof(TOKEN_OWNER64);

                emulator_object<TOKEN_OWNER64>{c.emu, token_information}.write(owner);
                c.emu.write_memory(token_information + sizeof(TOKEN_OWNER64), sid.data(), sid.size());
                return STATUS_SUCCESS;
            }

            if (token_information_class == TokenPrimaryGroup)
            {
                // The group TokenGroups lists first; a user outside an account domain (S-1-5-7) keeps its own SID.
                const auto account_group = account_domain_users_sid(sid);
                const auto group_sid = account_group ? std::span<const uint8_t>{*account_group} : sid;
                const auto required_size = sizeof(TOKEN_PRIMARY_GROUP64) + group_sid.size();
                return_length.write(static_cast<ULONG>(required_size));

                if (required_size > token_information_length)
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                TOKEN_PRIMARY_GROUP64 primary_group{};
                primary_group.PrimaryGroup = token_information + sizeof(TOKEN_PRIMARY_GROUP64);

                emulator_object<TOKEN_PRIMARY_GROUP64>{c.emu, token_information}.write(primary_group);
                c.emu.write_memory(token_information + sizeof(TOKEN_PRIMARY_GROUP64), group_sid.data(), group_sid.size());
                return STATUS_SUCCESS;
            }

            if (token_information_class == TokenDefaultDacl)
            {
                const auto acl_size = sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) + sid.size() - sizeof(ULONG);
                const auto required_size = sizeof(TOKEN_DEFAULT_DACL64) + acl_size;
                return_length.write(static_cast<ULONG>(required_size));

                if (required_size > token_information_length)
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                TOKEN_DEFAULT_DACL64 default_dacl{};
                default_dacl.DefaultDacl = token_information + sizeof(TOKEN_DEFAULT_DACL64);

                emulator_object<TOKEN_DEFAULT_DACL64>{c.emu, token_information}.write(default_dacl);

                const auto acl_offset = token_information + sizeof(TOKEN_DEFAULT_DACL64);
                ACL acl{};
                acl.AclRevision = 2; // ACL_REVISION
                acl.Sbz1 = 0;
                acl.AclSize = static_cast<USHORT>(acl_size);
                acl.AceCount = 1;
                acl.Sbz2 = 0;

                c.emu.write_memory(acl_offset, acl);

                const auto ace_offset = acl_offset + sizeof(ACL);
                ACCESS_ALLOWED_ACE ace{};
                ace.Header.AceType = 0; // ACCESS_ALLOWED_ACE_TYPE
                ace.Header.AceFlags = 0;
                ace.Header.AceSize = static_cast<USHORT>(sizeof(ACCESS_ALLOWED_ACE) + sid.size() - sizeof(ULONG));
                ace.Mask = GENERIC_ALL;

                c.emu.write_memory(ace_offset, ace);

                const auto sid_offset = ace_offset + sizeof(ACCESS_ALLOWED_ACE) - sizeof(ULONG);
                c.emu.write_memory(sid_offset, sid.data(), sid.size());

                return STATUS_SUCCESS;
            }

            if (token_information_class == TokenType)
            {
                constexpr auto required_size = sizeof(TOKEN_TYPE);
                return_length.write(required_size);

                if (required_size > token_information_length)
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                emulator_object<TOKEN_TYPE>{c.emu, token_information}.write(get_token_type(token_handle));
                return STATUS_SUCCESS;
            }

            if (token_information_class == TokenImpersonationLevel)
            {
                constexpr auto required_size = sizeof(SECURITY_IMPERSONATION_LEVEL);
                return_length.write(required_size);

                if (required_size > token_information_length)
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                const auto level =
                    !anonymous && get_token_type(token_handle) == TokenImpersonation ? SecurityImpersonation : SecurityAnonymous;
                emulator_object<SECURITY_IMPERSONATION_LEVEL>{c.emu, token_information}.write(level);
                return STATUS_SUCCESS;
            }

            if (token_information_class == TokenSessionId)
            {
                constexpr auto required_size = sizeof(ULONG);
                return_length.write(required_size);

                if (required_size > token_information_length)
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                emulator_object<ULONG>{c.emu, token_information}.write(token_session_id);
                return STATUS_SUCCESS;
            }

            if (token_information_class == TokenPrivateNameSpace)
            {
                constexpr auto required_size = sizeof(ULONG);
                return_length.write(required_size);

                if (required_size > token_information_length)
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                emulator_object<ULONG>{c.emu, token_information}.write(0);
                return STATUS_SUCCESS;
            }

            if (token_information_class == TokenUIAccess)
            {
                constexpr auto required_size = sizeof(ULONG);
                return_length.write(required_size);

                if (required_size > token_information_length)
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                emulator_object<ULONG>{c.emu, token_information}.write(1);
                return STATUS_SUCCESS;
            }

            if (token_information_class == TokenElevation)
            {
                constexpr auto required_size = sizeof(TOKEN_ELEVATION);
                return_length.write(required_size);

                if (required_size > token_information_length)
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                c.emu.write_memory(token_information, TOKEN_ELEVATION{
                                                          .TokenIsElevated = !anonymous && c.win_emu.runs_elevated() ? 1u : 0u,
                                                      });
                return STATUS_SUCCESS;
            }

            if (token_information_class == TokenElevationType)
            {
                constexpr auto required_size = sizeof(ULONG);
                return_length.write(required_size);

                if (required_size > token_information_length)
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                // TokenElevationTypeDefault: no UAC split token, as for the built-in Administrator (RID 500) the token
                // belongs to.
                emulator_object<ULONG>{c.emu, token_information}.write(1);
                return STATUS_SUCCESS;
            }

            if (token_information_class == TokenSandBoxInert || token_information_class == TokenHasRestrictions ||
                token_information_class == TokenIsAppContainer || token_information_class == TokenIsAppSilo)
            {
                constexpr auto required_size = sizeof(ULONG);
                return_length.write(required_size);

                if (required_size > token_information_length)
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                emulator_object<ULONG>{c.emu, token_information}.write(0);
                return STATUS_SUCCESS;
            }

            if (token_information_class == TokenIsSandboxed || token_information_class == TokenIsAppSilo)
            {
                constexpr auto required_size = sizeof(ULONG);
                return_length.write(required_size);

                if (required_size > token_information_length)
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                emulator_object<ULONG>{c.emu, token_information}.write(0);
                return STATUS_SUCCESS;
            }

            if (token_information_class == TokenStatistics)
            {
                constexpr auto required_size = sizeof(TOKEN_STATISTICS);
                return_length.write(required_size);

                if (required_size > token_information_length)
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
                TOKEN_STATISTICS stats{};
                stats.TokenId.LowPart = token_id;
                stats.AuthenticationId.LowPart = token_authentication_id;
                stats.ExpirationTime.QuadPart = std::numeric_limits<LONGLONG>::max();
                stats.TokenType = get_token_type(token_handle);
                stats.ImpersonationLevel = !anonymous && stats.TokenType == TokenImpersonation ? SecurityImpersonation : SecurityAnonymous;
                stats.GroupCount = anonymous ? 0 : static_cast<ULONG>(count_token_groups(c.win_emu, sid));
                stats.PrivilegeCount = 0;
                stats.ModifiedId.LowPart = token_modified_id;

                c.emu.write_memory(token_information, stats);

                return STATUS_SUCCESS;
            }

            if (token_information_class == TokenUserClaimAttributes || token_information_class == TokenDeviceClaimAttributes ||
                token_information_class == TokenRestrictedUserClaimAttributes ||
                token_information_class == TokenRestrictedDeviceClaimAttributes || token_information_class == TokenSecurityAttributes)
            {
                constexpr auto required_size = sizeof(TOKEN_SECURITY_ATTRIBUTES_INFORMATION);
                return_length.write(required_size);

                if (required_size > token_information_length)
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                c.emu.write_memory(token_information, TOKEN_SECURITY_ATTRIBUTES_INFORMATION{
                                                          .Version = 1,
                                                          .Reserved = {},
                                                          .AttributeCount = 0,
                                                          .Attribute = {},
                                                      });

                return STATUS_SUCCESS;
            }

            if (token_information_class == TokenIntegrityLevel)
            {
                // S-1-16-8192 (medium) or S-1-16-12288 (high)
                std::array<uint8_t, 20> integrity_sid = {
                    0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                };
                if (!anonymous && c.win_emu.runs_elevated())
                {
                    integrity_sid[9] = 0x30;
                }

                constexpr auto required_size = sizeof(integrity_sid) + sizeof(TOKEN_MANDATORY_LABEL64);
                return_length.write(required_size);

                if (required_size > token_information_length)
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                TOKEN_MANDATORY_LABEL64 label{};
                label.Label.Attributes = 0x60;
                label.Label.Sid = token_information + sizeof(TOKEN_MANDATORY_LABEL64);

                emulator_object<TOKEN_MANDATORY_LABEL64>{c.emu, token_information}.write(label);
                c.emu.write_memory(token_information + sizeof(TOKEN_MANDATORY_LABEL64), integrity_sid.data(), integrity_sid.size());
                return STATUS_SUCCESS;
            }

            if (token_information_class == TokenProcessTrustLevel)
            {
                constexpr auto required_size = sizeof(TOKEN_PROCESS_TRUST_LEVEL64);
                return_length.write(required_size);

                if (required_size > token_information_length)
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                c.emu.write_memory(token_information, TOKEN_PROCESS_TRUST_LEVEL64{
                                                          .TrustLevelSid = 0,
                                                      });

                return STATUS_SUCCESS;
            }

            if (token_information_class == TokenBnoIsolation)
            {
                constexpr auto required_size = sizeof(TOKEN_BNO_ISOLATION_INFORMATION64);
                return_length.write(required_size);

                if (required_size > token_information_length)
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                c.emu.write_memory(token_information, TOKEN_BNO_ISOLATION_INFORMATION64{
                                                          .IsolationPrefix = 0,
                                                          .IsolationEnabled = FALSE,
                                                      });

                return STATUS_SUCCESS;
            }

            c.win_emu.log.error("Unsupported token info class: %X\n", token_information_class);
            c.emu.stop();
            return STATUS_NOT_SUPPORTED;
        }

        NTSTATUS handle_NtAccessCheck(const syscall_context& c, const uint64_t security_descriptor, const handle requested_token,
                                      const ACCESS_MASK desired_access, const emulator_object<EMU_GENERIC_MAPPING> generic_mapping,
                                      const uint64_t /*privilege_set*/, const emulator_object<ULONG> privilege_set_length,
                                      const emulator_object<ACCESS_MASK> granted_access, const emulator_object<NTSTATUS> access_status)
        {
            const auto client_token = resolve_token(c, requested_token);
            if (client_token != CURRENT_PROCESS_TOKEN && client_token != CURRENT_THREAD_TOKEN &&
                client_token != CURRENT_THREAD_EFFECTIVE_TOKEN && client_token != DUMMY_IMPERSONATION_TOKEN &&
                client_token != ANONYMOUS_IMPERSONATION_TOKEN)
            {
                return STATUS_INVALID_HANDLE;
            }

            const auto control = emulator_object<SECURITY_DESCRIPTOR_CONTROL>{c.emu, security_descriptor + 2}.read();
            uint64_t dacl_address = 0;
            if (control & SE_SELF_RELATIVE)
            {
                const auto descriptor = emulator_object<SECURITY_DESCRIPTOR_RELATIVE>{c.emu, security_descriptor}.read();
                dacl_address = descriptor.Dacl == 0 ? 0 : security_descriptor + descriptor.Dacl;
            }
            else
            {
                dacl_address = emulator_object<uint64_t>{c.emu, security_descriptor + 32}.read();
            }

            const auto mapping = generic_mapping.read();
            const auto requested = map_generic_access(desired_access, mapping);
            ACCESS_MASK remaining = requested;
            ACCESS_MASK granted = 0;
            bool denied = false;

            if (!(control & SE_DACL_PRESENT) || dacl_address == 0)
            {
                granted = requested;
                remaining = 0;
            }
            else
            {
                const auto acl = emulator_object<ACL>{c.emu, dacl_address}.read();
                auto ace_address = dacl_address + sizeof(ACL);
                for (USHORT i = 0; i < acl.AceCount; ++i)
                {
                    const auto ace = emulator_object<ACCESS_ALLOWED_ACE>{c.emu, ace_address}.read();

                    // Only allow and deny ACEs take part. An ACE of another kind needs no SID: 19041 passes over one that is
                    // only a header.
                    if (ace.Header.AceType != ACCESS_ALLOWED_ACE_TYPE && ace.Header.AceType != ACCESS_DENIED_ACE_TYPE)
                    {
                        ace_address += ace.Header.AceSize;
                        continue;
                    }

                    const auto ace_sid = read_ace_sid(c.emu.memory(), ace_address, ace.Header.AceSize);
                    if (!ace_sid)
                    {
                        return STATUS_INVALID_ACL;
                    }

                    // SepNormalAccessCheck skips inherit-only ACEs and compares the masks as stored: an ACE's generic bits are
                    // not mapped, so they match no specific right.
                    const auto applies =
                        (ace.Header.AceFlags & INHERIT_ONLY_ACE) == 0 && token_holds_sid(c.win_emu, client_token, *ace_sid);
                    if (applies && ace.Header.AceType == ACCESS_DENIED_ACE_TYPE && (ace.Mask & remaining) != 0)
                    {
                        denied = true;
                        break;
                    }
                    if (applies && ace.Header.AceType == ACCESS_ALLOWED_ACE_TYPE)
                    {
                        const auto allowed = ace.Mask & remaining;
                        granted |= allowed;
                        remaining &= ~allowed;
                    }
                    ace_address += ace.Header.AceSize;
                }
            }

            privilege_set_length.write(0);
            granted_access.write(granted);
            access_status.write(!denied && remaining == 0 ? STATUS_SUCCESS : STATUS_ACCESS_DENIED);
            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtPrivilegeCheck(const syscall_context& c, const handle client_token, const emulator_pointer required_privileges,
                                         const emulator_object<BOOLEAN> result)
        {
            struct privilege_set_header
            {
                ULONG privilege_count;
                ULONG control;
            };

            constexpr ULONG privilege_set_all_necessary = 1;
            constexpr ULONG max_privilege_count = 66;
            constexpr uint64_t luid_and_attributes_size = 12;

            if (client_token != CURRENT_PROCESS_TOKEN && client_token != DUMMY_IMPERSONATION_TOKEN &&
                client_token != ANONYMOUS_IMPERSONATION_TOKEN)
            {
                const auto object_handle = c.proc.resolve_object_pseudo_handle(client_token, c.vcpu.active_thread);
                auto* store = c.proc.get_handle_store(object_handle);
                return store && store->get_object(object_handle) ? STATUS_OBJECT_TYPE_MISMATCH : STATUS_INVALID_HANDLE;
            }

            const auto header = emulator_object<privilege_set_header>{c.emu, required_privileges}.try_read();
            if (!header)
            {
                return STATUS_ACCESS_VIOLATION;
            }

            // PRIVILEGE_SET declares one entry, so Windows probes that much even for an empty set.
            const auto probed_entries = std::max<uint64_t>(header->privilege_count, 1);
            const auto set_size = sizeof(privilege_set_header) + (probed_entries * luid_and_attributes_size);
            if (!is_writable_range(c.win_emu.memory, required_privileges, set_size) ||
                !is_writable_range(c.win_emu.memory, result.value(), sizeof(BOOLEAN)))
            {
                return STATUS_ACCESS_VIOLATION;
            }

            if (header->privilege_count > max_privilege_count)
            {
                return STATUS_INVALID_PARAMETER;
            }

            const BOOLEAN granted = header->privilege_count == 0 && (header->control & privilege_set_all_necessary) != 0;
            result.write(granted);
            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtQuerySecurityAttributesToken(const syscall_context& c, const handle token_handle,
                                                       const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> attributes,
                                                       const ULONG number_of_attributes, const uint64_t buffer, const ULONG buffer_length,
                                                       const emulator_object<ULONG> return_length)
        {
            if (token_handle != CURRENT_PROCESS_TOKEN && token_handle != CURRENT_THREAD_TOKEN &&
                token_handle != CURRENT_THREAD_EFFECTIVE_TOKEN && token_handle != DUMMY_IMPERSONATION_TOKEN)
            {
                return STATUS_NOT_SUPPORTED;
            }

            if ((number_of_attributes != 0) != (attributes.value() != 0))
            {
                return STATUS_INVALID_PARAMETER;
            }

            if (number_of_attributes != 0)
            {
                for (ULONG index = 0; index < number_of_attributes; ++index)
                {
                    read_unicode_string(c.emu, attributes, index);
                }
                if (return_length.value())
                {
                    return_length.write(0);
                }
                return STATUS_NOT_FOUND;
            }

            constexpr auto required_size = sizeof(TOKEN_SECURITY_ATTRIBUTES_INFORMATION);
            if (return_length.value())
            {
                return_length.write(required_size);
            }

            if (buffer == 0 || buffer_length < required_size)
            {
                return STATUS_BUFFER_TOO_SMALL;
            }

            c.emu.write_memory(buffer, TOKEN_SECURITY_ATTRIBUTES_INFORMATION{
                                           .Version = 1,
                                           .Reserved = {},
                                           .AttributeCount = 0,
                                           .Attribute = {},
                                       });

            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtCompareSigningLevels(const syscall_context&, UCHAR, UCHAR)
        {
            return STATUS_NOT_SUPPORTED;
        }

        NTSTATUS handle_NtAdjustPrivilegesToken()
        {
            return STATUS_NOT_SUPPORTED;
        }

        NTSTATUS handle_NtQuerySecurityPolicy()
        {
            return STATUS_NOT_SUPPORTED;
        }
    }

    bool token_holds_sid(const windows_emulator& win_emu, const handle token, const std::span<const uint8_t> sid)
    {
        if (token == ANONYMOUS_IMPERSONATION_TOKEN)
        {
            return std::ranges::equal(sid, syscalls::anonymous_sid);
        }

        const auto& user = win_emu.process.sid;
        if (std::ranges::equal(sid, user) || syscalls::is_account_domain_users_sid(user, sid))
        {
            return true;
        }

        // The other enabled groups, compared where they are defined.
        bool held = false;
        syscalls::visit_standard_token_groups(win_emu.runs_elevated(), [&](const std::span<const uint8_t> group, const ULONG attributes) {
            held = held || ((attributes & syscalls::se_group_enabled) != 0 && std::ranges::equal(group, sid));
        });
        return held;
    }

    bool token_grants_access(const windows_emulator& win_emu, const handle token, const std::span<const std::byte> descriptor,
                             const ACCESS_MASK desired_access)
    {
        constexpr size_t sid_header_size = offsetof(SID, SubAuthority);
        constexpr size_t ace_sid_offset = offsetof(ACCESS_ALLOWED_ACE, SidStart);

        SECURITY_DESCRIPTOR_RELATIVE header{};
        if (descriptor.size() < sizeof(header))
        {
            return false;
        }

        memcpy(&header, descriptor.data(), sizeof(header));
        if ((header.Control & SE_SELF_RELATIVE) == 0)
        {
            return false;
        }

        if ((header.Control & SE_DACL_PRESENT) == 0 || header.Dacl == 0)
        {
            return true;
        }

        ACL acl{};
        if (header.Dacl > descriptor.size() - sizeof(acl))
        {
            return false;
        }

        memcpy(&acl, descriptor.data() + header.Dacl, sizeof(acl));
        if (acl.AclSize < sizeof(acl) || acl.AclSize > descriptor.size() - header.Dacl)
        {
            return false;
        }

        const auto aces = descriptor.subspan(header.Dacl, acl.AclSize);
        auto remaining = desired_access;
        size_t offset = sizeof(acl);
        for (USHORT index = 0; index < acl.AceCount && remaining != 0; ++index)
        {
            ACCESS_ALLOWED_ACE ace{};
            if (aces.size() - offset < ace_sid_offset)
            {
                return false;
            }

            memcpy(&ace, aces.data() + offset, ace_sid_offset);
            if (ace.Header.AceSize < ace_sid_offset || ace.Header.AceSize > aces.size() - offset)
            {
                return false;
            }

            const auto type = ace.Header.AceType;
            if ((type == ACCESS_ALLOWED_ACE_TYPE || type == ACCESS_DENIED_ACE_TYPE) && (ace.Header.AceFlags & INHERIT_ONLY_ACE) == 0)
            {
                const std::span sid{reinterpret_cast<const uint8_t*>(aces.data() + offset + ace_sid_offset),
                                    ace.Header.AceSize - ace_sid_offset};
                if (sid.size() < sid_header_size)
                {
                    return false;
                }

                SID sid_header{};
                memcpy(&sid_header, sid.data(), sid_header_size);
                const auto sid_size = sid_header_size + (sid_header.SubAuthorityCount * sizeof(DWORD));
                if (sid.size() < sid_size)
                {
                    return false;
                }

                if (token_holds_sid(win_emu, token, sid.first(sid_size)))
                {
                    if (type == ACCESS_DENIED_ACE_TYPE && (ace.Mask & remaining) != 0)
                    {
                        return false;
                    }

                    if (type == ACCESS_ALLOWED_ACE_TYPE)
                    {
                        remaining &= ~ace.Mask;
                    }
                }
            }

            offset += ace.Header.AceSize;
        }

        return remaining == 0;
    }

} // namespace sogen
