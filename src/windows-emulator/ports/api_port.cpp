#include "../std_include.hpp"
#include "api_port.hpp"

#include "../win32k_userconnect.hpp"
#include "../windows_emulator.hpp"

namespace
{
    bool is_user_server_dll_index(const uint64_t server_dll_index)
    {
        return server_dll_index <= std::numeric_limits<uint32_t>::max() &&
               static_cast<uint32_t>(server_dll_index) == win32k_userconnect::k_user_server_dll_index;
    }

    struct wow64_userconnect_payload
    {
        uint64_t request_capture_handle{};
        uint64_t request_reserved0{};
        uint64_t capture_buffer_ptr{};
        uint64_t server_dll_index{};
        uint64_t user_connect_ptr{};
        uint64_t user_connect_length{};
    };

    static_assert(sizeof(wow64_userconnect_payload) == 0x30);
    static_assert(offsetof(wow64_userconnect_payload, capture_buffer_ptr) == 0x10);
    static_assert(offsetof(wow64_userconnect_payload, server_dll_index) == 0x18);
    static_assert(offsetof(wow64_userconnect_payload, user_connect_ptr) == 0x20);
    static_assert(offsetof(wow64_userconnect_payload, user_connect_length) == 0x28);

    bool try_read_wow64_payload(windows_emulator& win_emu, const lpc_request_context& c, wow64_userconnect_payload& payload)
    {
        if (c.send_buffer_length < sizeof(payload))
        {
            return false;
        }

        if (!win_emu.memory.try_read_memory(c.send_buffer, &payload, sizeof(payload)))
        {
            return false;
        }

        if (!is_user_server_dll_index(payload.server_dll_index))
        {
            return false;
        }

        if (payload.user_connect_ptr == 0 || payload.user_connect_ptr > std::numeric_limits<uint32_t>::max())
        {
            return false;
        }

        if (payload.user_connect_length != sizeof(WIN32K_USERCONNECT32) &&
            payload.user_connect_length != (sizeof(WIN32K_USERCONNECT32) + win32k_userconnect::k_wow64_userconnect_header_size))
        {
            return false;
        }

        return true;
    }

    bool try_read_reply_server_dll_index(windows_emulator& win_emu, const lpc_request_context& c, uint32_t& server_dll_index)
    {
        server_dll_index = 0;
        return win_emu.memory.try_read_memory(c.recv_buffer + 0x18, &server_dll_index, sizeof(server_dll_index));
    }

    NTSTATUS try_write_wow64_userconnect_response(windows_emulator& win_emu, const wow64_userconnect_payload& payload)
    {
        uint32_t destination{};
        const auto destination_status =
            win32k_userconnect::resolve_wow64_destination(payload.user_connect_ptr, payload.user_connect_length, destination);
        if (destination_status != STATUS_SUCCESS)
        {
            return destination_status;
        }

        WIN32K_USERCONNECT32 connect{};
        const auto connect_status = win32k_userconnect::build_wow64_userconnect(win_emu.process, connect);
        if (connect_status != STATUS_SUCCESS)
        {
            return connect_status;
        }

        if (!win32k_userconnect::try_write_wow64_userconnect(win_emu.memory, destination, connect))
        {
            return STATUS_INVALID_PARAMETER;
        }

        return STATUS_SUCCESS;
    }

    struct api_port : port
    {
        static bool resolve_reply_base(windows_emulator& win_emu, const lpc_request_context& c, uint64_t& base)
        {
            base = 0;

            // CSRSS ApiPort connect reply has PORT_DATA_ENTRY @ +0x20.
            if (c.recv_buffer_length < (0x20 + sizeof(PORT_DATA_ENTRY<EmulatorTraits<Emu64>>)))
            {
                return false;
            }

            PORT_DATA_ENTRY<EmulatorTraits<Emu64>> direct_entry{};
            if (!win_emu.memory.try_read_memory(c.recv_buffer + 0x20, &direct_entry, sizeof(direct_entry)))
            {
                return false;
            }

            if (direct_entry.Base == 0)
            {
                return false;
            }

            base = direct_entry.Base;
            return true;
        }

        NTSTATUS handle_request(windows_emulator& win_emu, const lpc_request_context& c) override
        {
            wow64_userconnect_payload payload{};
            if (try_read_wow64_payload(win_emu, c, payload))
            {
                const auto status = try_write_wow64_userconnect_response(win_emu, payload);
                if (status != STATUS_SUCCESS)
                {
                    win_emu.log.warn("ApiPort WOW64 userconnect write failed: status=0x%X, ptr=0x%llX, len=0x%llX\n", status,
                                     static_cast<unsigned long long>(payload.user_connect_ptr),
                                     static_cast<unsigned long long>(payload.user_connect_length));
                    return status;
                }

                if (!win32k_userconnect::try_bootstrap_client_pfn_arrays_from_ntdll(win_emu))
                {
                    win_emu.log.warn("ApiPort userconnect callback-table bootstrap failed\n");
                }

                return STATUS_SUCCESS;
            }

            uint32_t server_dll_index{};
            if (!try_read_reply_server_dll_index(win_emu, c, server_dll_index) ||
                server_dll_index != win32k_userconnect::k_user_server_dll_index)
            {
                return STATUS_SUCCESS;
            }

            uint64_t base{};
            if (!resolve_reply_base(win_emu, c, base))
            {
                win_emu.log.warn("ApiPort userconnect reply base resolution failed\n");
                return STATUS_INVALID_PARAMETER;
            }

            if (!win32k_userconnect::try_write_api_port_userconnect_reply(win_emu.memory, base, win_emu.process))
            {
                win_emu.log.warn("ApiPort userconnect shared info write failed\n");
                return STATUS_INVALID_PARAMETER;
            }

            if (!win32k_userconnect::try_bootstrap_client_pfn_arrays_from_ntdll(win_emu))
            {
                win_emu.log.warn("ApiPort userconnect callback-table bootstrap failed\n");
            }

            return STATUS_SUCCESS;
        }
    };
}

std::unique_ptr<port> create_api_port()
{
    return std::make_unique<api_port>();
}
