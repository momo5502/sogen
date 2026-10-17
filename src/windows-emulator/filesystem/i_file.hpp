#pragma once

#include "../std_include.hpp"

#include <utils/stat.hpp>

namespace sogen
{

    class i_file
    {
      public:
        virtual ~i_file() = default;

        virtual size_t read(std::span<std::byte> buffer) = 0;
        virtual size_t write(std::span<const std::byte> buffer) = 0;
        virtual bool flush() = 0;

        virtual bool seek(int64_t offset, int origin) = 0;
        virtual int64_t tell() = 0;
        virtual int64_t size() = 0;
        virtual bool resize(uint64_t size) = 0;

        virtual std::optional<struct compat_stat> stat() = 0;
    };

} // namespace sogen
