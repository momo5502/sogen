#include "../std_include.hpp"
#include "lsa_policy_lookup.hpp"

#include "../windows_emulator.hpp"

namespace
{
    constexpr NTSTATUS k_status_none_mapped = static_cast<NTSTATUS>(0xC0000073);
    constexpr ULONG k_request_cookie_size = 8;
    constexpr ULONG k_open_policy_reply_size = 0x20;
    constexpr ULONG k_lookup_names_reply_size = 0x18;
    constexpr ULONG k_close_policy_reply_size = 0x0C;
    constexpr ULONG k_open_policy_context_attr_offset = 0x08;
    constexpr ULONG k_open_policy_ntstatus_offset = 0x1C;
    constexpr ULONG k_lookup_names_status_offset = 0x14;
    constexpr ULONG k_close_policy_status_offset = 0x08;

    struct lsa_policy_lookup_port : rpc_port
    {
        NTSTATUS handle_rpc(windows_emulator& win_emu, const uint32_t procedure_id, const lpc_request_context& c) override
        {
            if (!write_request_cookie(win_emu, c))
            {
                return STATUS_BUFFER_TOO_SMALL;
            }

            switch (procedure_id)
            {
            case 4:
                return handle_open_policy(win_emu, c);
            case 2:
                return handle_lookup_names(win_emu, c);
            case 3:
                return handle_close_policy(win_emu, c);
            default:
                // win_emu.log.warn("Unsupported lsapolicylookup procedure: %u\n", procedure_id);
                return STATUS_NOT_SUPPORTED;
            }
        }

      private:
        static bool ensure_recv_capacity(windows_emulator& win_emu, const lpc_request_context& c, const ULONG required,
                                         const char* operation)
        {
            if (c.recv_buffer_length >= required)
            {
                return true;
            }

            win_emu.log.warn("lsapolicylookup %s reply buffer too small: have 0x%X need 0x%X\n", operation, c.recv_buffer_length, required);
            return false;
        }

        static bool write_request_cookie(windows_emulator& win_emu, const lpc_request_context& c)
        {
            if (!ensure_recv_capacity(win_emu, c, k_request_cookie_size, "cookie"))
            {
                return false;
            }

            std::array<uint8_t, 8> request_cookie{};
            if (c.send_buffer_length >= k_request_cookie_size)
            {
                win_emu.emu().read_memory(c.send_buffer + c.send_buffer_length - k_request_cookie_size, request_cookie.data(),
                                          request_cookie.size());
            }
            win_emu.emu().write_memory(c.recv_buffer, request_cookie.data(), request_cookie.size());
            return true;
        }

        static NTSTATUS handle_open_policy(windows_emulator& win_emu, const lpc_request_context& c)
        {
            if (!ensure_recv_capacity(win_emu, c, k_open_policy_reply_size, "open_policy"))
            {
                return STATUS_BUFFER_TOO_SMALL;
            }

            std::array<std::byte, k_open_policy_reply_size> zeros{};
            win_emu.emu().write_memory(c.recv_buffer, zeros.data(), zeros.size());

            win_emu.emu().write_memory<uint32_t>(c.recv_buffer + k_open_policy_context_attr_offset, 0);
            win_emu.emu().write_memory<uint32_t>(c.recv_buffer + k_open_policy_ntstatus_offset, STATUS_SUCCESS);
            c.recv_buffer_length = k_open_policy_reply_size;
            return STATUS_SUCCESS;
        }

        static NTSTATUS handle_lookup_names(windows_emulator& win_emu, const lpc_request_context& c)
        {
            if (!ensure_recv_capacity(win_emu, c, k_lookup_names_reply_size, "lookup_names"))
            {
                return STATUS_BUFFER_TOO_SMALL;
            }

            win_emu.emu().write_memory<uint32_t>(c.recv_buffer + 0x08, 0);
            win_emu.emu().write_memory<uint32_t>(c.recv_buffer + 0x0C, 0);
            win_emu.emu().write_memory<uint32_t>(c.recv_buffer + 0x10, 0);
            win_emu.emu().write_memory<uint32_t>(c.recv_buffer + k_lookup_names_status_offset, k_status_none_mapped);
            c.recv_buffer_length = k_lookup_names_reply_size;
            return STATUS_SUCCESS;
        }

        static NTSTATUS handle_close_policy(windows_emulator& win_emu, const lpc_request_context& c)
        {
            if (!ensure_recv_capacity(win_emu, c, k_close_policy_reply_size, "close_policy"))
            {
                return STATUS_BUFFER_TOO_SMALL;
            }

            win_emu.emu().write_memory<uint32_t>(c.recv_buffer + k_close_policy_status_offset, STATUS_SUCCESS);
            c.recv_buffer_length = k_close_policy_reply_size;
            return STATUS_SUCCESS;
        }
    };
}

std::unique_ptr<port> create_lsa_policy_lookup_port()
{
    return std::make_unique<lsa_policy_lookup_port>();
}
