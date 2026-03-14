#pragma once

#include <fstream>
#include <string>
#include <cstdint>
#include <unordered_map>

namespace gdb_stub
{
    class gdb_filesystem final
    {
      public:
        gdb_filesystem() = default;
        ~gdb_filesystem() = default;

        gdb_filesystem(gdb_filesystem&&) = delete;
        gdb_filesystem(const gdb_filesystem&) = delete;

        gdb_filesystem& operator=(gdb_filesystem&&) = delete;
        gdb_filesystem& operator=(const gdb_filesystem&) = delete;

        uint32_t open(const std::string& file_path, uint32_t flags, uint32_t mode);
        void close(uint32_t fd);
        std::string read(uint32_t fd, size_t count, uint64_t offset);
        size_t write(uint32_t fd, uint64_t offset, const void* data, size_t length);
        std::string fstat(uint32_t fd);
        int unlink(const std::string& file_path);

      private:
        std::unordered_map<unsigned, std::fstream> opened_files;
        uint32_t free_index{1};
    };
}
