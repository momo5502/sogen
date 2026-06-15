#include "std_include.hpp"
#include "win32k_userconnect.hpp"

#include "process_context.hpp"
#include "windows_emulator.hpp"

namespace sogen
{

    namespace
    {
        constexpr size_t k_dispatch_client_message_index = 21;
        constexpr size_t k_ntdll_probe_size = 128;
        constexpr size_t k_expected_pfn_pointer_count = 3;
        constexpr size_t k_client_pfn_array_size = FNID_ARRAY_SIZE * sizeof(uint64_t);
        constexpr size_t k_client_worker_pfn_array_size = 0x90;

        struct client_pfn_arrays
        {
            uint64_t ansi{};
            uint64_t wide{};
            uint64_t worker{};
        };

        std::vector<uint64_t> scan_rip_relative_lea_references(const std::vector<uint8_t>& bytes, const uint64_t code_base,
                                                               const size_t max_results)
        {
            std::vector<uint64_t> results;
            results.reserve(max_results);

            for (size_t i = 0; i + 6 < bytes.size(); ++i)
            {
                if (bytes[i] != 0x48 || bytes[i + 1] != 0x8D || (bytes[i + 2] & 0xC7) != 0x05)
                {
                    continue;
                }

                int32_t disp{};
                std::memcpy(&disp, &bytes[i + 3], sizeof(disp));
                results.push_back(code_base + i + 7 + disp);

                if (results.size() >= max_results)
                {
                    break;
                }
            }

            return results;
        }

        void refresh_dispatch_client_message(process_context& process)
        {
            uint64_t dispatch_client_message = 0;

            process.user_handles.get_server_info().access([&](const USER_SERVERINFO& server_info) {
                dispatch_client_message = server_info.apfnClientA[k_dispatch_client_message_index];
                if (dispatch_client_message == 0)
                {
                    dispatch_client_message = server_info.apfnClientW[k_dispatch_client_message_index];
                }
            });

            if (dispatch_client_message != 0)
            {
                process.dispatch_client_message = dispatch_client_message;
            }
        }

        bool try_read_exact(memory_interface& memory, const uint64_t address, void* data, const size_t size)
        {
            return address != 0 && memory.try_read_memory(address, data, size);
        }

        // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays)
        bool try_copy_client_pfn_array(memory_interface& memory, const uint64_t source, uint64_t (&destination)[FNID_ARRAY_SIZE],
                                       const size_t source_size)
        {
            std::ranges::fill(destination, 0);
            return try_read_exact(memory, source, destination, std::min(sizeof(destination), source_size));
        }

        // user32 builds MessageBox dialogs from SERVERINFO.MBStrings: an array of
        // { WCHAR achName[16]; UINT id; UINT uStr; } (0x28 bytes) at gpsi + 0x3A4, indexed by the
        // standard order OK, Cancel, Abort, Retry, Ignore, Yes, No, Close, Help, Try Again, Continue.
        // SoftModalMessageBox reads each button's control id (+0x20) and caption (+0x00) from here, so
        // without it message-box buttons get id 0 (wrong return value) and empty captions.
        void seed_messagebox_button_strings(memory_interface& memory, const uint64_t serverinfo_base)
        {
            if (serverinfo_base == 0)
            {
                return;
            }

            struct mb_string
            {
                std::u16string_view text;
                uint32_t id;
            };
            static constexpr std::array<mb_string, 11> entries = {{
                {.text = u"OK", .id = 1},          // IDOK
                {.text = u"Cancel", .id = 2},      // IDCANCEL
                {.text = u"&Abort", .id = 3},      // IDABORT
                {.text = u"&Retry", .id = 4},      // IDRETRY
                {.text = u"&Ignore", .id = 5},     // IDIGNORE
                {.text = u"&Yes", .id = 6},        // IDYES
                {.text = u"&No", .id = 7},         // IDNO
                {.text = u"&Close", .id = 8},      // IDCLOSE
                {.text = u"Help", .id = 9},        // IDHELP
                {.text = u"&Try Again", .id = 10}, // IDTRYAGAIN
                {.text = u"&Continue", .id = 11},  // IDCONTINUE
            }};

            constexpr uint64_t k_mbstrings_offset = 0x3A4;
            constexpr uint64_t k_mbstrings_entry_size = 0x28;
            constexpr uint64_t k_mbstrings_id_offset = 0x20;
            constexpr size_t k_mbstrings_name_capacity = 16; // WCHAR achName[16]

            for (size_t i = 0; i < entries.size(); ++i)
            {
                const auto entry_base = serverinfo_base + k_mbstrings_offset + i * k_mbstrings_entry_size;

                std::array<char16_t, k_mbstrings_name_capacity> name{};
                const auto copy_count = std::min<size_t>(entries[i].text.size(), k_mbstrings_name_capacity - 1);
                std::ranges::copy_n(entries[i].text.begin(), static_cast<std::ptrdiff_t>(copy_count), name.begin());
                memory.write_memory(entry_base, name.data(), name.size() * sizeof(char16_t));

                const auto id = entries[i].id;
                memory.write_memory(entry_base + k_mbstrings_id_offset, &id, sizeof(id));
            }
        }

        bool try_copy_client_pfn_arrays(memory_interface& memory, process_context& process, const client_pfn_arrays arrays)
        {
            if (arrays.ansi == 0 || arrays.wide == 0 || arrays.worker == 0)
            {
                return false;
            }

            bool copied = false;
            process.user_handles.get_server_info().access([&](USER_SERVERINFO& server_info) {
                copied = try_copy_client_pfn_array(memory, arrays.ansi, server_info.apfnClientA, k_client_pfn_array_size) &&
                         try_copy_client_pfn_array(memory, arrays.wide, server_info.apfnClientW, k_client_pfn_array_size) &&
                         try_copy_client_pfn_array(memory, arrays.worker, server_info.apfnClientWorker, k_client_worker_pfn_array_size);
            });

            if (!copied)
            {
                return false;
            }

            // Seed after the pfn copy: the worker-array fill above zeroes part of the MBStrings region.
            seed_messagebox_button_strings(memory, process.user_handles.get_server_info().value());

            refresh_dispatch_client_message(process);
            return true;
        }

    }

    namespace win32k_userconnect
    {
        NTSTATUS narrow_wow64_address(const uint64_t address, uint32_t& narrowed)
        {
            narrowed = 0;

            if (address > std::numeric_limits<uint32_t>::max())
            {
                return STATUS_INVALID_PARAMETER;
            }

            narrowed = static_cast<uint32_t>(address);
            return STATUS_SUCCESS;
        }

        NTSTATUS resolve_wow64_destination(const uint64_t user_connect_ptr, const uint64_t user_connect_length, uint32_t& destination)
        {
            destination = 0;

            if (user_connect_ptr == 0)
            {
                return STATUS_INVALID_PARAMETER;
            }

            if (user_connect_length < sizeof(WIN32K_USERCONNECT32))
            {
                return STATUS_BUFFER_TOO_SMALL;
            }

            uint64_t offset = 0;
            if (user_connect_length == sizeof(WIN32K_USERCONNECT32))
            {
                offset = 0;
            }
            else if (user_connect_length == (sizeof(WIN32K_USERCONNECT32) + k_wow64_userconnect_header_size))
            {
                offset = k_wow64_userconnect_header_size;
            }
            else
            {
                return STATUS_INVALID_PARAMETER;
            }

            const auto destination64 = user_connect_ptr + offset;
            if (destination64 < user_connect_ptr)
            {
                return STATUS_INVALID_PARAMETER;
            }

            return narrow_wow64_address(destination64, destination);
        }

        NTSTATUS build_wow64_userconnect(const process_context& process, WIN32K_USERCONNECT32& connect)
        {
            connect = {};

            uint32_t psi{};
            uint32_t disp_info{};
            uint32_t ahe_list{};
            uint32_t monitor_info{};

            auto status = narrow_wow64_address(process.user_handles.get_server_info().value(), psi);
            if (status != STATUS_SUCCESS)
            {
                return status;
            }

            status = narrow_wow64_address(process.user_handles.get_display_info().value(), disp_info);
            if (status != STATUS_SUCCESS)
            {
                return status;
            }

            status = narrow_wow64_address(process.user_handles.get_handle_table().value(), ahe_list);
            if (status != STATUS_SUCCESS)
            {
                return status;
            }

            status = narrow_wow64_address(process.user_handles.get_display_info().value(), monitor_info);
            if (status != STATUS_SUCCESS)
            {
                return status;
            }

            connect.psi = psi;
            connect.ahe_list = ahe_list;
            connect.he_entry_size = sizeof(USER_HANDLEENTRY);
            connect.disp_info_low = disp_info;
            connect.monitor_info_low = monitor_info;
            std::ranges::fill(connect.wndmsg_table, uint8_t{0xFF});
            connect.wndmsg_count = k_wow64_wndmsg_count;
            connect.ime_msg_count = k_wow64_ime_msg_count;

            return STATUS_SUCCESS;
        }

        bool try_write_wow64_userconnect(memory_interface& memory, const uint64_t destination, const WIN32K_USERCONNECT32& connect)
        {
            try
            {
                const emulator_object<WIN32K_USERCONNECT32> connect_obj{memory, destination};
                connect_obj.write(connect);
                return true;
            }
            catch (...)
            {
                return false;
            }
        }

        void populate_user_shared_info(USER_SHAREDINFO& shared, const process_context& process)
        {
            shared.psi = process.user_handles.get_server_info().value();
            shared.aheList = process.user_handles.get_handle_table().value();
            shared.HeEntrySize = sizeof(USER_HANDLEENTRY);
            shared.pDispInfo = process.user_handles.get_display_info().value();
            for (int i = 0; i < FNID_ARRAY_SIZE; i++)
            {
                shared.awmControl[i] = process.user_handles.get_awm_control_message(i);
            }
            shared.DefWindowMsgs = process.user_handles.get_def_window_messages();
            shared.DefWindowSpecMsgs = process.user_handles.get_def_window_spec_messages();
        }

        bool try_write_user_shared_info(memory_interface& memory, const uint64_t destination, const process_context& process)
        {
            try
            {
                const emulator_object<USER_SHAREDINFO> shared_obj{memory, destination};
                auto shared = shared_obj.read();
                populate_user_shared_info(shared, process);
                shared_obj.write(shared);
                return true;
            }
            catch (...)
            {
                return false;
            }
        }

        bool try_write_api_port_userconnect_reply(memory_interface& memory, const uint64_t reply_base, const process_context& process)
        {
            const auto destination = reply_base + k_wow64_userconnect_reply_shared_info_offset;
            if (destination < reply_base)
            {
                return false;
            }

            return try_write_user_shared_info(memory, destination, process);
        }

        bool try_update_client_pfn_arrays_from_addresses(memory_interface& memory, process_context& process, const uint64_t apfn_client_a,
                                                         const uint64_t apfn_client_w, const uint64_t apfn_client_worker)
        {
            return try_copy_client_pfn_arrays(
                memory, process, client_pfn_arrays{.ansi = apfn_client_a, .wide = apfn_client_w, .worker = apfn_client_worker});
        }

        bool try_bootstrap_client_pfn_arrays_from_ntdll(windows_emulator& win_emu)
        {
            if (!win_emu.mod_manager.ntdll)
            {
                return false;
            }

            const auto retrieve_user_pfn = win_emu.mod_manager.ntdll->find_export("RtlRetrieveNtUserPfn");
            if (retrieve_user_pfn == 0)
            {
                return false;
            }

            std::vector<uint8_t> code_window(k_ntdll_probe_size);
            if (!win_emu.memory.try_read_memory(retrieve_user_pfn, code_window.data(), code_window.size()))
            {
                return false;
            }

            const auto pointers = scan_rip_relative_lea_references(code_window, retrieve_user_pfn, k_expected_pfn_pointer_count);
            if (pointers.size() != k_expected_pfn_pointer_count)
            {
                return false;
            }

            return try_update_client_pfn_arrays_from_addresses(win_emu.memory, win_emu.process, pointers[0], pointers[1], pointers[2]);
        }
    }

} // namespace sogen
