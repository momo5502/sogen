#pragma once
#include <filesystem>

class windows_emulator;

namespace minidump_loader
{
    void load_minidump_into_emulator(windows_emulator& win_emu, const std::filesystem::path& minidump_path);
}
