#pragma once

#include <memory>
#include <string>
#include <unordered_map>

namespace gdb_stub
{
    class gdb_fs final
    {
      public:
        gdb_fs() = default;
        ~gdb_fs() = default;

        gdb_fs(gdb_fs&&) = delete;
        gdb_fs(const gdb_fs&) = delete;

        gdb_fs& operator=(gdb_fs&&) = delete;
        gdb_fs& operator=(const gdb_fs&) = delete;

        unsigned open(const std::string& file_path, uint32_t flags, uint32_t mode);
        void close(unsigned fd);
        std::string read(unsigned fd, size_t count, uint64_t offset);
        size_t write(unsigned fd, uint64_t offset, const void* data, size_t length);
        std::string fstat(unsigned fd);
        int unlink(const std::string& file_path);

      private:
        struct file_deleter
        {
            void operator()(std::FILE* fp) const noexcept
            {
                if (fp)
                {
                    std::fclose(fp);
                }
            }
        };

        using auto_file_t = std::unique_ptr<std::FILE, file_deleter>;

        std::unordered_map<unsigned, auto_file_t> opened_files;
        unsigned free_index{1};
    };
}
