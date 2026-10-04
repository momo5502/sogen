#pragma once

#include "std_include.hpp"

#include <span>
#include <string>

namespace sogen
{
    // Reads slices of a host file without mapping it. The shared cache is 5.4 GB and a browser cannot map
    // it at all, so the pager asks for the pages it needs and nothing else; an interface rather than a
    // function because the browser supplies the bytes through a JS bridge instead of the filesystem.
    class host_range_reader
    {
      public:
        host_range_reader() = default;
        virtual ~host_range_reader() = default;

        host_range_reader(const host_range_reader&) = delete;
        host_range_reader& operator=(const host_range_reader&) = delete;
        host_range_reader(host_range_reader&&) = delete;
        host_range_reader& operator=(host_range_reader&&) = delete;

        // 0 for anything that cannot be sized, including a missing file.
        virtual uint64_t file_size(const std::string& path) = 0;

        // Returns how many bytes were placed in destination, which is short at the end of the file and 0
        // past it. Never partially fills without saying so: the caller cannot tell a zero page from a
        // failed read otherwise.
        virtual size_t read(const std::string& path, uint64_t offset, std::span<std::byte> destination) = 0;
    };

    host_range_reader& default_host_range_reader();
}
