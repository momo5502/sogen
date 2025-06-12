#pragma once
#include <filesystem>

class windows_emulator;

void load_minidump_into_emulator(windows_emulator& win_emu, std::filesystem::path minidump_path);