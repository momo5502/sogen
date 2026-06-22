#include "../std_include.hpp"
#include "lsa_policy_lookup.hpp"

#include "binary_writer.hpp"
#include "../registry/registry_utils.hpp"
#include "../windows_emulator.hpp"

namespace sogen
{

    namespace
    {
        constexpr NTSTATUS k_status_none_mapped = static_cast<NTSTATUS>(0xC0000073);
        constexpr ULONG k_open_policy_reply_size = 0x44;
        constexpr ULONG k_lookup_sids_not_mapped_reply_size = 0x18;
        constexpr ULONG k_lookup_sids_min_success_reply_size = 0x10C;
        constexpr ULONG k_close_policy_reply_size = 0x44;
        constexpr uint32_t k_lsa_max_referenced_domains = 0x20;
        constexpr uint32_t k_sid_type_user = 1;
        constexpr std::array<uint8_t, 16> k_policy_context_uuid = {
            0xBA, 0xA3, 0xDA, 0x01, 0x6E, 0x16, 0x62, 0x49, 0x81, 0x46, 0x13, 0x9B, 0x8A, 0x70, 0x69, 0xE7,
        };

        bool request_contains_sid(windows_emulator& win_emu, const lpc_request_context& c, const std::span<const uint8_t> sid)
        {
            if (sid.empty() || c.send_buffer_length < sid.size())
            {
                return false;
            }

            std::vector<uint8_t> request(c.send_buffer_length, 0);
            win_emu.emu().read_memory(c.send_buffer, request.data(), request.size());

            const auto it = std::ranges::search(request, sid);
            return it.begin() != request.end();
        }

        bool is_domain_user_sid(const std::span<const uint8_t> sid)
        {
            if (sid.size() < 12)
            {
                return false;
            }

            const auto sub_authority_count = sid[1];
            if (sub_authority_count < 2 || sid.size() < static_cast<size_t>(8 + (sub_authority_count * sizeof(uint32_t))))
            {
                return false;
            }

            constexpr std::array<uint8_t, 6> nt_authority = {0, 0, 0, 0, 0, 5};
            if (!std::ranges::equal(nt_authority, sid.subspan(2, nt_authority.size())))
            {
                return false;
            }

            uint32_t first_sub_authority{};
            std::memcpy(&first_sub_authority, sid.data() + 8, sizeof(first_sub_authority));
            return first_sub_authority == 21;
        }

        std::vector<uint8_t> derive_account_domain_sid(const std::span<const uint8_t> sid)
        {
            if (!is_domain_user_sid(sid))
            {
                return {sid.begin(), sid.end()};
            }

            std::vector<uint8_t> domain_sid(sid.begin(), sid.end() - sizeof(uint32_t));
            --domain_sid[1];
            return domain_sid;
        }

        void write_lsa_unicode_string(utils::aligned_binary_writer& writer, const std::u16string_view value)
        {
            writer.write(static_cast<uint16_t>(value.size() * sizeof(char16_t)));
            writer.write(static_cast<uint16_t>((value.size() + 1) * sizeof(char16_t)));
            writer.write_ndr_pointer(true);
        }

        void write_lsa_translated_name(utils::aligned_binary_writer& writer, const std::u16string_view value)
        {
            writer.write(k_sid_type_user);
            writer.align_to(writer.pointer_size());
            write_lsa_unicode_string(writer, value);
            writer.write<int32_t>(0);
            writer.align_to(writer.pointer_size());
        }

        void write_lookup_sids_success_reply(utils::aligned_binary_writer& writer, const std::u16string_view domain,
                                             const std::span<const uint8_t> domain_sid, const std::u16string_view user)
        {
            writer.write_ndr_pointer(true);

            writer.write<uint32_t>(1);
            writer.write_ndr_pointer(true);
            writer.write(k_lsa_max_referenced_domains);

            writer.write_pointer_sized(1);
            write_lsa_unicode_string(writer, domain);
            writer.write_ndr_pointer(true);

            writer.write_ndr_u16string(domain, false);
            writer.align_to(writer.pointer_size());

            writer.write(static_cast<uint32_t>(domain_sid.empty() ? 0 : domain_sid[1]));
            writer.align_to(writer.pointer_size());
            writer.write(domain_sid.data(), domain_sid.size(), alignof(uint32_t));
            writer.align_to(writer.pointer_size());

            writer.write<uint32_t>(1);
            writer.write_ndr_pointer(true);
            writer.write_pointer_sized(1);

            write_lsa_translated_name(writer, user);
            writer.write_ndr_u16string(user, false);

            writer.write<uint32_t>(1);
            writer.write(STATUS_SUCCESS);

            if (writer.offset() < k_lookup_sids_min_success_reply_size)
            {
                writer.pad(static_cast<size_t>(k_lookup_sids_min_success_reply_size - writer.offset()));
            }
        }

        struct lsa_policy_lookup_port : rpc_port
        {
            NTSTATUS handle_rpc(windows_emulator& win_emu, const uint32_t procedure_id, const lpc_request_context& c,
                                utils::aligned_binary_writer& writer, std::vector<alpc_reply_handle>& /*reply_handles*/) override
            {
                switch (procedure_id)
                {
                case 0:
                    return handle_open_policy(win_emu, writer);
                case 1:
                    return handle_close_policy(win_emu, writer);
                case 2:
                    return handle_lookup_sids(win_emu, c, writer);
                case 5:
                    // The shell's account-lookup path (e.g. SHGetKnownFolderPath, used while the engine sets
                    // up its crash-report queue) issues this additional lookup. We don't resolve it, but we
                    // must answer with a well-formed "none mapped" result: returning an RPC error makes the
                    // runtime raise RPC_S_CALL_FAILED in the caller, whose vectored exception handler then
                    // re-enters a half-initialized singleton and deadlocks.
                    return write_lookup_not_mapped_reply(writer);
                default:
                    return STATUS_NOT_SUPPORTED;
                }
            }

          private:
            static NTSTATUS handle_open_policy(windows_emulator& win_emu, utils::aligned_binary_writer& writer)
            {
                (void)win_emu;
                const auto reply_offset = writer.offset();
                writer.pad(k_open_policy_reply_size);
                writer.write_at(reply_offset + 0x04, k_policy_context_uuid.data(), k_policy_context_uuid.size());

                return STATUS_SUCCESS;
            }

            static NTSTATUS handle_lookup_sids(windows_emulator& win_emu, const lpc_request_context& c,
                                               utils::aligned_binary_writer& writer)
            {
                if (request_contains_sid(win_emu, c, win_emu.process.sid))
                {
                    const auto domain = registry_utils::get_account_domain(win_emu.registry);
                    const auto domain_sid = derive_account_domain_sid(win_emu.process.sid);
                    const auto user = registry_utils::get_user_name(win_emu.registry);

                    write_lookup_sids_success_reply(writer, domain, domain_sid, user);

                    return STATUS_SUCCESS;
                }

                return write_lookup_not_mapped_reply(writer);
            }

            static NTSTATUS write_lookup_not_mapped_reply(utils::aligned_binary_writer& writer)
            {
                const auto reply_offset = writer.offset();
                writer.pad(k_lookup_sids_not_mapped_reply_size);
                writer.write_at(reply_offset + 0x14, k_status_none_mapped);
                return STATUS_SUCCESS;
            }

            static NTSTATUS handle_close_policy(windows_emulator& win_emu, utils::aligned_binary_writer& writer)
            {
                (void)win_emu;
                writer.pad(k_close_policy_reply_size);
                return STATUS_SUCCESS;
            }
        };
    }

    std::unique_ptr<port> create_lsa_policy_lookup_port()
    {
        return std::make_unique<lsa_policy_lookup_port>();
    }

} // namespace sogen
