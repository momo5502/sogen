#pragma once

#include <gdb_fs.hpp>
#include <windows_emulator.hpp>

#include <fstream>
#include <cstdint>
#include <unordered_map>

class windows_filesystem final : public gdb_stub::filesystem_interface
{
  public:
    windows_filesystem(windows_emulator& win_emu)
        : win_emu_(&win_emu)
    {
    }

    windows_filesystem(windows_filesystem&&) = delete;
    windows_filesystem(const windows_filesystem&) = delete;

    windows_filesystem& operator=(windows_filesystem&&) = delete;
    windows_filesystem& operator=(const windows_filesystem&) = delete;

    uint32_t open(const std::string& file_path, uint32_t flags, uint32_t mode) override;
    void close(uint32_t fd) override;
    std::string read(uint32_t fd, size_t count, uint64_t offset) override;
    size_t write(uint32_t fd, uint64_t offset, const void* data, size_t length) override;
    std::string fstat(uint32_t fd) override;
    int unlink(const std::string& file_path) override;

  private:
    std::string translate_path(std::string_view emulated_path) const;

    windows_emulator* win_emu_{};
    std::unordered_map<unsigned, std::fstream> opened_files;
    uint32_t free_index{1};
};
