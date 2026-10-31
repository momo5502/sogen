#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <system_error>

namespace sogen
{
    struct macos_file
    {
        std::string guest_path{};
        std::filesystem::path host_path{};
        std::ifstream stream{};
        uint64_t offset{};
        bool is_socket{};

        size_t read_at(const uint64_t position, std::span<std::byte> buffer)
        {
            this->stream.clear();
            this->stream.seekg(static_cast<std::streamoff>(position));
            this->stream.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
            return static_cast<size_t>(this->stream.gcount());
        }

        uint64_t size() const
        {
            std::error_code ec{};
            const auto size = std::filesystem::file_size(this->host_path, ec);
            return ec ? 0 : size;
        }
    };

    // Descriptors 0-2 are the host's standard streams and are not tracked here.
    class macos_fd_table
    {
      public:
        std::optional<int> open_for_reading(std::string guest_path, const std::filesystem::path& host_path)
        {
            macos_file file{.guest_path = std::move(guest_path), .host_path = host_path};
            file.stream.open(host_path, std::ios::binary);
            if (!file.stream)
            {
                return std::nullopt;
            }

            const auto fd = this->lowest_free_descriptor();
            this->files_.emplace(fd, std::move(file));
            return fd;
        }

        int open_socket()
        {
            const auto fd = this->lowest_free_descriptor();
            this->files_.emplace(fd, macos_file{.is_socket = true});
            return fd;
        }

        macos_file* get(const int fd)
        {
            const auto it = this->files_.find(fd);
            return it == this->files_.end() ? nullptr : &it->second;
        }

        bool close(const int fd)
        {
            return this->files_.erase(fd) != 0;
        }

      private:
        static constexpr int FIRST_DESCRIPTOR = 3;

        std::map<int, macos_file> files_{};

        int lowest_free_descriptor() const
        {
            auto fd = FIRST_DESCRIPTOR;
            for (const auto& [used, _] : this->files_)
            {
                if (used != fd)
                {
                    break;
                }

                ++fd;
            }

            return fd;
        }
    };
}
