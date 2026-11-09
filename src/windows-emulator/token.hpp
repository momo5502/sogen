#pragma once

#include "std_include.hpp"
#include "handles.hpp"

namespace sogen
{
    class windows_emulator;

    // S-1-5-18, NT AUTHORITY\SYSTEM
    constexpr std::array<uint8_t, 12> local_system_sid{0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x12, 0x00, 0x00, 0x00};

    // S-1-5-2, NT AUTHORITY\NETWORK
    constexpr std::array<uint8_t, 12> network_sid{0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x02, 0x00, 0x00, 0x00};

    // S-1-5-32-544, BUILTIN\Administrators
    constexpr std::array<uint8_t, 16> administrators_sid{0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05,
                                                         0x20, 0x00, 0x00, 0x00, 0x20, 0x02, 0x00, 0x00};

    // The session NtQueryInformationToken reports for every token.
    constexpr ULONG token_session_id = 1;

    // Whether an access check matches the SID against one of the tokens NtAccessCheck takes: the process token and the
    // impersonation tokens hold the user and its enabled groups, the anonymous token only the anonymous SID.
    bool token_holds_sid(const windows_emulator& win_emu, handle token, std::span<const uint8_t> sid);

    // SeAccessCheck's walk of a self-relative descriptor's DACL for that token. It takes the ACE masks as stored, without
    // generic mapping, and skips inherit-only ACEs. A descriptor without a DACL grants everything.
    bool token_grants_access(const windows_emulator& win_emu, handle token, std::span<const std::byte> descriptor,
                             ACCESS_MASK desired_access);
}
