#include "../std_include.hpp"
#include "api_port.hpp"

#include "../windows_emulator.hpp"

namespace
{
    struct api_port : port
    {
        static std::vector<uint64_t> scan_rip_relative_lea_references(const std::vector<uint8_t>& buf, uint64_t base, size_t max = 0)
        {
            std::vector<uint64_t> results;
            if (max)
            {
                results.reserve(max);
            }

            for (size_t i = 0; i + 6 < buf.size(); ++i)
            {
                if (buf[i] == 0x48 && buf[i + 1] == 0x8D && (buf[i + 2] & 0xC7) == 0x05)
                {
                    int32_t disp{};
                    std::memcpy(&disp, &buf[i + 3], sizeof(disp));

                    results.push_back(base + i + 7 + disp);
                    if (max && results.size() >= max)
                    {
                        break;
                    }
                }
            }
            return results;
        }

        // Normally, the client PFN arrays are initialized via the NtUserInitializeClientPfnArrays syscall invoked by the CSRSS process.
        // However, since the emulator does not emulate the CSRSS process, we need to manually retrieve the pointers by scanning the
        // RtlRetrieveNtUserPfn function.
        static bool retrieve_user_pfn(windows_emulator& win_emu, uint64_t& apfnClientA, uint64_t& apfnClientW, uint64_t& apfnClientWorker)
        {
            try
            {
                const auto retrieve_user_pfn = win_emu.mod_manager.ntdll->find_export("RtlRetrieveNtUserPfn");

                if (!retrieve_user_pfn)
                {
                    return false;
                }

                std::vector<uint8_t> buffer(128);
                win_emu.memory.read_memory(retrieve_user_pfn, buffer.data(), buffer.size());

                const std::vector<uint64_t> resolved_globals = scan_rip_relative_lea_references(buffer, retrieve_user_pfn, 3);
                if (resolved_globals.size() != 3)
                {
                    return false;
                }

                apfnClientA = resolved_globals[0];
                apfnClientW = resolved_globals[1];
                apfnClientWorker = resolved_globals[2];
                return true;
            }
            catch (...)
            {
                return false;
            }
        }

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

            uint64_t apfnClientA{};
            uint64_t apfnClientW{};
            uint64_t apfnClientWorker{};
            if (retrieve_user_pfn(win_emu, apfnClientA, apfnClientW, apfnClientWorker))
            {
                win_emu.process.user_handles.get_server_info().access([&](USER_SERVERINFO& server_info) {
                    win_emu.memory.read_memory(apfnClientA, &server_info.apfnClientA, sizeof(server_info.apfnClientA));
                    win_emu.memory.read_memory(apfnClientW, &server_info.apfnClientW, sizeof(server_info.apfnClientW));
                    win_emu.memory.read_memory(apfnClientWorker, &server_info.apfnClientWorker, sizeof(server_info.apfnClientWorker));

                    // The DispatchClientMessage method is the same in both arrays.
                    win_emu.process.dispatch_client_message = server_info.apfnClientA[21];
                });
            }

            return STATUS_SUCCESS;
        }
    };
}

std::unique_ptr<port> create_api_port()
{
    return std::make_unique<api_port>();
}
