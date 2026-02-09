#pragma once

#include "std_include.hpp"

class linux_file_system
{
  public:
    linux_file_system() = default;

    explicit linux_file_system(const std::filesystem::path& root)
        : root_(root)
    {
    }

    std::filesystem::path translate(const std::string_view guest_path) const
    {
        if (guest_path.empty())
        {
            return this->root_;
        }

        // Handle special paths that should not be translated
        if (this->is_dev_null(guest_path))
        {
            return "/dev/null";
        }

        if (this->is_dev_zero(guest_path))
        {
            return "/dev/zero";
        }

        if (this->is_dev_urandom(guest_path))
        {
            return "/dev/urandom";
        }

        // /proc/self/* paths are handled by the procfs emulation layer,
        // not by the file system translator. Return the path as-is so
        // upper layers can detect and intercept it.
        if (this->is_proc_self(guest_path))
        {
            return std::filesystem::path(guest_path);
        }

        if (this->root_.empty())
        {
            return std::filesystem::path(guest_path);
        }

        // Translate absolute paths by prepending the emulation root
        if (!guest_path.empty() && guest_path[0] == '/')
        {
            return this->root_ / guest_path.substr(1);
        }

        return this->root_ / guest_path;
    }

    bool is_special_path(const std::string_view path) const
    {
        return this->is_proc_self(path) || this->is_dev_null(path) || this->is_dev_zero(path) || this->is_dev_urandom(path);
    }

    const std::filesystem::path& root() const
    {
        return this->root_;
    }

  private:
    std::filesystem::path root_{};

    static bool is_proc_self(const std::string_view path)
    {
        return path.starts_with("/proc/self/") || path == "/proc/self";
    }

    static bool is_dev_null(const std::string_view path)
    {
        return path == "/dev/null";
    }

    static bool is_dev_zero(const std::string_view path)
    {
        return path == "/dev/zero";
    }

    static bool is_dev_urandom(const std::string_view path)
    {
        return path == "/dev/urandom" || path == "/dev/random";
    }
};
