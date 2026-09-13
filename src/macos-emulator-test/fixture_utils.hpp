#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include <utils/io.hpp>

namespace sogen::test
{
    // A clock reading alone is not unique: the suites run under gtest_parallel, so several *processes*
    // sample it concurrently and two can land on the same value, after which one test's scratch file is
    // another's and whichever finishes first deletes it. The pid separates the processes and the counter
    // separates fixtures within one.
    inline std::string unique_temp_name(const std::string_view prefix)
    {
        static std::atomic<uint64_t> counter{0};

#ifdef _WIN32
        const auto pid = static_cast<uint64_t>(_getpid());
#else
        const auto pid = static_cast<uint64_t>(getpid());
#endif

        return std::string{prefix} + "-" + std::to_string(pid) + "-" + std::to_string(counter.fetch_add(1)) + "-" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    }

    class temp_directory
    {
      public:
        explicit temp_directory(const std::string_view name)
            : path_(std::filesystem::temp_directory_path() / unique_temp_name("sogen-macos-" + std::string{name}))
        {
            std::filesystem::create_directories(this->path_);
        }

        ~temp_directory()
        {
            std::error_code error{};
            std::filesystem::remove_all(this->path_, error);
        }

        temp_directory(const temp_directory&) = delete;
        temp_directory& operator=(const temp_directory&) = delete;
        temp_directory(temp_directory&&) = delete;
        temp_directory& operator=(temp_directory&&) = delete;

        const std::filesystem::path& path() const
        {
            return this->path_;
        }

      private:
        std::filesystem::path path_;
    };

    inline std::filesystem::path fixture_path(const std::string_view name)
    {
        return std::filesystem::path{MACOS_FIXTURE_ROOT} / name;
    }

    inline std::vector<std::byte> read_fixture(const std::string_view name)
    {
        const auto path = fixture_path(name);

        std::vector<std::byte> data{};
        if (!utils::io::read_file(path, &data))
        {
            throw std::runtime_error("Missing Mach-O fixture: " + path.string());
        }

        return data;
    }
}
