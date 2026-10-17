#pragma once
#include <filesystem>

#include <platform/win_pefile.hpp>

namespace sogen
{

    class windows_emulator;

    namespace minidump_loader
    {
        // The guest machine a minidump was taken on (AMD64 or ARM64), from its system info stream.
        PEMachineType get_minidump_machine(const std::filesystem::path& minidump_path);

        void load_minidump_into_emulator(windows_emulator& win_emu, const std::filesystem::path& minidump_path);
    }

} // namespace sogen
