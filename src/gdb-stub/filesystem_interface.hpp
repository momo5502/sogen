#pragma once

#include <string>
#include <cstdint>

namespace gdb_stub
{
    class filesystem_interface
    {
      public:
        virtual ~filesystem_interface() = default;
        virtual uint32_t open(const std::string& file_path, uint32_t flags, uint32_t mode) = 0;
        virtual void close(uint32_t fd) = 0;
        virtual std::string read(uint32_t fd, size_t count, uint64_t offset) = 0;
        virtual size_t write(uint32_t fd, uint64_t offset, const void* data, size_t length) = 0;
        virtual std::string fstat(uint32_t fd) = 0;
        virtual int unlink(const std::string& file_path) = 0;
    };
}
