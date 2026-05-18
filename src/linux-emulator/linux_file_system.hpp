#pragma once

#include "std_include.hpp"

class linux_file_system
{
  public:
    linux_file_system() = default;

    explicit linux_file_system(std::filesystem::path root)
        : root_(std::move(root))
    {
    }

    std::filesystem::path translate(const std::string_view guest_path) const
    {
        if (guest_path.empty())
        {
            return this->root_;
        }

        // Handle special paths that should not be translated
        if (is_dev_null(guest_path))
        {
            return "/dev/null";
        }

        if (is_dev_zero(guest_path))
        {
            return "/dev/zero";
        }

        if (is_dev_urandom(guest_path))
        {
            return "/dev/urandom";
        }

        // /proc/self/* paths are handled by the procfs emulation layer,
        // not by the file system translator. Return the path as-is so
        // upper layers can detect and intercept it.
        if (is_proc_self(guest_path))
        {
            return std::filesystem::path{guest_path};
        }

        if (this->root_.empty())
        {
            return std::filesystem::path{guest_path}.lexically_normal();
        }

        // If the guest path is already a host path under one of our explicit
        // passthrough prefixes, allow it as-is.
        if (!guest_path.empty() && guest_path[0] == '/')
        {
            const auto host_path = std::filesystem::path{guest_path}.lexically_normal();

            for (const auto& prefix : this->passthrough_prefixes_)
            {
                if (is_within(host_path, prefix))
                {
                    return host_path;
                }
            }

            // If this is already inside the emulation root, avoid double-prefixing
            // (e.g. root='/foo', path='/foo/bar' should remain '/foo/bar').
            if (is_within(host_path, this->root_))
            {
                return host_path;
            }

            return this->translate_relative_to(this->root_, guest_path.substr(1));
        }

        // Relative path: prefer emulation-root path by default.
        // If the target does not exist there, allow lookup under passthrough
        // prefixes (useful for executable-adjacent host files).
        auto rooted = this->translate_relative_to(this->root_, guest_path);
        if (std::filesystem::exists(rooted))
        {
            return rooted;
        }

        for (const auto& prefix : this->passthrough_prefixes_)
        {
            auto candidate = this->translate_relative_to(prefix, guest_path);
            if (std::filesystem::exists(candidate))
            {
                return candidate;
            }
        }

        return rooted;
    }

    std::filesystem::path translate_relative_to(const std::filesystem::path& base, const std::string_view guest_path) const
    {
        auto normalized_base = base.lexically_normal();
        auto candidate = (normalized_base / std::filesystem::path{guest_path}).lexically_normal();

        if (this->root_.empty())
        {
            return candidate;
        }

        if (is_within(normalized_base, this->root_))
        {
            auto relative = candidate.lexically_relative(this->root_);
            if (relative.empty() || relative == "." || *relative.begin() == "..")
            {
                auto normalized_guest = (std::filesystem::path{"/"} / std::filesystem::path{guest_path}).lexically_normal();
                relative = normalized_guest.lexically_relative("/");
            }

            return (this->root_ / relative).lexically_normal();
        }

        for (const auto& prefix : this->passthrough_prefixes_)
        {
            if (is_within(normalized_base, prefix))
            {
                auto relative = candidate.lexically_relative(prefix);
                if (!relative.empty() && relative != "." && *relative.begin() != "..")
                {
                    return (prefix / relative).lexically_normal();
                }

                return normalized_base;
            }
        }

        return candidate;
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

    static bool is_special_path(const std::string_view path)
    {
        return is_proc_self(path) || is_dev_null(path) || is_dev_zero(path) || is_dev_urandom(path);
    }

    const std::filesystem::path& root() const
    {
        return this->root_;
    }

  private:
    std::filesystem::path root_{};
    std::vector<std::filesystem::path> passthrough_prefixes_{};

    static bool is_within(const std::filesystem::path& path, const std::filesystem::path& prefix)
    {
        auto normalized_path = path.lexically_normal();
        auto normalized_prefix = prefix.lexically_normal();

        auto path_it = normalized_path.begin();
        auto prefix_it = normalized_prefix.begin();
        for (; prefix_it != normalized_prefix.end(); ++prefix_it, ++path_it)
        {
            if (path_it == normalized_path.end() || *path_it != *prefix_it)
            {
                return false;
            }
        }

        return true;
    }

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
