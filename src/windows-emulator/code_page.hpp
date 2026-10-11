#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace sogen
{
    class windows_emulator;

    struct ansi_oem_tables
    {
        std::array<uint8_t, 256> ansi_to_oem{};
        std::array<uint8_t, 256> oem_to_ansi{};
    };

    std::optional<ansi_oem_tables> build_ansi_oem_tables(std::span<const std::byte> ansi, std::span<const std::byte> oem);
    void initialize_user_code_page_tables(windows_emulator& win_emu);
}
