#pragma once

#include "std_include.hpp"

#include <filesystem>
#include <optional>

namespace sogen
{
    size_t host_mapping_granularity();

    class host_file_mapping
    {
      public:
        host_file_mapping() = default;
        ~host_file_mapping();

        host_file_mapping(const host_file_mapping&) = delete;
        host_file_mapping& operator=(const host_file_mapping&) = delete;
        host_file_mapping(host_file_mapping&& other) noexcept;
        host_file_mapping& operator=(host_file_mapping&& other) noexcept;

        // file_offset must be a multiple of host_mapping_granularity(), as POSIX mmap and MapViewOfFile
        // both require. An unaligned request is refused rather than quietly shifted to the page below:
        // sys_mmap forwards the guest's own offset, and a guest asking for one is making an error.
        static std::optional<host_file_mapping> create(const std::filesystem::path& path, uint64_t file_offset, size_t length);

        void* data() const
        {
            return this->base_;
        }

        size_t size() const
        {
            return this->length_;
        }

        explicit operator bool() const
        {
            return this->base_ != nullptr;
        }

      private:
        void release();

        void* base_{};
        size_t base_length_{};
        size_t length_{};

#ifdef _WIN32
        void* file_handle_{};
        void* mapping_handle_{};
#endif
    };
}
