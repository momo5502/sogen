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

    NTSTATUS handle_NtDuplicateToken(const syscall_context&, const handle existing_token_handle,
                                     ACCESS_MASK /*desired_access*/,
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
                                            const TOKEN_INFORMATION_CLASS token_information_class,
                                            const uint64_t token_information, const ULONG token_information_length,
                                            const emulator_object<ULONG> return_length)
    {
        if (token_handle != CURRENT_PROCESS_TOKEN && token_handle != CURRENT_THREAD_TOKEN &&
            token_handle != CURRENT_THREAD_EFFECTIVE_TOKEN && token_handle != DUMMY_IMPERSONATION_TOKEN)
        {
            return STATUS_NOT_SUPPORTED;
        }

        // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays)
        const uint8_t sid[] = {
            0x01, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x15, 0x00, 0x00, 0x00, 0x84, 0x94,
            0xD4, 0x04, 0x4B, 0x68, 0x42, 0x34, 0x23, 0xBE, 0x69, 0x4E, 0xE9, 0x03, 0x00, 0x00,
        };

        if (token_information_class == TokenAppContainerSid)
        {
            return STATUS_NOT_SUPPORTED;
        }

        if (token_information_class == TokenUser)
        {
            constexpr auto required_size = sizeof(sid) + 0x10;
            return_length.write(required_size);

            if (required_size > token_information_length)
            {
                return STATUS_BUFFER_TOO_SMALL;
            }

            TOKEN_USER64 user{};
            user.User.Attributes = 0;
            user.User.Sid = token_information + 0x10;

            emulator_object<TOKEN_USER64>{c.emu, token_information}.write(user);
            c.emu.write_memory(token_information + 0x10, sid, sizeof(sid));
            return STATUS_SUCCESS;
        }

        if (token_information_class == TokenGroups)
        {
            constexpr auto required_size = sizeof(TOKEN_GROUPS64) + sizeof(sid);
            return_length.write(required_size);

            if (required_size > token_information_length)
            {
                return STATUS_BUFFER_TOO_SMALL;
            }

            TOKEN_GROUPS64 groups{};
            groups.GroupCount = 1;
            groups.Groups[0].Attributes = 0;
            groups.Groups[0].Sid = token_information + sizeof(TOKEN_GROUPS64);

            emulator_object<TOKEN_GROUPS64>{c.emu, token_information}.write(groups);
            c.emu.write_memory(token_information + sizeof(TOKEN_GROUPS64), sid, sizeof(sid));
            return STATUS_SUCCESS;
        }

        if (token_information_class == TokenOwner)
        {
            constexpr auto required_size = sizeof(sid) + sizeof(TOKEN_OWNER64);
            return_length.write(required_size);

            if (required_size > token_information_length)
            {
                return STATUS_BUFFER_TOO_SMALL;
            }

            TOKEN_OWNER64 owner{};
            owner.Owner = token_information + sizeof(TOKEN_OWNER64);

            emulator_object<TOKEN_OWNER64>{c.emu, token_information}.write(owner);
            c.emu.write_memory(token_information + sizeof(TOKEN_OWNER64), sid, sizeof(sid));
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

            c.emu.write_memory(token_information, TOKEN_STATISTICS{});

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
            constexpr auto required_size = sizeof(sid) + sizeof(TOKEN_MANDATORY_LABEL64);
            return_length.write(required_size);

            if (required_size > token_information_length)
            {
                return STATUS_BUFFER_TOO_SMALL;
            }

            TOKEN_MANDATORY_LABEL64 label{};
            label.Label.Attributes = 0;
            label.Label.Sid = token_information + sizeof(TOKEN_MANDATORY_LABEL64);

            emulator_object<TOKEN_MANDATORY_LABEL64>{c.emu, token_information}.write(label);
            c.emu.write_memory(token_information + sizeof(TOKEN_MANDATORY_LABEL64), sid, sizeof(sid));
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

    NTSTATUS handle_NtQuerySecurityAttributesToken()
    {
        // puts("NtQuerySecurityAttributesToken not supported");
        return STATUS_NOT_SUPPORTED;
    }
}