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

        // If the guest path is already a host path under one of our explicit
        // passthrough prefixes, allow it as-is.
        if (!guest_path.empty() && guest_path[0] == '/')
        {
            const std::filesystem::path host_path(guest_path);

            for (const auto& prefix : this->passthrough_prefixes_)
            {
                if (host_path == prefix)
                {
                    return host_path;
                }

                const auto prefix_str = prefix.string();
                const auto host_str = host_path.string();
                if (!prefix_str.empty() && host_str.size() > prefix_str.size() && host_str.rfind(prefix_str, 0) == 0 &&
                    host_str[prefix_str.size()] == '/')
                {
                    return host_path;
                }
            }

            // If this is already inside the emulation root, avoid double-prefixing
            // (e.g. root='/foo', path='/foo/bar' should remain '/foo/bar').
            const auto root_str = this->root_.string();
            const auto host_str = host_path.string();
            if (!root_str.empty() && host_str.rfind(root_str, 0) == 0)
            {
                if (host_str.size() == root_str.size() || host_str[root_str.size()] == '/')
                {
                    return host_path;
                }
            }
        }

        // Translate absolute paths by prepending the emulation root
        if (!guest_path.empty() && guest_path[0] == '/')
        {
            return this->root_ / guest_path.substr(1);
        }

        return this->root_ / guest_path;
    }

    void add_passthrough_prefix(const std::filesystem::path& prefix)
    {
        if (prefix.empty())
        {
            return;
        }

        auto normalized = prefix.lexically_normal();
        if (!normalized.is_absolute())
        {
            normalized = std::filesystem::absolute(normalized);
        }

        // Deduplicate
        for (const auto& existing : this->passthrough_prefixes_)
        {
            if (existing == normalized)
            {
                return;
            }
        }

        this->passthrough_prefixes_.push_back(std::move(normalized));
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
    std::vector<std::filesystem::path> passthrough_prefixes_{};

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
