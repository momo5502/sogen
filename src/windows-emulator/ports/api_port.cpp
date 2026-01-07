#include "../std_include.hpp"
#include "api_port.hpp"

#include "../windows_emulator.hpp"

namespace
{
    struct api_port : port
    {
        NTSTATUS handle_request(windows_emulator& win_emu, const lpc_request_context& c) override
        {
            uint32_t server_dll_index{};
            win_emu.memory.read_memory(c.recv_buffer + 0x18, &server_dll_index, sizeof(server_dll_index));

            if (server_dll_index != 3)
            {
                return STATUS_NOT_SUPPORTED;
            }

            try
            {
                const emulator_object<PORT_DATA_ENTRY<EmulatorTraits<Emu64>>> data{win_emu.emu(), c.recv_buffer + 0x20};
                const auto dest = data.read();
                const auto base = dest.Base;

                const emulator_object<USER_SHAREDINFO> shared_obj{win_emu.emu(), base + 8};
                shared_obj.access([&](USER_SHAREDINFO& shared) {
                    shared.psi = win_emu.process.user_handles.get_server_info().value();
                    shared.aheList = win_emu.process.user_handles.get_handle_table().value();
                    shared.HeEntrySize = sizeof(USER_HANDLEENTRY);
                    shared.pDispInfo = win_emu.process.user_handles.get_display_info().value();
                });
            }
            catch (...)
            {
                return STATUS_NOT_SUPPORTED;
            }

            return STATUS_SUCCESS;
        }
    };
}

std::unique_ptr<port> create_api_port()
{
    return std::make_unique<api_port>();
}
