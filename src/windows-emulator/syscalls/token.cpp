#include "../std_include.hpp"
#include "../emulator_utils.hpp"
#include "../syscall_utils.hpp"

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

            constexpr std::array<uint8_t, 12> interactive_sid = {
                0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x04, 0x00, 0x00, 0x00,
            };

            constexpr std::array<uint8_t, 12> anonymous_sid{1, 1, 0, 0, 0, 0, 0, 5, 7, 0, 0, 0};

            // S-1-5-32-544, BUILTIN\Administrators
            constexpr std::array<uint8_t, 16> administrators_sid = {
                0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x20, 0x00, 0x00, 0x00, 0x20, 0x02, 0x00, 0x00,
            };

            struct token_group
            {
                std::span<const uint8_t> sid;
                ULONG attributes;
            };

            std::vector<token_group> get_token_groups(const syscall_context& c, const std::span<const uint8_t> sid)
            {
                std::vector<token_group> groups{
                    {sid, 0},
                    {interactive_sid, 0x7}, // SE_GROUP_MANDATORY | SE_GROUP_ENABLED_BY_DEFAULT | SE_GROUP_ENABLED
                };

                if (c.win_emu.runs_elevated())
                {
                    groups.push_back({administrators_sid, 0xF}); // the above | SE_GROUP_OWNER
                }

                return groups;
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

            bool sid_matches(const memory_manager& memory, const uint64_t address, const std::span<const uint8_t> expected)
            {
                std::array<uint8_t, 8> header{};
                memory.read_memory(address, header.data(), header.size());
                const auto size = 8 + static_cast<size_t>(header[1]) * sizeof(uint32_t);
                if (size != expected.size())
                {
                    return false;
                }

                std::vector<uint8_t> sid(size);
                memory.read_memory(address, sid.data(), sid.size());
                return std::ranges::equal(sid, expected);
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
                const auto entries = get_token_groups(c, sid);

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
                const auto required_size = sizeof(TOKEN_PRIMARY_GROUP64) + sid.size();
                return_length.write(static_cast<ULONG>(required_size));

                if (required_size > token_information_length)
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                TOKEN_PRIMARY_GROUP64 primary_group{};
                primary_group.PrimaryGroup = token_information + sizeof(TOKEN_PRIMARY_GROUP64);

                emulator_object<TOKEN_PRIMARY_GROUP64>{c.emu, token_information}.write(primary_group);
                c.emu.write_memory(token_information + sizeof(TOKEN_PRIMARY_GROUP64), sid.data(), sid.size());
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

                emulator_object<ULONG>{c.emu, token_information}.write(1);
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
                stats.GroupCount = anonymous ? 0 : static_cast<ULONG>(get_token_groups(c, sid).size());
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
                    if (ace.Header.AceSize < sizeof(ACCESS_ALLOWED_ACE))
                    {
                        return STATUS_INVALID_PARAMETER;
                    }

                    const auto ace_mask = map_generic_access(ace.Mask, mapping);
                    const auto ace_sid = ace_address + offsetof(ACCESS_ALLOWED_ACE, SidStart);
                    const auto anonymous = client_token == ANONYMOUS_IMPERSONATION_TOKEN;
                    const auto user_sid = anonymous ? std::span<const uint8_t>{anonymous_sid} : std::span<const uint8_t>{c.proc.sid};
                    const auto applies =
                        sid_matches(c.emu.memory(), ace_sid, user_sid) ||
                        (!anonymous && (sid_matches(c.emu.memory(), ace_sid, interactive_sid) ||
                                        (c.win_emu.runs_elevated() && sid_matches(c.emu.memory(), ace_sid, administrators_sid))));
                    if (applies && ace.Header.AceType == 1 && (ace_mask & remaining) != 0)
                    {
                        denied = true;
                        break;
                    }
                    if (applies && ace.Header.AceType == 0)
                    {
                        const auto allowed = ace_mask & remaining;
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

} // namespace sogen
