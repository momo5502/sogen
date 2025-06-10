#pragma once
#include <filesystem>

class windows_emulator;

class minidump_loader
{
  public:
    minidump_loader(windows_emulator& win_emu, const std::filesystem::path& minidump_path);
    ~minidump_loader();

    void load_into_emulator();

  private:
    windows_emulator& win_emu_;
    std::filesystem::path minidump_path_;
};