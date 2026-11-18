#include "std_include.hpp"

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
        constexpr ACCESS_MASK maximum_allowed = 0x02000000;
        constexpr ACCESS_MASK access_system_security = 0x01000000;
        constexpr ACCESS_MASK read_control = 0x00020000;
        constexpr ACCESS_MASK write_dac = 0x00040000;
        constexpr ACCESS_MASK file_read_data = 0x1;
        constexpr ACCESS_MASK file_write_data = 0x2;
        constexpr ACCESS_MASK file_generic_read = 0x00120089;
        constexpr std::array<uint8_t, 12> owner_rights_sid{1, 1, 0, 0, 0, 0, 0, 3, 4, 0, 0, 0}; // S-1-3-4
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

        struct descriptor_layout
        {
            std::span<const uint8_t> owner{local_system_sid};
            std::span<const uint8_t> group{local_system_sid};
            bool has_dacl{true};
        };

        struct access_reply
        {
            NTSTATUS result;
            NTSTATUS status;
            ACCESS_MASK granted;
            ULONG privilege_set_length;
            uint64_t privilege_set_head;
        };

        using byte_buffer = std::vector<uint8_t>;

        const sid_bytes nt_authority_sid{1, 0, 0, 0, 0, 0, 0, 5}; // S-1-5, no sub-authorities
        constexpr uint16_t compound_ace_impersonation = 1;        // COMPOUND_ACE_IMPERSONATION

        template <typename T>
        void append_value(byte_buffer& buffer, const T& value)
        {
            const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
            buffer.insert(buffer.end(), bytes, bytes + sizeof(value));
        }

        void append_bytes(byte_buffer& buffer, const std::span<const uint8_t> bytes)
        {
            buffer.insert(buffer.end(), bytes.begin(), bytes.end());
        }

        byte_buffer joined(const std::initializer_list<std::span<const uint8_t>> parts)
        {
            byte_buffer buffer{};
            for (const auto part : parts)
            {
                append_bytes(buffer, part);
            }

            return buffer;
        }

        template <typename T>
        byte_buffer bytes_of(const T& value)
        {
            byte_buffer buffer{};
            append_value(buffer, value);
            return buffer;
        }

        // An ACE's header alone, with zeros up to a length.
        byte_buffer raw_ace_header(const uint8_t type, const uint16_t ace_size, const size_t length)
        {
            auto ace = bytes_of(ACE_HEADER{.AceType = type, .AceFlags = 0, .AceSize = ace_size});
            ace.resize(length);
            return ace;
        }

        // An ACE for FILE_READ_DATA: its header, the mask and a body, AceSize its length unless ace_size says otherwise.
        byte_buffer raw_ace(const uint8_t type, const std::span<const uint8_t> body, const std::optional<uint16_t> ace_size = std::nullopt)
        {
            const auto length = static_cast<uint16_t>(sizeof(ACE_HEADER) + sizeof(ACCESS_MASK) + body.size());
            auto ace = raw_ace_header(type, ace_size.value_or(length), sizeof(ACE_HEADER));
            append_value(ace, file_read_data);
            append_bytes(ace, body);
            return ace;
        }

        struct acl_shape
        {
            uint8_t revision{ACL_REVISION};
            std::optional<uint16_t> acl_size{};
            std::optional<uint16_t> ace_count{};
            size_t slack{}; // zeros after the ACEs, inside AclSize
        };

        byte_buffer raw_acl(const std::initializer_list<byte_buffer> aces, const acl_shape& shape = {})
        {
            byte_buffer body{};
            for (const auto& ace : aces)
            {
                append_bytes(body, ace);
            }

            body.resize(body.size() + shape.slack);
            auto acl = bytes_of(ACL{.AclRevision = shape.revision,
                                    .Sbz1 = 0,
                                    .AclSize = shape.acl_size.value_or(static_cast<uint16_t>(sizeof(ACL) + body.size())),
                                    .AceCount = shape.ace_count.value_or(static_cast<uint16_t>(aces.size())),
                                    .Sbz2 = 0});
            append_bytes(acl, body);
            return acl;
        }

        // A self-relative descriptor laid out byte by byte. A part's pad puts that many bytes before it and realigns the next.
        struct raw_descriptor
        {
            std::optional<byte_buffer> dacl{};
            std::optional<byte_buffer> sacl{};
            std::optional<byte_buffer> owner{byte_buffer{local_system_sid.begin(), local_system_sid.end()}};
            std::optional<byte_buffer> group{byte_buffer{local_system_sid.begin(), local_system_sid.end()}};
            uint8_t revision{SECURITY_DESCRIPTOR_REVISION};
            SECURITY_DESCRIPTOR_CONTROL control{};
            size_t owner_pad{};
            size_t group_pad{};
            size_t dacl_pad{};
        };

        byte_buffer lay_out(const raw_descriptor& shape)
        {
            byte_buffer body{};
            const auto place = [&](const std::optional<byte_buffer>& part, const size_t pad) {
                if (!part)
                {
                    return DWORD{0};
                }

                body.resize(body.size() + pad);
                const auto offset = static_cast<DWORD>(sizeof(SECURITY_DESCRIPTOR_RELATIVE) + body.size());
                append_bytes(body, *part);
                body.resize(body.size() + ((sizeof(DWORD) - (pad % sizeof(DWORD))) % sizeof(DWORD)));
                return offset;
            };

            SECURITY_DESCRIPTOR_RELATIVE header{};
            header.Revision = shape.revision;
            header.Control = static_cast<SECURITY_DESCRIPTOR_CONTROL>(
                SE_SELF_RELATIVE | shape.control | (shape.dacl ? SE_DACL_PRESENT : 0) | (shape.sacl ? SE_SACL_PRESENT : 0));
            header.Owner = place(shape.owner, shape.owner_pad);
            header.Group = place(shape.group, shape.group_pad);
            header.Sacl = place(shape.sacl, 0);
            header.Dacl = place(shape.dacl, shape.dacl_pad);

            auto descriptor = bytes_of(header);
            append_bytes(descriptor, body);
            return descriptor;
        }

        // SECURITY_DESCRIPTOR as a 64-bit process lays out its absolute form.
        struct absolute_descriptor64
        {
            BYTE Revision;
            BYTE Sbz1;
            SECURITY_DESCRIPTOR_CONTROL Control;
            uint64_t Owner;
            uint64_t Group;
            uint64_t Sacl;
            uint64_t Dacl;
        };

        struct attribute_shape
        {
            WORD reserved{};
            DWORD flags{};
            std::optional<DWORD> value_count{};
            bool name_last_without_nul{};
        };

        // A CLAIM_SECURITY_ATTRIBUTE_RELATIVE_V1 named "A" with its values after the name, or, for name_last_without_nul, named
        // "AB" after the values and without its NUL.
        byte_buffer relative_attribute(const WORD value_type, const std::initializer_list<byte_buffer> values,
                                       const attribute_shape& shape = {})
        {
            constexpr std::array<uint8_t, 4> terminated_name{'A', 0, 0, 0};
            constexpr std::array<uint8_t, 4> unterminated_name{'A', 0, 'B', 0};
            const auto header_size = (4 * sizeof(DWORD)) + (values.size() * sizeof(DWORD));

            byte_buffer data{};
            auto name_offset = static_cast<DWORD>(header_size);
            if (!shape.name_last_without_nul)
            {
                append_bytes(data, terminated_name);
            }

            std::vector<DWORD> value_offsets{};
            for (const auto& value : values)
            {
                value_offsets.push_back(static_cast<DWORD>(header_size + data.size()));
                append_bytes(data, value);
            }

            if (shape.name_last_without_nul)
            {
                name_offset = static_cast<DWORD>(header_size + data.size());
                append_bytes(data, unterminated_name);
            }

            auto attribute = bytes_of(name_offset);
            append_value(attribute, value_type);
            append_value(attribute, shape.reserved);
            append_value(attribute, shape.flags);
            append_value(attribute, shape.value_count.value_or(static_cast<DWORD>(values.size())));
            for (const auto offset : value_offsets)
            {
                append_value(attribute, offset);
            }

            append_bytes(attribute, data);
            return attribute;
        }

        // A SYSTEM_RESOURCE_ATTRIBUTE_ACE for Everyone, padded to whole DWORDs.
        byte_buffer attribute_ace(const byte_buffer& attribute)
        {
            auto body = joined({everyone_sid, attribute});
            body.resize(body.size() + ((sizeof(DWORD) - (body.size() % sizeof(DWORD))) % sizeof(DWORD)));
            return raw_ace(SYSTEM_RESOURCE_ATTRIBUTE_ACE_TYPE, body);
        }

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

            // A self-relative descriptor with the layout's owner and group, an empty one left out, and a DACL that holds the
            // ACEs in order.
            uint64_t build_descriptor(const std::initializer_list<test_ace> aces, const descriptor_layout& layout)
            {
                const auto descriptor_address = this->storage + 0x1000;
                auto offset = static_cast<uint32_t>(sizeof(SECURITY_DESCRIPTOR_RELATIVE));
                const auto place = [&](const std::span<const uint8_t> sid) {
                    if (sid.empty())
                    {
                        return DWORD{0};
                    }

                    const auto sid_offset = offset;
                    this->emu->memory.write_memory(descriptor_address + sid_offset, sid.data(), sid.size());
                    offset += static_cast<uint32_t>(sid.size());
                    return DWORD{sid_offset};
                };

                SECURITY_DESCRIPTOR_RELATIVE descriptor{};
                descriptor.Revision = 1;
                descriptor.Control = SE_SELF_RELATIVE | (layout.has_dacl ? SE_DACL_PRESENT : 0);
                descriptor.Owner = place(layout.owner);
                descriptor.Group = place(layout.group);
                if (layout.has_dacl)
                {
                    descriptor.Dacl = offset;
                    auto ace_address = descriptor_address + offset + sizeof(ACL);
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
                    acl.AclSize = static_cast<uint16_t>(ace_address - (descriptor_address + offset));
                    acl.AceCount = static_cast<uint16_t>(aces.size());
                    emulator_object<ACL>{this->emu->memory, descriptor_address + offset}.write(acl);
                }

                emulator_object<SECURITY_DESCRIPTOR_RELATIVE>{this->emu->memory, descriptor_address}.write(descriptor);
                return descriptor_address;
            }

            // NtAccessCheck on a descriptor, with a privilege set of privilege_set_size bytes.
            access_reply call_access_check(const handle token, const uint64_t descriptor, const ACCESS_MASK desired_access,
                                           const ULONG privilege_set_size = 0x40)
            {
                const emulator_object<EMU_GENERIC_MAPPING> mapping{this->emu->memory, this->storage + 0x800};
                mapping.write(file_generic_mapping);

                const emulator_object<ULONG> privilege_set_length{this->emu->memory, this->storage + 0x820};
                const emulator_object<ACCESS_MASK> granted{this->emu->memory, this->storage + 0x830};
                const emulator_object<NTSTATUS> status{this->emu->memory, this->storage + 0x840};
                const auto privilege_set = this->storage + 0x880;
                privilege_set_length.write(privilege_set_size);
                this->emu->memory.set_memory(privilege_set, 0xCC, 0x40);

                const auto result = syscalls::handle_NtAccessCheck(this->context(), descriptor, token, desired_access, mapping,
                                                                   privilege_set, privilege_set_length, granted, status);
                return {.result = result,
                        .status = status.read(),
                        .granted = granted.read(),
                        .privilege_set_length = privilege_set_length.read(),
                        .privilege_set_head = this->emu->memory.read_memory<uint64_t>(privilege_set)};
            }

            access_reply access_check(const handle token, const std::initializer_list<test_ace> aces, const ACCESS_MASK desired_access,
                                      const descriptor_layout& layout = {}, const ULONG privilege_set_size = 0x40)
            {
                return this->call_access_check(token, this->build_descriptor(aces, layout), desired_access, privilege_set_size);
            }

            void expect_access(const std::initializer_list<test_ace> aces, const ACCESS_MASK desired_access, const NTSTATUS status,
                               const ACCESS_MASK granted, const descriptor_layout& layout = {})
            {
                const auto reply = this->access_check(CURRENT_PROCESS_TOKEN, aces, desired_access, layout);
                ASSERT_EQ(STATUS_SUCCESS, reply.result);
                EXPECT_EQ(status, reply.status);
                EXPECT_EQ(granted, reply.granted);
            }

            NTSTATUS check_access(const handle token, const std::initializer_list<test_ace> aces, const ACCESS_MASK desired_access)
            {
                const auto reply = this->access_check(token, aces, desired_access);
                EXPECT_EQ(STATUS_SUCCESS, reply.result);
                return reply.status;
            }

            NTSTATUS check_access(const handle token, const std::span<const uint8_t> sid, const ACCESS_MASK mask)
            {
                return this->check_access(token, {test_ace{.mask = mask, .sid = sid}}, mask);
            }

            // Where call_arguments puts NtAccessCheck's arguments, each replaceable by another address.
            struct argument_addresses
            {
                std::optional<uint64_t> mapping{};
                std::optional<uint64_t> privilege_set{};
                std::optional<uint64_t> privilege_set_length{};
                std::optional<uint64_t> granted{};
                std::optional<uint64_t> status{};
                std::optional<uint64_t> descriptor{};
            };

            // NtAccessCheck on a descriptor that grants FILE_READ_DATA, with its arguments at the given addresses (the
            // standard ones where none is given): what it returns.
            NTSTATUS call_arguments(const argument_addresses& at, const ACCESS_MASK desired_access,
                                    const handle token = CURRENT_PROCESS_TOKEN, const ULONG privilege_set_size = 0x40)
            {
                const auto descriptor = this->build_descriptor({{.mask = file_read_data, .sid = interactive_sid}}, {});
                const emulator_object<EMU_GENERIC_MAPPING> mapping{this->emu->memory, this->storage + 0x800};
                mapping.write(file_generic_mapping);
                const emulator_object<ULONG> privilege_set_length{this->emu->memory, this->storage + 0x820};
                privilege_set_length.write(privilege_set_size);

                return syscalls::handle_NtAccessCheck(this->context(), at.descriptor.value_or(descriptor), token, desired_access,
                                                      this->object_at<EMU_GENERIC_MAPPING>(at.mapping, this->storage + 0x800),
                                                      at.privilege_set.value_or(this->storage + 0x880),
                                                      this->object_at<ULONG>(at.privilege_set_length, this->storage + 0x820),
                                                      this->object_at<ACCESS_MASK>(at.granted, this->storage + 0x830),
                                                      this->object_at<NTSTATUS>(at.status, this->storage + 0x840));
            }

            template <typename T>
            emulator_object<T> object_at(const std::optional<uint64_t>& address, const uint64_t standard)
            {
                return {this->emu->memory, address.value_or(standard)};
            }

            // NtAccessCheck on descriptor bytes placed past an aligned address.
            access_reply check_raw_descriptor(const std::span<const uint8_t> descriptor, const size_t misalignment = 0,
                                              const ACCESS_MASK desired_access = file_read_data)
            {
                const auto address = this->storage + 0x1000 + misalignment;
                this->emu->memory.write_memory(address, descriptor.data(), descriptor.size());
                return this->call_access_check(CURRENT_PROCESS_TOKEN, address, desired_access);
            }

            // What NtAccessCheck answers for FILE_READ_DATA on a laid-out descriptor, and, when it takes the descriptor, the
            // access status.
            void expect_capture(const std::string& name, const raw_descriptor& shape, const NTSTATUS result, const size_t misalignment = 0,
                                const NTSTATUS access_status = STATUS_SUCCESS)
            {
                const auto reply = this->check_raw_descriptor(lay_out(shape), misalignment);
                EXPECT_EQ(result, reply.result) << name;
                if (result == STATUS_SUCCESS)
                {
                    EXPECT_EQ(access_status, reply.status) << name;
                }
            }

            void expect_dacl(const std::string& name, const byte_buffer& dacl, const NTSTATUS result)
            {
                this->expect_capture(name, {.dacl = dacl}, result);
            }

            // NtAccessCheck for FILE_READ_DATA on an absolute descriptor past an aligned address, whose DACL allows it for
            // INTERACTIVE unless dacl names another address.
            access_reply check_absolute_descriptor(const size_t misalignment, const std::optional<uint64_t> dacl = std::nullopt)
            {
                const auto sid = this->storage + 0x1800;
                const auto allowing_dacl = sid + 0x20;
                const auto allow = raw_acl({raw_ace(ACCESS_ALLOWED_ACE_TYPE, interactive_sid)});
                this->emu->memory.write_memory(sid, local_system_sid.data(), local_system_sid.size());
                this->emu->memory.write_memory(allowing_dacl, allow.data(), allow.size());

                const absolute_descriptor64 descriptor{.Revision = SECURITY_DESCRIPTOR_REVISION,
                                                       .Sbz1 = 0,
                                                       .Control = SE_DACL_PRESENT,
                                                       .Owner = sid,
                                                       .Group = sid,
                                                       .Sacl = 0,
                                                       .Dacl = dacl.value_or(allowing_dacl)};
                const auto address = this->storage + 0x1000 + misalignment;
                this->emu->memory.write_memory(address, &descriptor, sizeof(descriptor));
                return this->call_access_check(CURRENT_PROCESS_TOKEN, address, file_read_data);
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

    // SeCaptureSecurityDescriptor's RtlValidAcl fails a DACL with STATUS_INVALID_ACL when an allow or callback ACE is too short
    // for the SID its header describes (AceSize >= 16 + 4 * SubAuthorityCount), before the descriptor's owner and group,
    // ACCESS_SYSTEM_SECURITY or the privilege set's size are looked at (19041's AccessCheck on the host): a descriptor the
    // caller owns gets no READ_CONTROL from it.
    TEST_P(ElevatedTokenTest, AnAceTooShortForItsSidMakesTheAclInvalid)
    {
        this->emu->process.sid = local_user_sid;
        constexpr std::array<uint8_t, 8> sid_header_of_six{1, 6, 0, 0, 0, 0, 0, 5}; // S-1-5 and six sub-authorities not there
        constexpr std::array<uint16_t, 2> ace_sizes{16, 12};
        constexpr std::array<uint8_t, 2> ace_types{ACCESS_ALLOWED_ACE_TYPE, ACCESS_ALLOWED_CALLBACK_ACE_TYPE};
        const std::array layouts{descriptor_layout{}, descriptor_layout{.owner = {}}, descriptor_layout{.owner = local_user_sid}};
        for (const auto ace_type : ace_types)
        {
            for (const auto& layout : layouts)
            {
                for (const auto ace_size : ace_sizes)
                {
                    const auto descriptor = this->build_descriptor({}, layout);
                    const auto dacl = descriptor + emulator_object<SECURITY_DESCRIPTOR_RELATIVE>{this->emu->memory, descriptor}.read().Dacl;

                    ACCESS_ALLOWED_ACE ace{};
                    ace.Header.AceType = ace_type;
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

                    EXPECT_EQ(STATUS_INVALID_ACL, this->call_access_check(CURRENT_PROCESS_TOKEN, descriptor, file_generic_read).result)
                        << ace_size;
                    EXPECT_EQ(STATUS_INVALID_ACL, this->call_access_check(CURRENT_PROCESS_TOKEN, descriptor, read_control).result)
                        << ace_size;
                    EXPECT_EQ(STATUS_INVALID_ACL,
                              this->call_access_check(CURRENT_PROCESS_TOKEN, descriptor, access_system_security, 20).result)
                        << ace_size;
                    EXPECT_EQ(STATUS_INVALID_ACL, this->call_access_check(CURRENT_PROCESS_TOKEN, descriptor, file_generic_read, 0).result)
                        << ace_size;
                }
            }
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

    TEST_P(ElevatedTokenTest, RequestsWithGenericRightsAreNotMapped)
    {
        const auto reply = this->access_check(CURRENT_PROCESS_TOKEN, {{.mask = file_generic_read, .sid = interactive_sid}}, generic_read);
        EXPECT_EQ(STATUS_GENERIC_NOT_MAPPED, reply.result);
    }

    TEST_P(ElevatedTokenTest, DeniedRequestsGrantNothing)
    {
        this->expect_access({{.mask = file_read_data, .sid = interactive_sid}}, file_read_data | file_write_data, STATUS_ACCESS_DENIED, 0);
    }

    TEST_P(ElevatedTokenTest, MaximumAllowedGrantsWhatTheDaclAllows)
    {
        const test_ace allow_read{.mask = file_generic_read, .sid = interactive_sid};
        const test_ace deny_read_data{.type = ACCESS_DENIED_ACE_TYPE, .mask = file_read_data, .sid = interactive_sid};

        this->expect_access({allow_read}, maximum_allowed, STATUS_SUCCESS, file_generic_read);
        this->expect_access({allow_read, {.mask = write_dac, .sid = interactive_sid}}, maximum_allowed, STATUS_SUCCESS,
                            file_generic_read | write_dac);
        this->expect_access({deny_read_data, allow_read}, maximum_allowed, STATUS_SUCCESS, file_generic_read & ~file_read_data);
        this->expect_access({allow_read, deny_read_data}, maximum_allowed, STATUS_SUCCESS, file_generic_read);
        this->expect_access({allow_read}, maximum_allowed | file_write_data, STATUS_ACCESS_DENIED, 0);
        this->expect_access({{.mask = generic_all, .sid = local_system_sid}}, maximum_allowed, STATUS_ACCESS_DENIED, 0);
        this->expect_access({}, maximum_allowed, STATUS_SUCCESS, file_generic_mapping.GenericAll, {.has_dacl = false});
        this->expect_access({}, maximum_allowed, STATUS_ACCESS_DENIED, 0);
    }

    TEST_P(ElevatedTokenTest, OwnersHoldReadControlAndWriteDacUnlessAnOwnerRightsAceSaysOtherwise)
    {
        this->emu->process.sid = local_user_sid;
        const descriptor_layout owned{.owner = local_user_sid};
        const test_ace system_only{.mask = generic_all, .sid = local_system_sid};
        const test_ace owner_reads{.mask = file_read_data, .sid = owner_rights_sid};

        this->expect_access({system_only}, read_control, STATUS_SUCCESS, read_control, owned);
        this->expect_access({system_only}, read_control | write_dac, STATUS_SUCCESS, read_control | write_dac, owned);
        this->expect_access({system_only}, maximum_allowed, STATUS_SUCCESS, read_control | write_dac, owned);
        this->expect_access({system_only}, read_control | file_read_data, STATUS_ACCESS_DENIED, 0, owned);
        this->expect_access({}, read_control, STATUS_SUCCESS, read_control, owned);
        this->expect_access({owner_reads}, read_control, STATUS_ACCESS_DENIED, 0, owned);
        this->expect_access({owner_reads}, file_read_data, STATUS_SUCCESS, file_read_data, owned);
        this->expect_access({owner_reads}, file_read_data, STATUS_ACCESS_DENIED, 0);
    }

    TEST_P(ElevatedTokenTest, DescriptorsNeedAnOwnerAndAGroupAndThePrivilegeSetRoomForOne)
    {
        const test_ace allow_read{.mask = file_generic_read, .sid = interactive_sid};
        EXPECT_EQ(STATUS_INVALID_SECURITY_DESCR,
                  this->access_check(CURRENT_PROCESS_TOKEN, {allow_read}, file_generic_read, {.owner = {}}).result);
        EXPECT_EQ(STATUS_INVALID_SECURITY_DESCR,
                  this->access_check(CURRENT_PROCESS_TOKEN, {allow_read}, file_generic_read, {.group = {}}).result);

        const auto short_set = this->access_check(CURRENT_PROCESS_TOKEN, {allow_read}, file_generic_read, {}, 19);
        EXPECT_EQ(STATUS_BUFFER_TOO_SMALL, short_set.result);
        EXPECT_EQ(20u, short_set.privilege_set_length);

        const auto fitting = this->access_check(CURRENT_PROCESS_TOKEN, {allow_read}, file_generic_read, {}, 20);
        ASSERT_EQ(STATUS_SUCCESS, fitting.result);
        EXPECT_EQ(STATUS_SUCCESS, fitting.status);
        EXPECT_EQ(20u, fitting.privilege_set_length);
        EXPECT_EQ(0u, fitting.privilege_set_head);
    }

    // SeAccessCheckByType refuses it once the descriptor has an owner and a group, before it sizes the privilege set or gives
    // the owner READ_CONTROL (19041's AccessCheck on the host: a set of 0 bytes keeps its length and contents).
    TEST_P(ElevatedTokenTest, SystemSecurityAccessNeedsAPrivilegeTheTokenLacks)
    {
        constexpr uint64_t untouched_privilege_set = 0xCCCCCCCCCCCCCCCC;
        const test_ace allow_all{.mask = file_generic_mapping.GenericAll | access_system_security, .sid = interactive_sid};
        this->expect_access({allow_all}, access_system_security, STATUS_PRIVILEGE_NOT_HELD, 0);

        const auto no_set = this->access_check(CURRENT_PROCESS_TOKEN, {allow_all}, access_system_security, {}, 0);
        ASSERT_EQ(STATUS_SUCCESS, no_set.result);
        EXPECT_EQ(STATUS_PRIVILEGE_NOT_HELD, no_set.status);
        EXPECT_EQ(0u, no_set.granted);
        EXPECT_EQ(0u, no_set.privilege_set_length);
        EXPECT_EQ(untouched_privilege_set, no_set.privilege_set_head);

        EXPECT_EQ(STATUS_INVALID_SECURITY_DESCR,
                  this->access_check(CURRENT_PROCESS_TOKEN, {allow_all}, access_system_security, {.owner = {}}, 0).result);

        this->emu->process.sid = local_user_sid;
        this->expect_access({}, read_control | access_system_security, STATUS_PRIVILEGE_NOT_HELD, 0, {.owner = local_user_sid});
    }

    // RtlpOwnerAcesPresent finds OWNER RIGHTS in callback and object ACEs too, unless they are inherit-only; an object ACE
    // keeps its SID after the object type GUIDs its flags announce. A callback ACE grants nothing to NtAccessCheck itself.
    TEST_P(ElevatedTokenTest, OwnerRightsCountInCallbackAndObjectAces)
    {
        this->emu->process.sid = local_user_sid;
        const descriptor_layout owned{.owner = local_user_sid};
        constexpr std::array<uint8_t, 2> callback_types{ACCESS_ALLOWED_CALLBACK_ACE_TYPE, ACCESS_DENIED_CALLBACK_ACE_TYPE};
        constexpr uint8_t inherit_only = object_inherit_ace | INHERIT_ONLY_ACE;
        for (const auto type : callback_types)
        {
            this->expect_access({{.type = type, .mask = file_read_data, .sid = owner_rights_sid}}, read_control, STATUS_ACCESS_DENIED, 0,
                                owned);
            this->expect_access({{.type = type, .flags = inherit_only, .mask = file_read_data, .sid = owner_rights_sid}}, read_control,
                                STATUS_SUCCESS, read_control, owned);
        }

        const auto descriptor = this->build_descriptor({}, owned);
        const auto dacl = descriptor + emulator_object<SECURITY_DESCRIPTOR_RELATIVE>{this->emu->memory, descriptor}.read().Dacl;
        constexpr auto sid_offset = offsetof(ACCESS_ALLOWED_OBJECT_ACE, InheritedObjectType); // after one GUID
        ACCESS_ALLOWED_OBJECT_ACE ace{};
        ace.Header.AceType = ACCESS_ALLOWED_OBJECT_ACE_TYPE;
        ace.Header.AceSize = static_cast<uint16_t>(sid_offset + owner_rights_sid.size());
        ace.Mask = file_read_data;
        ace.Flags = ACE_OBJECT_TYPE_PRESENT;
        ace.ObjectType.Data1 = 1;
        this->emu->memory.write_memory(dacl + sizeof(ACL), &ace, sid_offset);
        this->emu->memory.write_memory(dacl + sizeof(ACL) + sid_offset, owner_rights_sid.data(), owner_rights_sid.size());

        ACL acl{};
        acl.AclRevision = ACL_REVISION_DS;
        acl.AclSize = static_cast<uint16_t>(sizeof(ACL) + ace.Header.AceSize);
        acl.AceCount = 1;
        emulator_object<ACL>{this->emu->memory, dacl}.write(acl);

        const auto reply = this->call_access_check(CURRENT_PROCESS_TOKEN, descriptor, read_control);
        ASSERT_EQ(STATUS_SUCCESS, reply.result);
        EXPECT_EQ(STATUS_ACCESS_DENIED, reply.status);
        EXPECT_EQ(0u, reply.granted);

        // RtlpValidObjectAce wants the whole SID after the GUIDs: without room for its sub-authority the DACL is invalid.
        ace.Header.AceSize = static_cast<uint16_t>(sid_offset + offsetof(SID, SubAuthority));
        this->emu->memory.write_memory(dacl + sizeof(ACL), &ace, sid_offset);
        acl.AclSize = static_cast<uint16_t>(sizeof(ACL) + ace.Header.AceSize);
        emulator_object<ACL>{this->emu->memory, dacl}.write(acl);
        EXPECT_EQ(STATUS_INVALID_ACL, this->call_access_check(CURRENT_PROCESS_TOKEN, descriptor, read_control).result);
    }

    // SeCaptureSecurityDescriptor, for a caller in user mode, checks the descriptor's alignment (4 bytes, 8 for the absolute
    // form) and revision, sizes the SACL and the DACL (AclSize rounded up to DWORDs, 8 bytes to 64 KB), probes the owner and
    // the group, then has RtlValidAcl take the SACL and the DACL and checks both SIDs. SeAccessCheckByType then wants a
    // descriptor with an owner and a group (19041's NtAccessCheck on the host).
    TEST_P(ElevatedTokenTest, DescriptorsAreCheckedAsSeCaptureSecurityDescriptorChecksThem)
    {
        constexpr uint64_t user_probe_address = 0x7FFFFFFF0000; // MM_USER_PROBE_ADDRESS
        const auto allow = raw_acl({raw_ace(ACCESS_ALLOWED_ACE_TYPE, interactive_sid)});
        const auto revision_one_acl = raw_acl({}, {.revision = 1});
        const auto acl_of_size = [](const uint16_t size) { return raw_acl({}, {.acl_size = size}); };

        sid_bytes sixteen_sub_authorities{SID_REVISION, 16, 0, 0, 0, 0, 0, 5};
        sixteen_sub_authorities.resize(sixteen_sub_authorities.size() + (16 * sizeof(DWORD)));
        sid_bytes revision_two{local_system_sid.begin(), local_system_sid.end()};
        revision_two[offsetof(SID, Revision)] = 2;
        auto revision_seventeen = revision_two;
        revision_seventeen[offsetof(SID, Revision)] = 0x11;

        this->expect_capture("valid", {.dacl = allow}, STATUS_SUCCESS);
        this->expect_capture("at an offset of 4", {.dacl = allow}, STATUS_SUCCESS, 4);
        this->expect_capture("at an offset of 2", {.dacl = allow}, STATUS_DATATYPE_MISALIGNMENT, 2);
        this->expect_capture("revision 2", {.dacl = allow, .revision = 2}, STATUS_UNKNOWN_REVISION);
        this->expect_capture("revision 2 at an offset of 2", {.dacl = allow, .revision = 2}, STATUS_DATATYPE_MISALIGNMENT, 2);
        EXPECT_EQ(STATUS_INVALID_SECURITY_DESCR, this->call_access_check(CURRENT_PROCESS_TOKEN, 0, file_read_data).result);
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->call_access_check(CURRENT_PROCESS_TOKEN, user_probe_address, file_read_data).result);
        EXPECT_EQ(STATUS_SUCCESS, this->check_absolute_descriptor(8).result);
        EXPECT_EQ(STATUS_DATATYPE_MISALIGNMENT, this->check_absolute_descriptor(4).result);
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->check_absolute_descriptor(8, user_probe_address).result);

        // The ACLs are sized before the owner and the group are probed, which comes before anything is copied.
        constexpr std::array<uint16_t, 4> refused_sizes{0, 4, 7, 0xFFFD};
        for (const auto size : refused_sizes)
        {
            this->expect_capture("DACL of AclSize " + std::to_string(size), {.dacl = acl_of_size(size)}, STATUS_INVALID_ACL);
        }

        this->expect_capture("DACL of AclSize 8 without ACEs", {.dacl = acl_of_size(static_cast<uint16_t>(sizeof(ACL)))}, STATUS_SUCCESS, 0,
                             STATUS_ACCESS_DENIED);
        this->expect_capture("DACL at an offset of 2", {.dacl = allow, .dacl_pad = 2}, STATUS_DATATYPE_MISALIGNMENT);
        this->expect_capture("owner at an offset of 2", {.dacl = allow, .owner_pad = 2}, STATUS_DATATYPE_MISALIGNMENT);
        this->expect_capture("group at an offset of 2", {.dacl = allow, .group_pad = 2}, STATUS_DATATYPE_MISALIGNMENT);
        this->expect_capture("owner at an offset of 2, DACL of revision 1", {.dacl = revision_one_acl, .owner_pad = 2},
                             STATUS_DATATYPE_MISALIGNMENT);
        this->expect_capture("owner at an offset of 2, DACL of AclSize 0", {.dacl = acl_of_size(0), .owner_pad = 2}, STATUS_INVALID_ACL);

        // RtlValidAcl takes the SACL, then the DACL, before the SIDs are checked.
        this->expect_capture("SACL of AclSize 0", {.dacl = allow, .sacl = acl_of_size(0)}, STATUS_INVALID_ACL);
        this->expect_capture("SACL of revision 1", {.dacl = allow, .sacl = revision_one_acl}, STATUS_INVALID_ACL);
        this->expect_capture("SACL with an audit ACE", {.dacl = allow, .sacl = raw_acl({raw_ace(SYSTEM_AUDIT_ACE_TYPE, everyone_sid)})},
                             STATUS_SUCCESS);
        this->expect_capture("SACL present without an offset", {.dacl = allow, .control = SE_SACL_PRESENT}, STATUS_SUCCESS);
        this->expect_capture("SACL of revision 1, owner of 16 sub-authorities",
                             {.dacl = allow, .sacl = revision_one_acl, .owner = sixteen_sub_authorities}, STATUS_INVALID_ACL);
        this->expect_capture("DACL of revision 1, owner of 16 sub-authorities",
                             {.dacl = revision_one_acl, .owner = sixteen_sub_authorities}, STATUS_INVALID_ACL);

        // The owner and the group: at most 15 sub-authorities, and a revision whose low nibble is 1.
        this->expect_capture("owner of 16 sub-authorities", {.dacl = allow, .owner = sixteen_sub_authorities}, STATUS_INVALID_SID);
        this->expect_capture("group of 16 sub-authorities", {.dacl = allow, .group = sixteen_sub_authorities}, STATUS_INVALID_SID);
        this->expect_capture("owner of revision 2", {.dacl = allow, .owner = revision_two}, STATUS_INVALID_SID);
        this->expect_capture("owner of revision 0x11", {.dacl = allow, .owner = revision_seventeen}, STATUS_SUCCESS);
        this->expect_capture("owner of 16 sub-authorities, no group",
                             {.dacl = allow, .owner = sixteen_sub_authorities, .group = std::nullopt}, STATUS_INVALID_SID);
        this->expect_capture("no owner, group of 16 sub-authorities",
                             {.dacl = allow, .owner = std::nullopt, .group = sixteen_sub_authorities}, STATUS_INVALID_SID);
        this->expect_capture("no owner", {.dacl = allow, .owner = std::nullopt}, STATUS_INVALID_SECURITY_DESCR);

        // A descriptor the caller owns gets READ_CONTROL from an empty DACL, and nothing from one RtlValidAcl does not take.
        this->emu->process.sid = local_user_sid;
        const auto owned_reply = this->check_raw_descriptor(lay_out({.dacl = raw_acl({}), .owner = local_user_sid}), 0, read_control);
        ASSERT_EQ(STATUS_SUCCESS, owned_reply.result);
        EXPECT_EQ(read_control, owned_reply.granted);
        EXPECT_EQ(STATUS_INVALID_ACL,
                  this->check_raw_descriptor(lay_out({.dacl = revision_one_acl, .owner = local_user_sid}), 0, read_control).result);
    }

    // RtlValidAcl: an ACL of revision 2 to 4 whose ACEs, as many as AceCount says, each start at an even offset with a header
    // that ends before the ACL does, end inside the ACL and pass their type's check. That is whole DWORDs and a valid SID for
    // the types that keep one after the mask, an ACL of revision 3 and two SIDs for compound ACEs, an ACL of revision 4 and
    // the SID after the GUIDs for object ACEs, and 4 bytes for a type 19041 does not know (its NtAccessCheck on the host).
    TEST_P(ElevatedTokenTest, RtlValidAclChecksTheAclAndEachAceByItsType)
    {
        constexpr uint8_t unknown_type = SYSTEM_ACCESS_FILTER_ACE_TYPE + 1; // past the last type 19041 knows
        const auto allow = raw_ace(ACCESS_ALLOWED_ACE_TYPE, interactive_sid);
        this->expect_dacl("revision 1", raw_acl({}, {.revision = 1}), STATUS_INVALID_ACL);
        this->expect_dacl("revision 5", raw_acl({}, {.revision = 5}), STATUS_INVALID_ACL);
        this->expect_dacl("revision 3", raw_acl({allow}, {.revision = ACL_REVISION3}), STATUS_SUCCESS);
        this->expect_dacl("revision 4", raw_acl({allow}, {.revision = ACL_REVISION_DS}), STATUS_SUCCESS);
        this->expect_dacl("AceCount 1 without an ACE", raw_acl({}, {.ace_count = 1}), STATUS_INVALID_ACL);
        this->expect_dacl("AceCount 2 with one ACE", raw_acl({allow}, {.ace_count = 2}), STATUS_INVALID_ACL);
        this->expect_dacl("AceCount 1 with two ACEs", raw_acl({allow, raw_ace(ACCESS_DENIED_ACE_TYPE, interactive_sid)}, {.ace_count = 1}),
                          STATUS_SUCCESS);
        this->expect_dacl("an ACE past AclSize", raw_acl({allow}, {.acl_size = static_cast<uint16_t>(sizeof(ACL) + 16)}),
                          STATUS_INVALID_ACL);
        this->expect_dacl("a header that ends the ACL", raw_acl({allow, raw_ace_header(unknown_type, 4, 4)}), STATUS_INVALID_ACL);
        this->expect_dacl("a header and slack", raw_acl({allow, raw_ace_header(unknown_type, 4, 4)}, {.slack = 4}), STATUS_SUCCESS);
        this->expect_dacl("AceSize 3", raw_acl({allow, raw_ace_header(unknown_type, 3, 4)}, {.slack = 4}), STATUS_INVALID_ACL);
        this->expect_dacl("an ACE at an odd offset", raw_acl({raw_ace_header(unknown_type, 5, 5), allow}), STATUS_INVALID_ACL);
        this->expect_dacl("an ACE at an even offset", raw_acl({raw_ace_header(unknown_type, 6, 6), allow}), STATUS_SUCCESS);

        // The types that keep their SID right after the mask, among them mandatory label, scoped policy and trust label ACEs.
        auto revision_two_sid = everyone_sid;
        revision_two_sid[offsetof(SID, Revision)] = 2;
        sid_bytes sixteen_sub_authorities{SID_REVISION, 16, 0, 0, 0, 0, 0, 5};
        sixteen_sub_authorities.resize(sixteen_sub_authorities.size() + (16 * sizeof(DWORD)));
        constexpr std::array<uint8_t, 11> sid_ace_types{ACCESS_ALLOWED_ACE_TYPE,
                                                        ACCESS_DENIED_ACE_TYPE,
                                                        SYSTEM_AUDIT_ACE_TYPE,
                                                        SYSTEM_ALARM_ACE_TYPE,
                                                        ACCESS_ALLOWED_CALLBACK_ACE_TYPE,
                                                        ACCESS_DENIED_CALLBACK_ACE_TYPE,
                                                        SYSTEM_AUDIT_CALLBACK_ACE_TYPE,
                                                        SYSTEM_ALARM_CALLBACK_ACE_TYPE,
                                                        SYSTEM_MANDATORY_LABEL_ACE_TYPE,
                                                        SYSTEM_SCOPED_POLICY_ID_ACE_TYPE,
                                                        SYSTEM_PROCESS_TRUST_LABEL_ACE_TYPE};
        for (const auto type : sid_ace_types)
        {
            const auto name = "type " + std::to_string(type);
            this->expect_dacl(name, raw_acl({allow, raw_ace(type, everyone_sid)}), STATUS_SUCCESS);
            this->expect_dacl(name + ", SID of revision 2", raw_acl({allow, raw_ace(type, revision_two_sid)}), STATUS_INVALID_ACL);
        }

        this->expect_dacl("AceSize not of whole DWORDs",
                          raw_acl({allow, raw_ace(ACCESS_ALLOWED_ACE_TYPE, everyone_sid, uint16_t{18})}, {.slack = 2}), STATUS_INVALID_ACL);
        this->expect_dacl("AceSize 12", raw_acl({allow, raw_ace(ACCESS_ALLOWED_ACE_TYPE, std::span{everyone_sid}.first(4))}),
                          STATUS_INVALID_ACL);
        this->expect_dacl("S-1-5 in 16 bytes", raw_acl({allow, raw_ace(ACCESS_ALLOWED_ACE_TYPE, nt_authority_sid)}), STATUS_SUCCESS);
        this->expect_dacl("16 sub-authorities", raw_acl({allow, raw_ace(SYSTEM_MANDATORY_LABEL_ACE_TYPE, sixteen_sub_authorities)}),
                          STATUS_INVALID_ACL);
        this->expect_dacl("AceSize short of the SID",
                          raw_acl({allow, raw_ace(SYSTEM_MANDATORY_LABEL_ACE_TYPE, everyone_sid, uint16_t{16})}, {.slack = 4}),
                          STATUS_INVALID_ACL);

        // Compound ACEs: the client's SID, then the server's, which RtlpValidCompoundAce wants room for.
        const auto compound_ace = [](const std::span<const uint8_t> server, const uint16_t compound_type = compound_ace_impersonation) {
            auto body = bytes_of(compound_type);
            append_value(body, uint16_t{0});
            append_bytes(body, everyone_sid);
            append_bytes(body, server);
            return raw_ace(ACCESS_ALLOWED_COMPOUND_ACE_TYPE, body);
        };
        auto cut_short_sid = sid_bytes{local_system_sid.begin(), local_system_sid.end()};
        cut_short_sid[offsetof(SID, SubAuthorityCount)] = 2;
        const auto roomy_nt_authority_sid = joined({nt_authority_sid, std::array<uint8_t, sizeof(DWORD)>{}});
        constexpr acl_shape revision_three{.revision = ACL_REVISION3};
        this->expect_dacl("compound, revision 2", raw_acl({allow, compound_ace(local_system_sid)}), STATUS_INVALID_ACL);
        this->expect_dacl("compound, revision 3", raw_acl({allow, compound_ace(local_system_sid)}, revision_three), STATUS_SUCCESS);
        this->expect_dacl("compound, CompoundAceType 2", raw_acl({allow, compound_ace(local_system_sid, 2)}, revision_three),
                          STATUS_INVALID_ACL);
        this->expect_dacl("compound, server of revision 2", raw_acl({allow, compound_ace(revision_two_sid)}, revision_three),
                          STATUS_INVALID_ACL);
        this->expect_dacl("compound, server cut short", raw_acl({allow, compound_ace(cut_short_sid)}, revision_three), STATUS_INVALID_ACL);
        this->expect_dacl("compound, S-1-5 server with room", raw_acl({allow, compound_ace(roomy_nt_authority_sid)}, revision_three),
                          STATUS_SUCCESS);

        // Object ACEs: the SID after the GUIDs the flags announce, with room for a sub-authority.
        const auto object_ace = [](const std::span<const uint8_t> sid, const DWORD flags = 0, const size_t guid_bytes = 0) {
            auto body = bytes_of(flags);
            body.resize(body.size() + guid_bytes);
            append_bytes(body, sid);
            return raw_ace(ACCESS_ALLOWED_OBJECT_ACE_TYPE, body);
        };
        constexpr acl_shape revision_four{.revision = ACL_REVISION_DS};
        constexpr DWORD both_object_types = ACE_OBJECT_TYPE_PRESENT | ACE_INHERITED_OBJECT_TYPE_PRESENT;
        this->expect_dacl("object, revision 3", raw_acl({allow, object_ace(everyone_sid)}, revision_three), STATUS_INVALID_ACL);
        this->expect_dacl("object, revision 4", raw_acl({allow, object_ace(everyone_sid)}, revision_four), STATUS_SUCCESS);
        this->expect_dacl("object, S-1-5 in 20 bytes", raw_acl({allow, object_ace(nt_authority_sid)}, revision_four), STATUS_INVALID_ACL);
        this->expect_dacl("object, S-1-5 with room", raw_acl({allow, object_ace(roomy_nt_authority_sid)}, revision_four), STATUS_SUCCESS);
        this->expect_dacl("object, both GUIDs",
                          raw_acl({allow, object_ace(everyone_sid, both_object_types, 2 * sizeof(GUID))}, revision_four), STATUS_SUCCESS);
        this->expect_dacl("object, inherited object type cut short",
                          raw_acl({allow, object_ace(everyone_sid, ACE_INHERITED_OBJECT_TYPE_PRESENT, sizeof(DWORD))}, revision_four),
                          STATUS_INVALID_ACL);
        this->expect_dacl("object, AceSize 8",
                          raw_acl({allow, raw_ace_header(ACCESS_ALLOWED_OBJECT_ACE_TYPE, 8, 8)}, {.revision = ACL_REVISION_DS, .slack = 4}),
                          STATUS_INVALID_ACL);
    }

    // RtlpValidAttributeAce wants a CLAIM_SECURITY_ATTRIBUTE_RELATIVE_V1 of 20 bytes at least after the SID, which
    // RtlpValidRelativeAttribute takes with Reserved 0, known flags, values, a terminated name and each value of a type it knows
    // inside it; RtlpValidAccessFilterAce wants 6 bytes at least after the SID that start with "artx" (19041's NtAccessCheck on
    // the host).
    TEST_P(ElevatedTokenTest, AttributeAndAccessFilterAcesHoldWhatTheirTypesSay)
    {
        const auto allow = raw_ace(ACCESS_ALLOWED_ACE_TYPE, interactive_sid);
        const auto with_attribute = [&](const byte_buffer& attribute) { return raw_acl({allow, attribute_ace(attribute)}); };
        const auto int64_value = bytes_of(int64_t{7});
        const byte_buffer terminated_string{'x', 0, 0, 0};
        const byte_buffer unterminated_string{'x', 0, 'y', 0};
        constexpr DWORD custom_flag = 0x00100000; // one of CLAIM_SECURITY_ATTRIBUTE_CUSTOM_FLAGS
        constexpr DWORD unknown_flag = CLAIM_SECURITY_ATTRIBUTE_VALID_FLAGS + 1;
        constexpr WORD unknown_value_type = 7;

        this->expect_dacl("INT64", with_attribute(relative_attribute(CLAIM_SECURITY_ATTRIBUTE_TYPE_INT64, {int64_value})), STATUS_SUCCESS);
        this->expect_dacl("Reserved 1",
                          with_attribute(relative_attribute(CLAIM_SECURITY_ATTRIBUTE_TYPE_INT64, {int64_value}, {.reserved = 1})),
                          STATUS_INVALID_ACL);
        this->expect_dacl("an unknown flag",
                          with_attribute(relative_attribute(CLAIM_SECURITY_ATTRIBUTE_TYPE_INT64, {int64_value}, {.flags = unknown_flag})),
                          STATUS_INVALID_ACL);
        this->expect_dacl("valid and custom flags",
                          with_attribute(relative_attribute(CLAIM_SECURITY_ATTRIBUTE_TYPE_INT64, {int64_value},
                                                            {.flags = CLAIM_SECURITY_ATTRIBUTE_VALID_FLAGS | custom_flag})),
                          STATUS_SUCCESS);
        this->expect_dacl("no values",
                          with_attribute(relative_attribute(CLAIM_SECURITY_ATTRIBUTE_TYPE_INT64, {int64_value}, {.value_count = DWORD{0}})),
                          STATUS_INVALID_ACL);
        this->expect_dacl(
            "a name without its NUL",
            with_attribute(relative_attribute(CLAIM_SECURITY_ATTRIBUTE_TYPE_INT64, {int64_value}, {.name_last_without_nul = true})),
            STATUS_INVALID_ACL);
        this->expect_dacl("type INVALID", with_attribute(relative_attribute(CLAIM_SECURITY_ATTRIBUTE_TYPE_INVALID, {int64_value})),
                          STATUS_INVALID_ACL);
        this->expect_dacl("UINT64", with_attribute(relative_attribute(CLAIM_SECURITY_ATTRIBUTE_TYPE_UINT64, {int64_value})),
                          STATUS_SUCCESS);
        this->expect_dacl("FQBN", with_attribute(relative_attribute(CLAIM_SECURITY_ATTRIBUTE_TYPE_FQBN, {int64_value})),
                          STATUS_INVALID_ACL);
        this->expect_dacl("an unknown type", with_attribute(relative_attribute(unknown_value_type, {int64_value})), STATUS_INVALID_ACL);
        this->expect_dacl("STRING", with_attribute(relative_attribute(CLAIM_SECURITY_ATTRIBUTE_TYPE_STRING, {terminated_string})),
                          STATUS_SUCCESS);
        this->expect_dacl("STRING without its NUL",
                          with_attribute(relative_attribute(CLAIM_SECURITY_ATTRIBUTE_TYPE_STRING, {unterminated_string})),
                          STATUS_INVALID_ACL);
        this->expect_dacl("BOOLEAN 1", with_attribute(relative_attribute(CLAIM_SECURITY_ATTRIBUTE_TYPE_BOOLEAN, {bytes_of(uint64_t{1})})),
                          STATUS_SUCCESS);
        this->expect_dacl("BOOLEAN 2", with_attribute(relative_attribute(CLAIM_SECURITY_ATTRIBUTE_TYPE_BOOLEAN, {bytes_of(uint64_t{2})})),
                          STATUS_INVALID_ACL);
        this->expect_dacl(
            "SID", with_attribute(relative_attribute(CLAIM_SECURITY_ATTRIBUTE_TYPE_SID, {joined({bytes_of(DWORD{12}), everyone_sid})})),
            STATUS_SUCCESS);
        this->expect_dacl("SID of length 0", with_attribute(relative_attribute(CLAIM_SECURITY_ATTRIBUTE_TYPE_SID, {bytes_of(DWORD{0})})),
                          STATUS_INVALID_ACL);
        this->expect_dacl("OCTET_STRING of length 0",
                          with_attribute(relative_attribute(CLAIM_SECURITY_ATTRIBUTE_TYPE_OCTET_STRING, {bytes_of(DWORD{0})})),
                          STATUS_SUCCESS);
        this->expect_dacl("OCTET_STRING past the ACE",
                          with_attribute(relative_attribute(CLAIM_SECURITY_ATTRIBUTE_TYPE_OCTET_STRING, {bytes_of(DWORD{64})})),
                          STATUS_INVALID_ACL);
        this->expect_dacl("16 bytes after the SID",
                          raw_acl({allow, raw_ace(SYSTEM_RESOURCE_ATTRIBUTE_ACE_TYPE, joined({everyone_sid, std::array<uint8_t, 16>{}}))}),
                          STATUS_INVALID_ACL);

        constexpr std::array<uint8_t, 4> condition_signature{'a', 'r', 't', 'x'};
        constexpr std::array<uint8_t, 4> reversed_signature{'x', 't', 'r', 'a'};
        constexpr std::array<uint8_t, 4> zeros{};
        const auto filter_ace = [](const byte_buffer& condition) {
            return raw_ace(SYSTEM_ACCESS_FILTER_ACE_TYPE, joined({everyone_sid, condition}));
        };
        this->expect_dacl("filter of 8 bytes", raw_acl({allow, filter_ace(joined({condition_signature, zeros}))}), STATUS_SUCCESS);
        this->expect_dacl("filter of the signature alone", raw_acl({allow, filter_ace(joined({condition_signature}))}), STATUS_INVALID_ACL);
        this->expect_dacl("filter without the signature", raw_acl({allow, filter_ace(joined({reversed_signature, zeros}))}),
                          STATUS_INVALID_ACL);
    }

    // SeAccessCheckByType reads the privilege set's length, then SeAccessCheckByTypeWithAdminlessChecks probes AccessStatus,
    // GrantedAccess and PrivilegeSetLength for writing and the privilege set for its length (ProbeForWrite: nothing for a
    // length of 0, else alignment first), and copies the GenericMapping, which must be DWORD-aligned, before it refuses
    // generic rights or looks at the token (19041's NtAccessCheck on the host).
    TEST_P(ElevatedTokenTest, ArgumentsAreProbedBeforeGenericRightsAreRefused)
    {
        const auto mapping = this->storage + 0x800;
        const auto privilege_set = this->storage + 0x880;
        const handle no_token = make_handle(0x7FFF1234);
        EXPECT_EQ(STATUS_GENERIC_NOT_MAPPED, this->call_arguments({}, generic_read));
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->call_arguments({.mapping = uint64_t{0}}, generic_read));
        EXPECT_EQ(STATUS_DATATYPE_MISALIGNMENT, this->call_arguments({.mapping = mapping + 2}, generic_read));
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->call_arguments({.granted = uint64_t{0}}, generic_read));
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->call_arguments({.status = uint64_t{0}}, generic_read));
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->call_arguments({.privilege_set_length = uint64_t{0}}, generic_read));
        EXPECT_EQ(STATUS_DATATYPE_MISALIGNMENT, this->call_arguments({.privilege_set = privilege_set + 2}, generic_read));
        EXPECT_EQ(STATUS_GENERIC_NOT_MAPPED,
                  this->call_arguments({.privilege_set = privilege_set + 2}, generic_read, CURRENT_PROCESS_TOKEN, 0));
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->call_arguments({.privilege_set = uint64_t{0}}, generic_read));
        EXPECT_EQ(STATUS_GENERIC_NOT_MAPPED, this->call_arguments({.privilege_set = uint64_t{0}}, generic_read, CURRENT_PROCESS_TOKEN, 0));
        EXPECT_EQ(STATUS_DATATYPE_MISALIGNMENT,
                  this->call_arguments({.mapping = uint64_t{0}, .privilege_set = privilege_set + 2}, generic_read));
        EXPECT_EQ(STATUS_ACCESS_VIOLATION,
                  this->call_arguments({.privilege_set = privilege_set + 2, .granted = uint64_t{0}}, generic_read));
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->call_arguments({.mapping = uint64_t{0}}, file_read_data, no_token));
        EXPECT_EQ(STATUS_INVALID_HANDLE, this->call_arguments({}, file_read_data, no_token));
    }

    // SeAccessCheckByTypeWithAdminlessChecks writes each probed DWORD back and ProbeForWrite a byte of each page, and the
    // GenericMapping and the descriptor are copied, so an output on a page the guest may not write and a mapping or a
    // descriptor on one it may not read fault, while a read-only mapping is copied (19041's NtAccessCheck on the host).
    TEST_P(ElevatedTokenTest, ProbesAndCopiesFaultWhereThePageProtectionForbidsThem)
    {
        const auto read_only = this->emu->memory.allocate_memory(0x1000, memory_permission::read);
        const auto no_access = this->emu->memory.allocate_memory(0x1000, memory_permission::read_write);
        ASSERT_TRUE(this->emu->memory.protect_memory(no_access, 0x1000, memory_permission::none));
        emulator_object<EMU_GENERIC_MAPPING>{this->emu->memory, read_only}.write(file_generic_mapping);
        const auto read_only_output = read_only + 0x100;

        EXPECT_EQ(STATUS_GENERIC_NOT_MAPPED, this->call_arguments({.mapping = read_only}, generic_read));
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->call_arguments({.mapping = no_access}, generic_read));
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->call_arguments({.descriptor = no_access}, file_read_data));
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->call_arguments({.granted = read_only_output}, generic_read));
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->call_arguments({.status = read_only_output}, generic_read));
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->call_arguments({.privilege_set_length = read_only_output}, generic_read));
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->call_arguments({.privilege_set_length = no_access}, generic_read));
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->call_arguments({.privilege_set = read_only_output}, generic_read));
        EXPECT_EQ(STATUS_GENERIC_NOT_MAPPED,
                  this->call_arguments({.privilege_set = read_only_output}, generic_read, CURRENT_PROCESS_TOKEN, 0));
    }

    // A guard page under a probe or a copy loses its guard and ends the call with STATUS_GUARD_PAGE_VIOLATION, as the kernel's
    // own access to it does; the next call finds an ordinary page (19041 on the host).
    TEST_P(ElevatedTokenTest, ProbesAndCopiesTakeTheGuardOfAGuardPage)
    {
        const auto guarded = this->emu->memory.allocate_memory(0x1000, memory_permission::read_write);
        const auto guard = [&] {
            return this->emu->memory.protect_memory(guarded, 0x1000,
                                                    nt_memory_permission(memory_permission::read_write, memory_permission_ext::guard));
        };

        ASSERT_TRUE(guard());
        EXPECT_EQ(STATUS_GUARD_PAGE_VIOLATION, this->call_arguments({.granted = guarded}, generic_read));
        EXPECT_EQ(STATUS_GENERIC_NOT_MAPPED, this->call_arguments({.granted = guarded}, generic_read));
        ASSERT_TRUE(guard());
        EXPECT_EQ(STATUS_GUARD_PAGE_VIOLATION, this->call_arguments({.mapping = guarded}, generic_read));
        EXPECT_FALSE(this->emu->memory.is_guard_page(guarded));
    }

    INSTANTIATE_TEST_SUITE_P(Elevation, ElevatedTokenTest, testing::Values(false, true),
                             [](const testing::TestParamInfo<bool>& info) { return info.param ? "Elevated" : "Default"; });
}
