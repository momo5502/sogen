#include "../std_include.hpp"
#include "../emulator_utils.hpp"
#include "../syscall_utils.hpp"

namespace sogen
{

    namespace syscalls
    {
        namespace
        {
            constexpr std::array<uint8_t, 12> interactive_sid = {
                0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x04, 0x00, 0x00, 0x00,
            };

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
            return token_handle == DUMMY_IMPERSONATION_TOKEN //
                       ? TokenImpersonation
                       : TokenPrimary;
        }

        NTSTATUS handle_NtDuplicateToken(const syscall_context&, const handle existing_token_handle, ACCESS_MASK /*desired_access*/,
                                         const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>>
                                         /*object_attributes*/,
                                         const BOOLEAN /*effective_only*/, const TOKEN_TYPE type,
                                         const emulator_object<handle> new_token_handle)
        {
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

        NTSTATUS handle_NtQueryInformationToken(const syscall_context& c, const handle token_handle,
                                                const TOKEN_INFORMATION_CLASS token_information_class, const uint64_t token_information,
                                                const ULONG token_information_length, const emulator_object<ULONG> return_length)
        {
            if (token_handle != CURRENT_PROCESS_TOKEN && token_handle != CURRENT_THREAD_TOKEN &&
                token_handle != CURRENT_THREAD_EFFECTIVE_TOKEN && token_handle != DUMMY_IMPERSONATION_TOKEN)
            {
                return STATUS_NOT_SUPPORTED;
            }

            if (token_information_class == TokenAppContainerSid)
            {
                return STATUS_NOT_SUPPORTED;
            }

            const auto& sid = c.proc.sid;

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
                constexpr auto group_entry_size = sizeof(SID_AND_ATTRIBUTES64);
                const auto required_size = sizeof(TOKEN_GROUPS64) + group_entry_size + sid.size() + interactive_sid.size();
                return_length.write(static_cast<ULONG>(required_size));

                if (required_size > token_information_length)
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                TOKEN_GROUPS64 groups{};
                groups.GroupCount = 2;
                groups.Groups[0].Attributes = 0;
                groups.Groups[0].Sid = token_information + sizeof(TOKEN_GROUPS64) + group_entry_size;

                SID_AND_ATTRIBUTES64 interactive_group{};
                interactive_group.Attributes = 0x7; // SE_GROUP_MANDATORY | SE_GROUP_ENABLED_BY_DEFAULT | SE_GROUP_ENABLED
                interactive_group.Sid = groups.Groups[0].Sid + sid.size();

                emulator_object<TOKEN_GROUPS64>{c.emu, token_information}.write(groups);
                c.emu.write_memory(token_information + sizeof(TOKEN_GROUPS64), interactive_group);
                c.emu.write_memory(groups.Groups[0].Sid, sid.data(), sid.size());
                c.emu.write_memory(interactive_group.Sid, interactive_sid);
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

                const auto level = get_token_type(token_handle) == TokenImpersonation ? SecurityImpersonation : SecurityAnonymous;
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
                                                          .TokenIsElevated = 0,
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

                // TokenElevationTypeDefault: no UAC split token (matches the non-elevated TokenElevation above).
                emulator_object<ULONG>{c.emu, token_information}.write(1);
                return STATUS_SUCCESS;
            }

            if (token_information_class == TokenIsAppContainer || token_information_class == TokenIsAppSilo)
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

                TOKEN_STATISTICS stats{};
                stats.TokenType = get_token_type(token_handle);
                stats.ImpersonationLevel = stats.TokenType == TokenImpersonation ? SecurityImpersonation : SecurityAnonymous;
                stats.GroupCount = 2;
                stats.PrivilegeCount = 0;

                c.emu.write_memory(token_information, stats);

                return STATUS_SUCCESS;
            }

            if (token_information_class == TokenSecurityAttributes)
            {
                constexpr auto required_size = sizeof(TOKEN_SECURITY_ATTRIBUTES_INFORMATION);
                return_length.write(required_size);

                if (required_size > token_information_length)
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                c.emu.write_memory(token_information, TOKEN_SECURITY_ATTRIBUTES_INFORMATION{
                                                          .Version = 0,
                                                          .Reserved = {},
                                                          .AttributeCount = 0,
                                                          .Attribute = {},
                                                      });

                return STATUS_SUCCESS;
            }

            if (token_information_class == TokenIntegrityLevel)
            {
                // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays)
                const uint8_t medium_integrity_sid[] = {
                    0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                };

                constexpr auto required_size = sizeof(medium_integrity_sid) + sizeof(TOKEN_MANDATORY_LABEL64);
                return_length.write(required_size);

                if (required_size > token_information_length)
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                TOKEN_MANDATORY_LABEL64 label{};
                label.Label.Attributes = 0x60;
                label.Label.Sid = token_information + sizeof(TOKEN_MANDATORY_LABEL64);

                emulator_object<TOKEN_MANDATORY_LABEL64>{c.emu, token_information}.write(label);
                c.emu.write_memory(token_information + sizeof(TOKEN_MANDATORY_LABEL64), medium_integrity_sid, sizeof(medium_integrity_sid));
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

        NTSTATUS handle_NtAccessCheck(const syscall_context& c, const uint64_t security_descriptor, const handle client_token,
                                      const ACCESS_MASK desired_access, const emulator_object<EMU_GENERIC_MAPPING> generic_mapping,
                                      const uint64_t /*privilege_set*/, const emulator_object<ULONG> privilege_set_length,
                                      const emulator_object<ACCESS_MASK> granted_access, const emulator_object<NTSTATUS> access_status)
        {
            if (client_token != CURRENT_PROCESS_TOKEN && client_token != CURRENT_THREAD_TOKEN &&
                client_token != CURRENT_THREAD_EFFECTIVE_TOKEN && client_token != DUMMY_IMPERSONATION_TOKEN)
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
                    const bool applies =
                        sid_matches(c.emu.memory(), ace_sid, c.proc.sid) || sid_matches(c.emu.memory(), ace_sid, interactive_sid);
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

            if (number_of_attributes == 2)
            {
                const auto attribute_name_1 = read_unicode_string(c.emu, attributes, 0);
                const auto attribute_name_2 = read_unicode_string(c.emu, attributes, 1);
                if (attribute_name_1 == u"WIN://SYSAPPID" && attribute_name_2 == u"WIN://PKG")
                {
                    constexpr auto attrs_offset = sizeof(TOKEN_SECURITY_ATTRIBUTES_INFORMATION);
                    constexpr auto claims_offset = attrs_offset + 2 * sizeof(TOKEN_SECURITY_ATTRIBUTE_V1);
                    constexpr auto required_size = claims_offset + sizeof(uint64_t);

                    if (return_length.value())
                    {
                        return_length.write(required_size);
                    }

                    if (buffer == 0 || buffer_length < required_size)
                    {
                        return STATUS_BUFFER_TOO_SMALL;
                    }

                    const auto attrs_ptr = buffer + attrs_offset;
                    const auto claims_ptr = buffer + claims_offset;

                    TOKEN_SECURITY_ATTRIBUTES_INFORMATION info{};
                    info.Version = 1;
                    info.Reserved = 0;
                    info.AttributeCount = 2;
                    info.Attribute.pAttributeV1 = attrs_ptr;

                    std::array<TOKEN_SECURITY_ATTRIBUTE_V1, 2> attrs{};

                    attrs[0].ValueType = 3; // STRING
                    attrs[0].ValueCount = 0;
                    attrs[0].Values = 0;

                    attrs[1].ValueType = 2; // UINT64
                    attrs[1].ValueCount = 1;
                    attrs[1].Values = claims_ptr;

                    c.emu.write_memory(buffer, info);
                    c.emu.write_memory(attrs_ptr, attrs);
                    c.emu.write_memory(claims_ptr, 0);

                    return STATUS_SUCCESS;
                }
            }

            constexpr auto required_size = sizeof(TOKEN_SECURITY_ATTRIBUTES_INFORMATION);
            if (return_length.value())
            {
                return_length.write(required_size);
            }

            if (buffer == 0)
            {
                return STATUS_INVALID_PARAMETER;
            }

            if (buffer_length < required_size)
            {
                return STATUS_BUFFER_TOO_SMALL;
            }

            c.emu.write_memory(buffer, TOKEN_SECURITY_ATTRIBUTES_INFORMATION{
                                           .Version = 0,
                                           .Reserved = {},
                                           .AttributeCount = 0,
                                           .Attribute = {},
                                       });

            return STATUS_SUCCESS;
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
