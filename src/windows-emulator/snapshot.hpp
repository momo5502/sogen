#pragma once

#include "windows_emulator.hpp"

namespace sogen
{

    namespace snapshot
    {
        // The configuration is the launcher's description of the host setup the process depends on (emulation root, mounts,
        // registry, network). Loading a snapshot under a different one fails instead of reopening files elsewhere.
        std::vector<std::byte> create_emulator_snapshot(windows_emulator& win_emu, std::string_view configuration = {});
        std::filesystem::path write_emulator_snapshot(windows_emulator& win_emu, bool log = true);
        void write_emulator_snapshot(windows_emulator& win_emu, const std::filesystem::path& snapshot_file,
                                     std::string_view configuration = {});

        PEMachineType get_snapshot_machine(const std::filesystem::path& snapshot_file);

        void load_emulator_snapshot(windows_emulator& win_emu, std::span<const std::byte> snapshot, std::string_view configuration = {});
        void load_emulator_snapshot(windows_emulator& win_emu, const std::filesystem::path& snapshot_file,
                                    std::string_view configuration = {});
    }

} // namespace sogen
