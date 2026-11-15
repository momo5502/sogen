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

            constexpr ULONG se_privilege_enabled = 0x2;
            constexpr ULONG se_privilege_removed = 0x4;
            constexpr ULONG disable_max_privilege = 0x1;
            constexpr uint32_t se_change_notify_privilege = 23;

            struct luid_and_attributes
            {
                ULONG luid_low{};
                LONG luid_high{};
                ULONG attributes{};
            };

            static_assert(sizeof(luid_and_attributes) == 12);

            bool is_modeled_token(const handle token)
            {
                return token == CURRENT_PROCESS_TOKEN || token == DUMMY_IMPERSONATION_TOKEN || token == ANONYMOUS_IMPERSONATION_TOKEN;
            }

            // Impersonation tokens are modeled as the process impersonating itself, so they share its privileges.
            std::vector<token_privilege>* find_token_privileges(const syscall_context& c, const handle requested_token)
            {
                if (auto* opened = c.proc.tokens.get(resolve_token_object(c, requested_token)); opened && opened->privileges)
                {
                    return &*opened->privileges;
                }

                const auto token = resolve_token(c, requested_token);
                if (token == CURRENT_PROCESS_TOKEN || token == DUMMY_IMPERSONATION_TOKEN)
                {
                    return &c.proc.token_privileges;
                }

                return nullptr;
            }

            std::vector<token_privilege> copy_token_privileges(const syscall_context& c, const handle requested_token)
            {
                const auto* privileges = find_token_privileges(c, requested_token);
                return privileges ? *privileges : std::vector<token_privilege>{};
            }

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

            std::vector<token_group> get_token_groups(const windows_emulator& win_emu, const std::span<const uint8_t> user_sid,
                                                      const bool anonymous)
            {
                if (anonymous)
                {
                    return {};
                }

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
            size_t count_token_groups(const windows_emulator& win_emu, const std::span<const uint8_t> user_sid, const bool anonymous)
            {
                if (anonymous)
                {
                    return 0;
                }

                size_t count = is_account_domain_user(user_sid) ? 1 : 0;
                visit_standard_token_groups(win_emu.runs_elevated(), [&](const std::span<const uint8_t>, const ULONG) { ++count; });
                return count;
            }

            NTSTATUS write_token_groups(const syscall_context& c, const uint64_t token_information, const ULONG token_information_length,
                                        const emulator_object<ULONG> return_length, const std::span<const token_group> groups)
            {
                const auto entries_size = offsetof(TOKEN_GROUPS64, Groups) + groups.size() * sizeof(SID_AND_ATTRIBUTES64);
                auto required_size = entries_size;
                for (const auto& group : groups)
                {
                    required_size += group.sid.size();
                }

                return_length.write(static_cast<ULONG>(required_size));
                if (required_size > token_information_length)
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                c.emu.write_memory(token_information, static_cast<ULONG>(groups.size()));
                auto sid_address = token_information + entries_size;
                for (size_t index = 0; index < groups.size(); ++index)
                {
                    SID_AND_ATTRIBUTES64 entry{};
                    entry.Sid = sid_address;
                    entry.Attributes = groups[index].attributes;
                    c.emu.write_memory(token_information + offsetof(TOKEN_GROUPS64, Groups) + index * sizeof(SID_AND_ATTRIBUTES64), entry);
                    c.emu.write_memory(sid_address, groups[index].sid.data(), groups[index].sid.size());
                    sid_address += groups[index].sid.size();
                }

                return STATUS_SUCCESS;
            }

            // SeCaptureLuidAndAttributesArray fails a larger count with STATUS_INVALID_PARAMETER before it allocates.
            constexpr ULONG max_privilege_count = 66;

            std::optional<std::vector<luid_and_attributes>> read_privilege_list(const syscall_context& c, const uint64_t address)
            {
                const auto count = c.emu.read_memory<ULONG>(address);
                if (count > max_privilege_count)
                {
                    return std::nullopt;
                }

                std::vector<luid_and_attributes> entries(count);
                if (count != 0)
                {
                    c.emu.read_memory(address + sizeof(ULONG), entries.data(), entries.size() * sizeof(luid_and_attributes));
                }
                return entries;
            }

            void write_privilege_list(const syscall_context& c, const uint64_t address, const std::span<const luid_and_attributes> entries)
            {
                c.emu.write_memory(address, static_cast<ULONG>(entries.size()));
                if (!entries.empty())
                {
                    c.emu.write_memory(address + sizeof(ULONG), entries.data(), entries.size_bytes());
                }
            }

            constexpr ACCESS_MASK generic_rights = 0xF0000000; // GENERIC_READ | GENERIC_WRITE | GENERIC_EXECUTE | GENERIC_ALL
            constexpr ACCESS_MASK maximum_allowed = 0x02000000;
            constexpr ACCESS_MASK access_system_security = 0x01000000;
            constexpr ACCESS_MASK owner_implicit_rights = 0x00060000; // READ_CONTROL | WRITE_DAC

            // A PRIVILEGE_SET with room for one LUID_AND_ATTRIBUTES, the least NtAccessCheck takes.
            constexpr ULONG minimum_privilege_set_size = 20;

            constexpr std::array<uint8_t, 12> owner_rights_sid{1, 1, 0, 0, 0, 0, 0, 3, 4, 0, 0, 0}; // S-1-3-4

            // MM_USER_PROBE_ADDRESS: a probe on behalf of user mode faults on a range that reaches it.
            constexpr uint64_t user_probe_address = 0x7FFFFFFF0000;

            constexpr size_t sid_header_size = offsetof(SID, SubAuthority);

            // Where most ACE types keep their SID: right after the mask.
            constexpr size_t ace_sid_offset = offsetof(ACCESS_ALLOWED_ACE, SidStart);

            // COMPOUND_ACCESS_ALLOWED_ACE: its CompoundAceType and Reserved come before the client's SID, which the server's
            // follows.
            constexpr size_t compound_ace_sid_offset = ace_sid_offset + (2 * sizeof(uint16_t));
            constexpr uint16_t compound_ace_impersonation = 1; // COMPOUND_ACE_IMPERSONATION, the only CompoundAceType

            // RtlpValidCompoundAce and RtlpValidObjectAce want room for a SID with a sub-authority, whatever the SID says.
            constexpr size_t smallest_sid_room = sid_header_size + sizeof(DWORD);

            // CLAIM_SECURITY_ATTRIBUTE_RELATIVE_V1 up to its value offsets, which follow it.
            struct relative_attribute_header
            {
                DWORD name;
                WORD value_type;
                WORD reserved;
                DWORD flags;
                DWORD value_count;
            };

            // An attribute with one value offset, the least RtlpValidAttributeAce and RtlpValidRelativeAttribute take, and
            // room for a name of one character and its NUL.
            constexpr size_t smallest_relative_attribute = sizeof(relative_attribute_header) + sizeof(DWORD);
            constexpr size_t smallest_attribute_name = 2 * sizeof(char16_t);

            // A conditional expression starts with "artx"; RtlpValidAccessFilterAce wants 6 bytes of it at least.
            constexpr uint32_t conditional_expression_signature = 0x78747261;
            constexpr size_t smallest_access_filter_condition = 6;

            // A value of a copied buffer at an offset the caller has checked.
            template <typename T>
            T read_at(const std::span<const uint8_t> data, const size_t offset)
            {
                T value{};
                memcpy(&value, data.subspan(offset, sizeof(value)).data(), sizeof(value));
                return value;
            }

            // The SID at an offset of a copied buffer once RtlValidAcl or SeCaptureSecurityDescriptor has taken it.
            std::span<const uint8_t> sid_at(const std::span<const uint8_t> data, const size_t offset)
            {
                return data.subspan(offset, sid_header_size + (data[offset + offsetof(SID, SubAuthorityCount)] * sizeof(DWORD)));
            }

            // The length of an ACE's SID at an offset when RtlValidAcl takes it, revision 1 with at most 15 sub-authorities, or
            // none. The caller has checked that its revision and count lie inside the ACE.
            std::optional<size_t> valid_sid_length(const std::span<const uint8_t> ace, const size_t offset)
            {
                const auto sub_authority_count = ace[offset + offsetof(SID, SubAuthorityCount)];
                if (ace[offset + offsetof(SID, Revision)] != SID_REVISION || sub_authority_count > SID_MAX_SUB_AUTHORITIES)
                {
                    return std::nullopt;
                }

                return sid_header_size + (sub_authority_count * sizeof(DWORD));
            }

            // Where the SID after an ACE's mask ends, for the types RtlValidAcl and its helpers check that way: an ACE of whole
            // DWORDs that holds the SID. None when it does not.
            std::optional<size_t> mask_sid_end(const std::span<const uint8_t> ace)
            {
                if (ace.size() % sizeof(DWORD) != 0 || ace.size() < ace_sid_offset + sid_header_size)
                {
                    return std::nullopt;
                }

                const auto sid_length = valid_sid_length(ace, ace_sid_offset);
                if (!sid_length || ace.size() < ace_sid_offset + *sid_length)
                {
                    return std::nullopt;
                }

                return ace_sid_offset + *sid_length;
            }

            // RtlpValidCompoundAce: an impersonation ACE of whole DWORDs with the client's SID, then the server's.
            bool is_valid_compound_ace(const std::span<const uint8_t> ace)
            {
                if (ace.size() % sizeof(DWORD) != 0 || ace.size() < compound_ace_sid_offset + smallest_sid_room ||
                    read_at<uint16_t>(ace, ace_sid_offset) != compound_ace_impersonation)
                {
                    return false;
                }

                const auto client = valid_sid_length(ace, compound_ace_sid_offset);
                if (!client || ace.size() < compound_ace_sid_offset + *client + smallest_sid_room)
                {
                    return false;
                }

                const auto server = valid_sid_length(ace, compound_ace_sid_offset + *client);
                return server && ace.size() >= compound_ace_sid_offset + *client + *server;
            }

            // Where an object ACE keeps its SID: after its flags and the object type GUIDs they announce. The caller has checked
            // that the flags lie inside the ACE.
            size_t object_ace_sid_offset(const std::span<const uint8_t> ace)
            {
                const auto flags = read_at<DWORD>(ace, offsetof(ACCESS_ALLOWED_OBJECT_ACE, Flags));
                auto offset = offsetof(ACCESS_ALLOWED_OBJECT_ACE, ObjectType);
                offset += (flags & ACE_OBJECT_TYPE_PRESENT) != 0 ? sizeof(GUID) : 0;
                offset += (flags & ACE_INHERITED_OBJECT_TYPE_PRESENT) != 0 ? sizeof(GUID) : 0;
                return offset;
            }

            // RtlpValidObjectAce: an ACE of whole DWORDs with its SID after the GUIDs.
            bool is_valid_object_ace(const std::span<const uint8_t> ace)
            {
                if (ace.size() % sizeof(DWORD) != 0 || ace.size() < offsetof(ACCESS_ALLOWED_OBJECT_ACE, ObjectType))
                {
                    return false;
                }

                const auto sid_offset = object_ace_sid_offset(ace);
                if (ace.size() < sid_offset + smallest_sid_room)
                {
                    return false;
                }

                const auto sid_length = valid_sid_length(ace, sid_offset);
                return sid_length && ace.size() >= sid_offset + *sid_length;
            }

            // RtlStringCbLengthW: whether a NUL ends a UTF-16 string inside the buffer.
            bool holds_terminated_string(const std::span<const uint8_t> buffer)
            {
                for (size_t offset = 0; offset + sizeof(char16_t) <= buffer.size(); offset += sizeof(char16_t))
                {
                    if (read_at<char16_t>(buffer, offset) == 0)
                    {
                        return true;
                    }
                }

                return false;
            }

            // Whether RtlpValidRelativeAttribute takes the value at an offset of an attribute of a value type.
            bool is_valid_attribute_value(const std::span<const uint8_t> attribute, const WORD value_type, const DWORD offset)
            {
                if (offset > attribute.size())
                {
                    return false;
                }

                const auto room = attribute.size() - offset;
                switch (value_type)
                {
                case CLAIM_SECURITY_ATTRIBUTE_TYPE_INT64:
                case CLAIM_SECURITY_ATTRIBUTE_TYPE_UINT64:
                    return room >= sizeof(uint64_t);
                case CLAIM_SECURITY_ATTRIBUTE_TYPE_STRING:
                    return room >= sizeof(char16_t) && holds_terminated_string(attribute.subspan(offset));
                case CLAIM_SECURITY_ATTRIBUTE_TYPE_BOOLEAN:
                    return room >= sizeof(uint64_t) && read_at<uint64_t>(attribute, offset) <= 1;
                case CLAIM_SECURITY_ATTRIBUTE_TYPE_SID:
                case CLAIM_SECURITY_ATTRIBUTE_TYPE_OCTET_STRING: {
                    // CLAIM_SECURITY_ATTRIBUTE_OCTET_STRING_RELATIVE: a length and that many bytes, which a SID must have.
                    if (room < sizeof(DWORD))
                    {
                        return false;
                    }

                    const auto length = read_at<DWORD>(attribute, offset);
                    return room - sizeof(DWORD) >= length && (length != 0 || value_type == CLAIM_SECURITY_ATTRIBUTE_TYPE_OCTET_STRING);
                }
                default: // CLAIM_SECURITY_ATTRIBUTE_TYPE_FQBN among them
                    return false;
                }
            }

            // RtlpValidRelativeAttribute: a CLAIM_SECURITY_ATTRIBUTE_RELATIVE_V1 inside the buffer, with known flags, a
            // terminated name and one value at least, of a type it knows, each inside the buffer.
            bool is_valid_relative_attribute(const std::span<const uint8_t> attribute)
            {
                if (attribute.size() < smallest_relative_attribute)
                {
                    return false;
                }

                constexpr DWORD known_flags = CLAIM_SECURITY_ATTRIBUTE_VALID_FLAGS | CLAIM_SECURITY_ATTRIBUTE_CUSTOM_FLAGS;
                const auto header = read_at<relative_attribute_header>(attribute, 0);
                if (header.reserved != 0 || (header.flags & ~known_flags) != 0 || header.value_count == 0 ||
                    header.name > attribute.size() || attribute.size() - header.name < smallest_attribute_name ||
                    !holds_terminated_string(attribute.subspan(header.name)) ||
                    attribute.size() - sizeof(header) < uint64_t{header.value_count} * sizeof(DWORD) ||
                    header.value_type == CLAIM_SECURITY_ATTRIBUTE_TYPE_INVALID)
                {
                    return false;
                }

                for (DWORD index = 0; index < header.value_count; ++index)
                {
                    const auto offset = read_at<DWORD>(attribute, sizeof(header) + (index * sizeof(DWORD)));
                    if (!is_valid_attribute_value(attribute, header.value_type, offset))
                    {
                        return false;
                    }
                }

                return true;
            }

            // RtlValidAcl's check of an ACE by its type, in an ACL of a revision. 19041 checks no other type further.
            bool is_valid_ace(const std::span<const uint8_t> ace, const uint8_t type, const uint8_t acl_revision)
            {
                switch (type)
                {
                case ACCESS_ALLOWED_ACE_TYPE:
                case ACCESS_DENIED_ACE_TYPE:
                case SYSTEM_AUDIT_ACE_TYPE:
                case SYSTEM_ALARM_ACE_TYPE:
                case ACCESS_ALLOWED_CALLBACK_ACE_TYPE:
                case ACCESS_DENIED_CALLBACK_ACE_TYPE:
                case SYSTEM_AUDIT_CALLBACK_ACE_TYPE:
                case SYSTEM_ALARM_CALLBACK_ACE_TYPE:
                case SYSTEM_MANDATORY_LABEL_ACE_TYPE:
                case SYSTEM_SCOPED_POLICY_ID_ACE_TYPE:
                case SYSTEM_PROCESS_TRUST_LABEL_ACE_TYPE:
                    return mask_sid_end(ace).has_value();
                case ACCESS_ALLOWED_COMPOUND_ACE_TYPE:
                    return acl_revision >= ACL_REVISION3 && is_valid_compound_ace(ace);
                case ACCESS_ALLOWED_OBJECT_ACE_TYPE:
                case ACCESS_DENIED_OBJECT_ACE_TYPE:
                case SYSTEM_AUDIT_OBJECT_ACE_TYPE:
                case SYSTEM_ALARM_OBJECT_ACE_TYPE:
                case ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE:
                case ACCESS_DENIED_CALLBACK_OBJECT_ACE_TYPE:
                case SYSTEM_AUDIT_CALLBACK_OBJECT_ACE_TYPE:
                case SYSTEM_ALARM_CALLBACK_OBJECT_ACE_TYPE:
                    return acl_revision >= ACL_REVISION_DS && is_valid_object_ace(ace);
                case SYSTEM_RESOURCE_ATTRIBUTE_ACE_TYPE: {
                    // RtlpValidAttributeAce: the SID, then the attribute.
                    const auto attribute = mask_sid_end(ace);
                    return attribute && ace.size() >= *attribute + smallest_relative_attribute &&
                           is_valid_relative_attribute(ace.subspan(*attribute));
                }
                case SYSTEM_ACCESS_FILTER_ACE_TYPE: {
                    // RtlpValidAccessFilterAce: the SID, then the conditional expression.
                    const auto condition = mask_sid_end(ace);
                    return condition && ace.size() >= *condition + smallest_access_filter_condition &&
                           read_at<uint32_t>(ace, *condition) == conditional_expression_signature;
                }
                default:
                    return ace.size() >= sizeof(ACE_HEADER);
                }
            }

            // RtlValidAcl, on the copy SeCaptureSecurityDescriptor makes of an ACL: AclSize bytes in an aligned buffer, so an
            // ACE is aligned where its offset is even. Each of the ACEs AceCount names starts at an even offset with a header
            // that ends before the ACL does, ends inside the ACL and passes its type's check.
            bool is_valid_acl(const std::span<const uint8_t> acl)
            {
                const auto header = read_at<ACL>(acl, 0);
                if (header.AclRevision < MIN_ACL_REVISION || header.AclRevision > MAX_ACL_REVISION || header.AclSize < sizeof(ACL))
                {
                    return false;
                }

                auto offset = sizeof(ACL);
                for (USHORT index = 0; index < header.AceCount; ++index)
                {
                    if (offset + sizeof(ACE_HEADER) >= acl.size() || offset % sizeof(WORD) != 0)
                    {
                        return false;
                    }

                    const auto ace = read_at<ACE_HEADER>(acl, offset);
                    if (ace.AceSize > acl.size() - offset ||
                        !is_valid_ace(acl.subspan(offset, ace.AceSize), ace.AceType, header.AclRevision))
                    {
                        return false;
                    }

                    offset += ace.AceSize;
                }

                return true;
            }

            // RtlpOwnerAcesPresent: whether an ACE RtlValidAcl has taken names OWNER RIGHTS where its type keeps the SID, right
            // after the mask, after a compound ACE's type, or after an object ACE's flags and GUIDs. It reads no SID from ACEs
            // of other types.
            bool names_owner_rights(const std::span<const uint8_t> ace, const uint8_t type)
            {
                auto sid_offset = ace_sid_offset;
                switch (type)
                {
                case ACCESS_ALLOWED_ACE_TYPE:
                case ACCESS_DENIED_ACE_TYPE:
                case SYSTEM_AUDIT_ACE_TYPE:
                case SYSTEM_ALARM_ACE_TYPE:
                case ACCESS_ALLOWED_CALLBACK_ACE_TYPE:
                case ACCESS_DENIED_CALLBACK_ACE_TYPE:
                case SYSTEM_AUDIT_CALLBACK_ACE_TYPE:
                case SYSTEM_ALARM_CALLBACK_ACE_TYPE:
                    break;
                case ACCESS_ALLOWED_COMPOUND_ACE_TYPE:
                    sid_offset = compound_ace_sid_offset;
                    break;
                case ACCESS_ALLOWED_OBJECT_ACE_TYPE:
                case ACCESS_DENIED_OBJECT_ACE_TYPE:
                case SYSTEM_AUDIT_OBJECT_ACE_TYPE:
                case SYSTEM_ALARM_OBJECT_ACE_TYPE:
                case ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE:
                case ACCESS_DENIED_CALLBACK_OBJECT_ACE_TYPE:
                case SYSTEM_AUDIT_CALLBACK_OBJECT_ACE_TYPE:
                case SYSTEM_ALARM_CALLBACK_OBJECT_ACE_TYPE:
                    sid_offset = object_ace_sid_offset(ace);
                    break;
                default:
                    return false;
                }

                return std::ranges::equal(sid_at(ace, sid_offset), owner_rights_sid);
            }

            // SECURITY_DESCRIPTOR as a 64-bit process lays out its absolute form.
            struct security_descriptor64
            {
                BYTE Revision;
                BYTE Sbz1;
                SECURITY_DESCRIPTOR_CONTROL Control;
                uint64_t Owner;
                uint64_t Group;
                uint64_t Sacl;
                uint64_t Dacl;
            };

            // A read as a probe on behalf of user mode makes it: a range that reaches MM_USER_PROBE_ADDRESS, or memory the
            // guest may not read, faults.
            bool read_user_memory(const syscall_context& c, const uint64_t address, void* data, const size_t size)
            {
                return address <= user_probe_address && size <= user_probe_address - address &&
                       is_readable_range(c.win_emu.memory, address, size) && c.emu.try_read_memory(address, data, size);
            }

            // A DWORD argument probed for writing as SeAccessCheckByTypeWithAdminlessChecks probes one, which reads it and
            // writes it back: a range that reaches MM_USER_PROBE_ADDRESS, or memory the guest may not write, faults.
            bool probe_dword_for_write(const syscall_context& c, const uint64_t address)
            {
                return address <= user_probe_address - sizeof(DWORD) && is_writable_range(c.win_emu.memory, address, sizeof(DWORD));
            }

            // ProbeForWrite: nothing for a length of 0, else the alignment, then a range below MM_USER_PROBE_ADDRESS, then a byte
            // of each of its pages written back, which faults on memory the guest may not write.
            NTSTATUS probe_for_write(const syscall_context& c, const uint64_t address, const uint64_t length, const uint64_t alignment)
            {
                if (length == 0)
                {
                    return STATUS_SUCCESS;
                }

                if (address % alignment != 0)
                {
                    return STATUS_DATATYPE_MISALIGNMENT;
                }

                const auto last = address + length - 1;
                if (last < address || last >= user_probe_address || !is_writable_range(c.win_emu.memory, address, length))
                {
                    return STATUS_ACCESS_VIOLATION;
                }

                return STATUS_SUCCESS;
            }

            // SeCaptureSecurityDescriptor's probe of an ACL: its AclSize, then, unless that is 0, the ACL's alignment and the
            // range it spans. The room it takes, AclSize rounded up to DWORDs, is 8 bytes to 64 KB.
            NTSTATUS probe_acl(const syscall_context& c, const uint64_t address, uint16_t& size)
            {
                size = 0;
                if (address == 0)
                {
                    return STATUS_SUCCESS;
                }

                if (!read_user_memory(c, address + offsetof(ACL, AclSize), &size, sizeof(size)))
                {
                    return STATUS_ACCESS_VIOLATION;
                }

                if (size != 0 && address % sizeof(DWORD) != 0)
                {
                    return STATUS_DATATYPE_MISALIGNMENT;
                }

                if (size != 0 && (address > user_probe_address || size > user_probe_address - address))
                {
                    return STATUS_ACCESS_VIOLATION;
                }

                const auto room = (size_t{size} + sizeof(DWORD) - 1) & ~(sizeof(DWORD) - 1);
                return room < sizeof(ACL) || room > std::numeric_limits<uint16_t>::max() ? STATUS_INVALID_ACL : STATUS_SUCCESS;
            }

            // SeCaptureSecurityDescriptor's probe of a SID: its SubAuthorityCount, which sizes it, its alignment and its range.
            NTSTATUS probe_sid(const syscall_context& c, const uint64_t address, uint8_t& sub_authority_count)
            {
                sub_authority_count = 0;
                if (address == 0)
                {
                    return STATUS_SUCCESS;
                }

                if (!read_user_memory(c, address + offsetof(SID, SubAuthorityCount), &sub_authority_count, sizeof(sub_authority_count)))
                {
                    return STATUS_ACCESS_VIOLATION;
                }

                if (address % sizeof(DWORD) != 0)
                {
                    return STATUS_DATATYPE_MISALIGNMENT;
                }

                const auto size = sid_header_size + (sub_authority_count * sizeof(DWORD));
                return address > user_probe_address || size > user_probe_address - address ? STATUS_ACCESS_VIOLATION : STATUS_SUCCESS;
            }

            // SeCaptureSecurityDescriptor's copy of a probed ACL, which RtlValidAcl must take with its AclSize still the probed
            // one. None without an address.
            NTSTATUS copy_acl(const syscall_context& c, const uint64_t address, const uint16_t size,
                              std::optional<std::vector<uint8_t>>& acl)
            {
                if (address == 0)
                {
                    return STATUS_SUCCESS;
                }

                auto& copy = acl.emplace(size);
                if (!read_user_memory(c, address, copy.data(), copy.size()))
                {
                    return STATUS_ACCESS_VIOLATION;
                }

                return size < sizeof(ACL) || read_at<ACL>(copy, 0).AclSize != size || !is_valid_acl(copy) ? STATUS_INVALID_ACL
                                                                                                          : STATUS_SUCCESS;
            }

            // SeCaptureSecurityDescriptor's copy of a probed SID, as long as the probed count says and keeping that count: it
            // takes at most 15 sub-authorities and looks at the revision's low nibble alone. Empty without an address.
            NTSTATUS copy_sid(const syscall_context& c, const uint64_t address, const uint8_t sub_authority_count,
                              std::vector<uint8_t>& sid)
            {
                if (address == 0)
                {
                    return STATUS_SUCCESS;
                }

                constexpr uint8_t revision_nibble = 0xF;
                sid.resize(sid_header_size + (sub_authority_count * sizeof(DWORD)));
                if (!read_user_memory(c, address, sid.data(), sid.size()))
                {
                    return STATUS_ACCESS_VIOLATION;
                }

                sid[offsetof(SID, SubAuthorityCount)] = sub_authority_count;
                return sub_authority_count > SID_MAX_SUB_AUTHORITIES || (sid[offsetof(SID, Revision)] & revision_nibble) != SID_REVISION
                           ? STATUS_INVALID_SID
                           : STATUS_SUCCESS;
            }

            // A descriptor as SeCaptureSecurityDescriptor copies it: an owner or a group left out is empty, and a DACL that
            // SE_DACL_PRESENT leaves out or that has no address is none.
            struct captured_descriptor
            {
                std::vector<uint8_t> owner{};
                std::vector<uint8_t> group{};
                std::optional<std::vector<uint8_t>> dacl{};
            };

            // SeCaptureSecurityDescriptor for a caller in user mode: it checks the descriptor's alignment and revision, probes
            // the SACL and the DACL, then the owner and the group, and copies them in that order, failing an ACL that
            // RtlValidAcl does not take and a SID that is not valid. No descriptor is captured as none.
            NTSTATUS capture_security_descriptor(const syscall_context& c, const uint64_t address,
                                                 std::optional<captured_descriptor>& captured)
            {
                captured.reset();
                if (address == 0)
                {
                    return STATUS_SUCCESS;
                }

                if (address % sizeof(DWORD) != 0)
                {
                    return STATUS_DATATYPE_MISALIGNMENT;
                }

                SECURITY_DESCRIPTOR_RELATIVE header{};
                if (!read_user_memory(c, address, &header, sizeof(header)))
                {
                    return STATUS_ACCESS_VIOLATION;
                }

                const auto self_relative = (header.Control & SE_SELF_RELATIVE) != 0;
                if (!self_relative && address % sizeof(uint64_t) != 0)
                {
                    return STATUS_DATATYPE_MISALIGNMENT;
                }

                if (header.Revision != SECURITY_DESCRIPTOR_REVISION)
                {
                    return STATUS_UNKNOWN_REVISION;
                }

                // Offsets in the self-relative form, pointers in the absolute one; an ACL its flag leaves out is not looked at.
                const auto sacl_present = (header.Control & SE_SACL_PRESENT) != 0;
                const auto dacl_present = (header.Control & SE_DACL_PRESENT) != 0;
                uint64_t owner{};
                uint64_t group{};
                uint64_t sacl{};
                uint64_t dacl{};
                if (self_relative)
                {
                    const auto at = [&](const DWORD offset) { return offset == 0 ? uint64_t{0} : address + offset; };
                    owner = at(header.Owner);
                    group = at(header.Group);
                    sacl = sacl_present ? at(header.Sacl) : 0;
                    dacl = dacl_present ? at(header.Dacl) : 0;
                }
                else
                {
                    const auto read_pointer = [&](const size_t offset, uint64_t& pointer) {
                        return read_user_memory(c, address + offset, &pointer, sizeof(pointer));
                    };

                    if (!read_pointer(offsetof(security_descriptor64, Owner), owner) ||
                        !read_pointer(offsetof(security_descriptor64, Group), group) ||
                        (sacl_present && !read_pointer(offsetof(security_descriptor64, Sacl), sacl)) ||
                        (dacl_present && !read_pointer(offsetof(security_descriptor64, Dacl), dacl)))
                    {
                        return STATUS_ACCESS_VIOLATION;
                    }
                }

                uint16_t sacl_size{};
                uint16_t dacl_size{};
                uint8_t owner_count{};
                uint8_t group_count{};
                if (const auto status = probe_acl(c, sacl, sacl_size); status != STATUS_SUCCESS)
                {
                    return status;
                }

                if (const auto status = probe_acl(c, dacl, dacl_size); status != STATUS_SUCCESS)
                {
                    return status;
                }

                if (const auto status = probe_sid(c, owner, owner_count); status != STATUS_SUCCESS)
                {
                    return status;
                }

                if (const auto status = probe_sid(c, group, group_count); status != STATUS_SUCCESS)
                {
                    return status;
                }

                captured_descriptor copy{};
                std::optional<std::vector<uint8_t>> sacl_copy{};
                if (const auto status = copy_acl(c, sacl, sacl_size, sacl_copy); status != STATUS_SUCCESS)
                {
                    return status;
                }

                if (const auto status = copy_acl(c, dacl, dacl_size, copy.dacl); status != STATUS_SUCCESS)
                {
                    return status;
                }

                if (const auto status = copy_sid(c, owner, owner_count, copy.owner); status != STATUS_SUCCESS)
                {
                    return status;
                }

                if (const auto status = copy_sid(c, group, group_count, copy.group); status != STATUS_SUCCESS)
                {
                    return status;
                }

                captured = std::move(copy);
                return STATUS_SUCCESS;
            }

            struct dacl_ace
            {
                uint8_t type{};
                uint8_t flags{};
                ACCESS_MASK mask{};
                std::vector<uint8_t> sid{};
                bool owner_rights{}; // RtlpOwnerAcesPresent finds OWNER RIGHTS in it
            };

            // The ACEs of a DACL RtlValidAcl has taken, with the masks and SIDs of its allow and deny ACEs.
            std::vector<dacl_ace> read_dacl(const std::span<const uint8_t> acl)
            {
                const auto header = read_at<ACL>(acl, 0);
                std::vector<dacl_ace> aces{};
                aces.reserve(header.AceCount);
                auto offset = sizeof(ACL);
                for (USHORT index = 0; index < header.AceCount; ++index)
                {
                    const auto ace_header = read_at<ACE_HEADER>(acl, offset);
                    const auto ace = acl.subspan(offset, ace_header.AceSize);
                    auto& entry = aces.emplace_back(dacl_ace{.type = ace_header.AceType,
                                                             .flags = ace_header.AceFlags,
                                                             .owner_rights = names_owner_rights(ace, ace_header.AceType)});
                    if (entry.type == ACCESS_ALLOWED_ACE_TYPE || entry.type == ACCESS_DENIED_ACE_TYPE)
                    {
                        entry.mask = read_at<ACCESS_MASK>(ace, offsetof(ACCESS_ALLOWED_ACE, Mask));
                        const auto sid = sid_at(ace, ace_sid_offset);
                        entry.sid.assign(sid.begin(), sid.end());
                    }

                    offset += ace_header.AceSize;
                }

                return aces;
            }

            struct access_check_result
            {
                NTSTATUS status;
                ACCESS_MASK granted;
            };

            // SeAccessCheckByType's owner rights, then SepAccessCheck, for a token that holds no privileges. A missing DACL
            // grants everything, MAXIMUM_ALLOWED in the mapping's GenericAll.
            access_check_result check_access(const windows_emulator& win_emu, const handle token,
                                             const std::optional<std::vector<dacl_ace>>& dacl, const bool is_owner, ACCESS_MASK desired,
                                             const ACCESS_MASK generic_all)
            {
                // SepNormalAccessCheck skips inherit-only ACEs and compares the masks as stored: an ACE's generic bits are
                // not mapped, so they match no specific right. An OWNER RIGHTS ACE stands for the owner.
                const auto applies = [&](const dacl_ace& ace) {
                    return (ace.flags & INHERIT_ONLY_ACE) == 0 && !ace.sid.empty() &&
                           (token_holds_sid(win_emu, token, ace.sid) || (is_owner && std::ranges::equal(ace.sid, owner_rights_sid)));
                };

                // RtlpOwnerAcesPresent: an OWNER RIGHTS ACE that is not inherit-only, of any type, replaces the owner's implicit
                // READ_CONTROL and WRITE_DAC.
                const auto has_owner_rights_ace = dacl && std::ranges::any_of(*dacl, [](const dacl_ace& ace) {
                                                      return (ace.flags & INHERIT_ONLY_ACE) == 0 && ace.owner_rights;
                                                  });

                ACCESS_MASK granted = 0;
                if (is_owner && !has_owner_rights_ace && (desired & (maximum_allowed | owner_implicit_rights)) != 0)
                {
                    granted = (desired & maximum_allowed) != 0 ? owner_implicit_rights : desired & owner_implicit_rights;
                    desired &= ~owner_implicit_rights;
                }

                if (desired == 0)
                {
                    return {.status = STATUS_SUCCESS, .granted = granted};
                }

                if (!dacl)
                {
                    granted |= desired;
                    if ((desired & maximum_allowed) != 0)
                    {
                        granted = (granted & ~maximum_allowed) | generic_all;
                    }

                    return {.status = STATUS_SUCCESS, .granted = granted};
                }

                if ((desired & maximum_allowed) != 0)
                {
                    // SepMaximumAccessCheck: each ACE grants or refuses only what the ACEs before it left open.
                    ACCESS_MASK allowed = 0;
                    ACCESS_MASK refused = 0;
                    for (const auto& ace : *dacl)
                    {
                        if (!applies(ace))
                        {
                            continue;
                        }

                        if (ace.type == ACCESS_ALLOWED_ACE_TYPE)
                        {
                            allowed |= ace.mask & ~refused;
                        }
                        else
                        {
                            refused |= ace.mask & ~allowed;
                        }
                    }

                    granted |= allowed;
                    if ((desired & ~(allowed | maximum_allowed)) != 0 || granted == 0)
                    {
                        return {.status = STATUS_ACCESS_DENIED, .granted = 0};
                    }

                    return {.status = STATUS_SUCCESS, .granted = granted};
                }

                auto remaining = desired;
                for (const auto& ace : *dacl)
                {
                    if (remaining == 0)
                    {
                        break;
                    }

                    if (!applies(ace))
                    {
                        continue;
                    }

                    if (ace.type == ACCESS_DENIED_ACE_TYPE && (ace.mask & remaining) != 0)
                    {
                        return {.status = STATUS_ACCESS_DENIED, .granted = 0};
                    }

                    if (ace.type == ACCESS_ALLOWED_ACE_TYPE)
                    {
                        remaining &= ~ace.mask;
                    }
                }

                if (remaining != 0)
                {
                    return {.status = STATUS_ACCESS_DENIED, .granted = 0};
                }

                return {.status = STATUS_SUCCESS, .granted = granted | desired};
            }
        }

        TOKEN_TYPE get_token_type(const handle token_handle)
        {
            const auto impersonation = token_handle == DUMMY_IMPERSONATION_TOKEN || token_handle == ANONYMOUS_IMPERSONATION_TOKEN;
            return impersonation ? TokenImpersonation : TokenPrimary;
        }

        // Apart from their privileges, tokens are modeled by their type alone and no access check consults them, so a
        // restricted token behaves like the token it was filtered from, and attributes set on a token are not recorded.
        NTSTATUS handle_NtFilterToken(const syscall_context& c, const handle existing_token_handle, const ULONG flags,
                                      const emulator_pointer /*sids_to_disable*/, const emulator_pointer privileges_to_delete,
                                      const emulator_pointer /*restricted_sids*/, const emulator_object<handle> new_token_handle)
        {
            const auto token = resolve_token(c, existing_token_handle);
            if (!is_modeled_token(token))
            {
                return token != 0 ? STATUS_INVALID_HANDLE : STATUS_NO_TOKEN;
            }

            auto privileges = copy_token_privileges(c, existing_token_handle);
            if (flags & disable_max_privilege)
            {
                std::erase_if(privileges, [](const token_privilege& privilege) { return privilege.luid != se_change_notify_privilege; });
            }
            if (privileges_to_delete)
            {
                const auto entries = read_privilege_list(c, privileges_to_delete);
                if (!entries)
                {
                    return STATUS_INVALID_PARAMETER;
                }

                for (const auto& entry : *entries)
                {
                    std::erase_if(privileges, [&](const token_privilege& privilege) {
                        return entry.luid_high == 0 && privilege.luid == entry.luid_low;
                    });
                }
            }

            new_token_handle.write(open_token(c, token, std::move(privileges)));
            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtDuplicateToken(const syscall_context& c, const handle requested_token, ACCESS_MASK /*desired_access*/,
                                         const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                                         const BOOLEAN effective_only, const TOKEN_TYPE type,
                                         const emulator_object<handle> new_token_handle)
        {
            if (const auto status = validate_object_attributes(object_attributes); status != STATUS_SUCCESS)
            {
                return status;
            }

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
            auto privileges = copy_token_privileges(c, requested_token);
            if (effective_only)
            {
                std::erase_if(privileges,
                              [](const token_privilege& privilege) { return (privilege.attributes & se_privilege_enabled) == 0; });
            }

            if (get_token_type(existing_token_handle) == type)
            {
                new_token_handle.write(open_token(c, existing_token_handle, std::move(privileges)));
            }
            else if (type == TokenPrimary)
            {
                new_token_handle.write(open_token(c, CURRENT_PROCESS_TOKEN, std::move(privileges)));
            }
            else
            {
                new_token_handle.write(open_token(c, DUMMY_IMPERSONATION_TOKEN, std::move(privileges)));
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
                std::vector<luid_and_attributes> entries{};
                if (const auto* privileges = find_token_privileges(c, requested_token))
                {
                    for (const auto& privilege : *privileges)
                    {
                        entries.push_back({.luid_low = privilege.luid, .attributes = privilege.attributes});
                    }
                }

                const auto required_size = sizeof(ULONG) + entries.size() * sizeof(luid_and_attributes);
                return_length.write(static_cast<ULONG>(required_size));
                if (token_information_length < required_size)
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }
                write_privilege_list(c, token_information, entries);
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

            if (token_information_class == TokenRestrictedSids)
            {
                return write_token_groups(c, token_information, token_information_length, return_length, {});
            }

            if (token_information_class == TokenGroups)
            {
                return write_token_groups(c, token_information, token_information_length, return_length,
                                          get_token_groups(c.win_emu, sid, anonymous));
            }

            if (token_information_class == TokenLogonSid)
            {
                if (anonymous)
                {
                    return STATUS_NOT_FOUND;
                }

                const auto groups = get_token_groups(c.win_emu, sid, anonymous);
                const auto logon = std::ranges::find_if(
                    groups, [](const token_group& group) { return (group.attributes & se_group_logon_id) == se_group_logon_id; });
                if (logon == groups.end())
                {
                    return STATUS_NOT_FOUND;
                }

                return write_token_groups(c, token_information, token_information_length, return_length, std::span{logon, 1});
            }

            if (token_information_class == TokenOwner)
            {
                // LSA makes Administrators the default owner of an administrator's unfiltered token.
                const auto owner_sid = !anonymous && c.win_emu.runs_elevated() ? std::span<const uint8_t>{administrators_sid} : sid;
                const auto required_size = sizeof(TOKEN_OWNER64) + owner_sid.size();
                return_length.write(static_cast<ULONG>(required_size));

                if (required_size > token_information_length)
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                TOKEN_OWNER64 owner{};
                owner.Owner = token_information + sizeof(TOKEN_OWNER64);

                emulator_object<TOKEN_OWNER64>{c.emu, token_information}.write(owner);
                c.emu.write_memory(token_information + sizeof(TOKEN_OWNER64), owner_sid.data(), owner_sid.size());
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
                stats.GroupCount = static_cast<ULONG>(count_token_groups(c.win_emu, sid, anonymous));
                const auto* privileges = find_token_privileges(c, requested_token);
                stats.PrivilegeCount = privileges ? static_cast<ULONG>(privileges->size()) : 0;
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
                                      const uint64_t privilege_set, const emulator_object<ULONG> privilege_set_length,
                                      const emulator_object<ACCESS_MASK> granted_access, const emulator_object<NTSTATUS> access_status)
        {
            // SeAccessCheckByType reads the privilege set's length first. For a caller in user mode
            // SeAccessCheckByTypeWithAdminlessChecks then probes AccessStatus, GrantedAccess and PrivilegeSetLength for
            // writing and the privilege set for its length, clears a set that has room for one, and copies the GenericMapping,
            // which must be DWORD-aligned, before it checks anything else. A fault ends the call with its status.
            ULONG set_length{};
            if (!read_user_memory(c, privilege_set_length.value(), &set_length, sizeof(set_length)) ||
                !probe_dword_for_write(c, access_status.value()) || !probe_dword_for_write(c, granted_access.value()) ||
                !probe_dword_for_write(c, privilege_set_length.value()))
            {
                return STATUS_ACCESS_VIOLATION;
            }

            if (const auto status = probe_for_write(c, privilege_set, set_length, sizeof(DWORD)); status != STATUS_SUCCESS)
            {
                return status;
            }

            constexpr ULONG no_privileges = 0;
            if (privilege_set && set_length >= minimum_privilege_set_size &&
                !c.emu.try_write_memory(privilege_set, &no_privileges, sizeof(no_privileges)))
            {
                return STATUS_ACCESS_VIOLATION;
            }

            if (generic_mapping.value() % sizeof(DWORD) != 0)
            {
                return STATUS_DATATYPE_MISALIGNMENT;
            }

            EMU_GENERIC_MAPPING mapping{};
            if (!read_user_memory(c, generic_mapping.value(), &mapping, sizeof(mapping)))
            {
                return STATUS_ACCESS_VIOLATION;
            }

            if ((desired_access & generic_rights) != 0)
            {
                return STATUS_GENERIC_NOT_MAPPED;
            }

            const auto client_token = resolve_token(c, requested_token);
            if (client_token != CURRENT_PROCESS_TOKEN && client_token != CURRENT_THREAD_TOKEN &&
                client_token != CURRENT_THREAD_EFFECTIVE_TOKEN && client_token != DUMMY_IMPERSONATION_TOKEN &&
                client_token != ANONYMOUS_IMPERSONATION_TOKEN)
            {
                return STATUS_INVALID_HANDLE;
            }

            // SeCaptureSecurityDescriptor checks and copies the descriptor before SeAccessCheckByType looks at its owner and
            // group, the privileges or the privilege set's size.
            std::optional<captured_descriptor> descriptor{};
            if (const auto status = capture_security_descriptor(c, security_descriptor, descriptor); status != STATUS_SUCCESS)
            {
                return status;
            }

            if (!descriptor || descriptor->owner.empty() || descriptor->group.empty())
            {
                return STATUS_INVALID_SECURITY_DESCR;
            }

            // SePrivilegePolicyCheck answers ACCESS_SYSTEM_SECURITY, which takes SeSecurityPrivilege, before the privilege set
            // is sized: the token holds no privileges, so the request is refused and grants nothing.
            if ((desired_access & access_system_security) != 0)
            {
                granted_access.write(0);
                access_status.write(STATUS_PRIVILEGE_NOT_HELD);
                return STATUS_SUCCESS;
            }

            if (set_length < minimum_privilege_set_size)
            {
                privilege_set_length.write(minimum_privilege_set_size);
                return STATUS_BUFFER_TOO_SMALL;
            }

            constexpr uint64_t empty_privilege_set = 0; // PrivilegeCount and Control
            if (!c.emu.try_write_memory(privilege_set, &empty_privilege_set, sizeof(empty_privilege_set)))
            {
                return STATUS_ACCESS_VIOLATION;
            }

            std::optional<std::vector<dacl_ace>> dacl{};
            if (descriptor->dacl)
            {
                dacl = read_dacl(*descriptor->dacl);
            }

            const auto is_owner = token_holds_sid(c.win_emu, client_token, descriptor->owner);
            const auto result = check_access(c.win_emu, client_token, dacl, is_owner, desired_access, mapping.GenericAll);
            granted_access.write(result.granted);
            access_status.write(result.status);
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
            constexpr ULONG se_privilege_used_for_access = 0x80000000;
            // A real token handle is accepted as well as the pseudo handles that name a token. The effective token is
            // not one of them: it is resolved only by the query calls, so Windows rejects it here.
            if (!c.proc.tokens.get(client_token) && !is_modeled_token(client_token))
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
            const auto set_size = sizeof(privilege_set_header) + (probed_entries * sizeof(luid_and_attributes));
            if (!is_writable_range(c.win_emu.memory, required_privileges, set_size) ||
                !is_writable_range(c.win_emu.memory, result.value(), sizeof(BOOLEAN)))
            {
                return STATUS_ACCESS_VIOLATION;
            }

            if (header->privilege_count > max_privilege_count)
            {
                return STATUS_INVALID_PARAMETER;
            }

            const auto* privileges = find_token_privileges(c, client_token);
            const auto entries = required_privileges + sizeof(privilege_set_header);
            ULONG held = 0;

            for (ULONG index = 0; index < header->privilege_count; ++index)
            {
                const emulator_object<luid_and_attributes> entry{c.emu, entries + (index * sizeof(luid_and_attributes))};
                auto requested = entry.read();
                const auto enabled =
                    privileges && requested.luid_high == 0 && std::ranges::any_of(*privileges, [&](const token_privilege& privilege) {
                        return privilege.luid == requested.luid_low && (privilege.attributes & se_privilege_enabled) != 0;
                    });

                held += enabled ? 1 : 0;
                requested.attributes = enabled ? (requested.attributes | se_privilege_used_for_access)
                                               : (requested.attributes & ~se_privilege_used_for_access);
                entry.write(requested);
            }

            const auto all_necessary = (header->control & privilege_set_all_necessary) != 0;
            result.write(all_necessary ? held == header->privilege_count : held > 0);
            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtQuerySecurityAttributesToken(const syscall_context& c, const handle requested_token,
                                                       const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> attributes,
                                                       const ULONG number_of_attributes, const uint64_t buffer, const ULONG buffer_length,
                                                       const emulator_object<ULONG> return_length)
        {
            const auto token_handle = resolve_token(c, requested_token);
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

        NTSTATUS handle_NtSetInformationToken(const syscall_context& c, const handle requested_token,
                                              const TOKEN_INFORMATION_CLASS token_information_class, const uint64_t /*token_information*/,
                                              const ULONG token_information_length)
        {
            const auto token = resolve_token(c, requested_token);
            if (!is_modeled_token(token))
            {
                return token != 0 ? STATUS_INVALID_HANDLE : STATUS_NO_TOKEN;
            }

            size_t minimum_length{};
            switch (token_information_class)
            {
            case TokenOwner:
            case TokenPrimaryGroup:
            case TokenDefaultDacl:
                minimum_length = sizeof(EmulatorTraits<Emu64>::PVOID);
                break;
            case TokenIntegrityLevel:
                minimum_length = 2 * sizeof(EmulatorTraits<Emu64>::PVOID);
                break;
            case TokenSessionId:
            case TokenVirtualizationEnabled:
            case TokenUIAccess:
            case TokenMandatoryPolicy:
                minimum_length = sizeof(ULONG);
                break;
            default:
                c.win_emu.log.error("Unsupported token set info class: %X\n", token_information_class);
                return STATUS_INVALID_INFO_CLASS;
            }

            return token_information_length < minimum_length ? STATUS_INFO_LENGTH_MISMATCH : STATUS_SUCCESS;
        }

        NTSTATUS handle_NtAdjustPrivilegesToken(const syscall_context& c, const handle requested_token,
                                                const BOOLEAN disable_all_privileges, const emulator_pointer new_state,
                                                const ULONG buffer_length, const emulator_pointer previous_state,
                                                const emulator_object<ULONG> return_length)
        {
            if (!is_modeled_token(resolve_token(c, requested_token)))
            {
                return STATUS_INVALID_HANDLE;
            }
            if (!disable_all_privileges && !new_state)
            {
                return STATUS_INVALID_PARAMETER;
            }

            std::vector<token_privilege> no_privileges{};
            auto* privileges = find_token_privileges(c, requested_token);
            if (!privileges)
            {
                privileges = &no_privileges;
            }

            bool not_all_assigned = false;
            std::vector<std::pair<size_t, ULONG>> changes{};
            if (disable_all_privileges)
            {
                for (size_t index = 0; index < privileges->size(); ++index)
                {
                    const auto attributes = (*privileges)[index].attributes;
                    if (attributes & se_privilege_enabled)
                    {
                        changes.emplace_back(index, attributes & ~se_privilege_enabled);
                    }
                }
            }
            else
            {
                const auto entries = read_privilege_list(c, new_state);
                if (!entries)
                {
                    return STATUS_INVALID_PARAMETER;
                }

                for (const auto& entry : *entries)
                {
                    const auto privilege = std::ranges::find_if(*privileges, [&](const token_privilege& candidate) {
                        return entry.luid_high == 0 && candidate.luid == entry.luid_low;
                    });
                    if (privilege == privileges->end())
                    {
                        not_all_assigned = true;
                        continue;
                    }

                    const auto attributes = (entry.attributes & se_privilege_removed) ? se_privilege_removed
                                                                                      : (privilege->attributes & ~se_privilege_enabled) |
                                                                                            (entry.attributes & se_privilege_enabled);
                    if (attributes != privilege->attributes)
                    {
                        changes.emplace_back(static_cast<size_t>(privilege - privileges->begin()), attributes);
                    }
                }
            }

            if (previous_state)
            {
                std::vector<luid_and_attributes> previous{};
                previous.reserve(changes.size());
                for (const auto& [index, attributes] : changes)
                {
                    // SepAdjustPrivileges clears a removed privilege without reporting its previous state.
                    if (attributes & se_privilege_removed)
                    {
                        continue;
                    }

                    previous.push_back({.luid_low = (*privileges)[index].luid, .attributes = (*privileges)[index].attributes});
                }

                // SepAdjustPrivileges asks for a whole TOKEN_PRIVILEGES, with its one inline entry, even when it reports none.
                const auto required_size = sizeof(ULONG) + std::max<size_t>(previous.size(), 1) * sizeof(luid_and_attributes);
                return_length.write(static_cast<ULONG>(required_size));
                if (buffer_length < required_size)
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }
                write_privilege_list(c, previous_state, previous);
            }

            for (const auto& [index, attributes] : changes)
            {
                (*privileges)[index].attributes = attributes;
            }
            std::erase_if(*privileges, [](const token_privilege& privilege) { return (privilege.attributes & se_privilege_removed) != 0; });

            return not_all_assigned ? STATUS_NOT_ALL_ASSIGNED : STATUS_SUCCESS;
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
