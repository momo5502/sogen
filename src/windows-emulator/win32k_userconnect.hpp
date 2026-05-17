#pragma once

#include "common/platform/process.hpp"
#include "common/platform/user.hpp"
#include "emulator_utils.hpp"

struct process_context;
class windows_emulator;

namespace win32k_userconnect
{
    constexpr uint32_t k_user_server_dll_index = 3;
    constexpr ULONG k_wow64_wndmsg_count = 0x2000;
    constexpr ULONG k_wow64_ime_msg_count = 0x2000;
    constexpr ULONG k_wow64_userconnect_header_size = 0x8;
    constexpr ULONG k_wow64_userconnect_reply_shared_info_offset = sizeof(uint64_t);

    NTSTATUS narrow_wow64_address(uint64_t address, uint32_t& narrowed);
    NTSTATUS resolve_wow64_destination(uint64_t user_connect_ptr, uint64_t user_connect_length, uint32_t& destination);
    NTSTATUS build_wow64_userconnect(const process_context& process, WIN32K_USERCONNECT32& connect);
    bool try_write_wow64_userconnect(memory_interface& memory, uint64_t destination, const WIN32K_USERCONNECT32& connect);
    void populate_user_shared_info(USER_SHAREDINFO& shared, const process_context& process);
    bool try_write_user_shared_info(memory_interface& memory, uint64_t destination, const process_context& process);
    bool try_write_api_port_userconnect_reply(memory_interface& memory, uint64_t reply_base, const process_context& process);
    bool try_update_client_pfn_arrays_from_addresses(memory_interface& memory, process_context& process, uint64_t apfn_client_a,
                                                     uint64_t apfn_client_w, uint64_t apfn_client_worker);
    bool try_bootstrap_client_pfn_arrays_from_ntdll(windows_emulator& win_emu);
}
