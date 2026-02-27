#include "../std_include.hpp"
#include "../emulator_utils.hpp"
#include "../syscall_utils.hpp"

namespace syscalls
{
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

        const auto& sid = c.win_emu.sid;

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
            const auto required_size = sizeof(TOKEN_GROUPS64) + sid.size();
            return_length.write(static_cast<ULONG>(required_size));

            if (required_size > token_information_length)
            {
                return STATUS_BUFFER_TOO_SMALL;
            }

            TOKEN_GROUPS64 groups{};
            groups.GroupCount = 1;
            groups.Groups[0].Attributes = 0;
            groups.Groups[0].Sid = token_information + sizeof(TOKEN_GROUPS64);

            emulator_object<TOKEN_GROUPS64>{c.emu, token_information}.write(groups);
            c.emu.write_memory(token_information + sizeof(TOKEN_GROUPS64), sid.data(), sid.size());
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

        if (token_information_class == TokenIsAppContainer)
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
            stats.GroupCount = 1;
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

    NTSTATUS handle_NtQuerySecurityAttributesToken(const syscall_context& c, const handle token_handle,
                                                   emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> /*attributes*/,
                                                   const ULONG /*number_of_attributes*/, const uint64_t buffer, const ULONG buffer_length,
                                                   const emulator_object<ULONG> return_length)
    {
        if (token_handle != CURRENT_PROCESS_TOKEN && token_handle != CURRENT_THREAD_TOKEN &&
            token_handle != CURRENT_THREAD_EFFECTIVE_TOKEN && token_handle != DUMMY_IMPERSONATION_TOKEN)
        {
            return STATUS_NOT_SUPPORTED;
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
}
