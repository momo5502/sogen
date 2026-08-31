#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace sogen
{

    class guest_file_system
    {
      public:
        guest_file_system() = default;

        explicit guest_file_system(std::filesystem::path root)
            : root_(std::move(root)),
              canonical_root_(canonical_host_path(this->root_))
        {
        }

        std::filesystem::path translate(const std::string_view guest_path) const
        {
            if (guest_path.empty())
            {
                return this->root_;
            }

            const auto normalized_guest_string = normalize_guest_path_string(guest_path);
            auto normalized_guest = std::filesystem::path{normalized_guest_string};

            if (is_dev_null(normalized_guest_string))
            {
                return "/dev/null";
            }

            if (is_dev_zero(normalized_guest_string))
            {
                return "/dev/zero";
            }

            if (is_dev_urandom(normalized_guest_string))
            {
                return "/dev/urandom";
            }

            if (auto mapped = this->translate_mapped_path(normalized_guest))
            {
                return std::move(*mapped);
            }

            if (this->root_.empty())
            {
                return normalized_guest;
            }

            if (!normalized_guest_string.empty() && normalized_guest_string.front() == '/')
            {
                for (const auto& prefix : this->passthrough_prefixes_)
                {
                    if (is_within(normalized_guest, prefix))
                    {
                        return normalized_guest;
                    }
                }

                if (is_within(normalized_guest, this->root_))
                {
                    return normalized_guest;
                }

                const auto relative = normalized_guest_string == "/" ? std::string{} : normalized_guest_string.substr(1);
                return this->translate_relative_to(this->root_, relative);
            }

            return this->translate_relative_to(this->root_, normalized_guest_string);
        }

        std::filesystem::path translate_guest_relative_to(const std::string_view cwd, const std::string_view guest_path) const
        {
            if (guest_path.empty() || guest_path.front() == '/' || guest_path.front() == '\\')
            {
                return this->translate(guest_path);
            }

            const auto absolute_guest = resolve_guest_path_string(cwd, guest_path);
            return this->translate(absolute_guest);
        }

        std::filesystem::path translate_symlink_target(const std::string_view target_guest,
                                                       const std::string_view resolved_link_guest) const
        {
            if (target_guest.empty())
            {
                return {};
            }

            const auto link_parent_path = normalize_guest_path(resolved_link_guest).parent_path();
            const auto link_parent = link_parent_path.empty() ? std::string{"/"} : link_parent_path.string();
            const auto resolved_target = resolve_guest_path_string(link_parent, target_guest);
            auto target_host = this->translate(resolved_target).lexically_normal();
            const auto link_parent_host = this->translate(link_parent).lexically_normal();
            auto relative = target_host.lexically_relative(link_parent_host);
            if (relative.empty())
            {
                return target_host;
            }
            return relative;
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
                    const auto normalized_guest = normalize_guest_path_string(guest_path);
                    relative = std::filesystem::path{normalized_guest == "/" ? std::string{} : normalized_guest.substr(1)};
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
                normalized = std::filesystem::absolute(normalized).lexically_normal();
            }

            for (const auto& existing : this->passthrough_prefixes_)
            {
                if (existing == normalized)
                {
                    return;
                }
            }

            this->passthrough_prefixes_.push_back(std::move(normalized));
        }

        void add_path_mapping(const std::filesystem::path& guest, const std::filesystem::path& host, const bool read_only)
        {
            auto normalized_guest = normalize_guest_path(guest.string());
            auto normalized_host = host.lexically_normal();
            if (!normalized_host.is_absolute())
            {
                normalized_host = std::filesystem::absolute(normalized_host).lexically_normal();
            }

            auto canonical_host = canonical_host_path(normalized_host);

            for (auto& mapping : this->path_mappings_)
            {
                if (mapping.guest_root == normalized_guest)
                {
                    mapping.host_root = std::move(normalized_host);
                    mapping.canonical_host_root = std::move(canonical_host);
                    mapping.read_only = read_only;
                    return;
                }
            }

            this->path_mappings_.push_back({.guest_root = std::move(normalized_guest),
                                            .host_root = std::move(normalized_host),
                                            .canonical_host_root = std::move(canonical_host),
                                            .read_only = read_only});
        }

        bool is_read_only_guest_path(const std::string_view guest_path) const
        {
            const auto normalized_guest_string = normalize_guest_path_string(guest_path);
            if (is_special_path(normalized_guest_string))
            {
                return false;
            }

            const auto* mapping = this->find_mapping(std::filesystem::path{normalized_guest_string});
            if (mapping != nullptr && mapping->read_only)
            {
                return true;
            }

            const auto host_path = this->translate(guest_path);
            if (this->is_read_only_host_path(host_path))
            {
                return true;
            }

            if (mapping != nullptr)
            {
                return false;
            }

            return this->escapes_root(host_path);
        }

        static bool is_special_path(const std::string_view path)
        {
            return is_proc_self(path) || is_dev_null(path) || is_dev_zero(path) || is_dev_urandom(path);
        }

        static std::filesystem::path normalize_guest_path(const std::string_view guest_path)
        {
            return std::filesystem::path{normalize_guest_path_string(guest_path)};
        }

        static std::string normalize_guest_path_string(const std::string_view guest_path)
        {
            std::vector<std::string> components{};
            std::string path{};
            path.reserve(guest_path.size() + 1);
            for (const char ch : guest_path)
            {
                path.push_back(ch == '\\' ? '/' : ch);
            }

            size_t offset = 0;
            while (offset <= path.size())
            {
                const auto next = path.find('/', offset);
                const auto end = next == std::string::npos ? path.size() : next;
                const auto component = std::string_view{path}.substr(offset, end - offset);
                if (component == "..")
                {
                    if (!components.empty())
                    {
                        components.pop_back();
                    }
                }
                else if (!component.empty() && component != ".")
                {
                    components.emplace_back(component);
                }

                if (next == std::string::npos)
                {
                    break;
                }
                offset = next + 1;
            }

            std::string normalized{"/"};
            for (const auto& component : components)
            {
                if (normalized.size() > 1)
                {
                    normalized.push_back('/');
                }
                normalized += component;
            }

            return normalized;
        }

        static std::string resolve_guest_path_string(const std::string_view cwd, const std::string_view guest_path)
        {
            if (guest_path.empty() || guest_path.front() == '/' || guest_path.front() == '\\')
            {
                return normalize_guest_path_string(guest_path);
            }

            auto absolute = normalize_guest_path_string(cwd);
            if (absolute != "/")
            {
                absolute.push_back('/');
            }
            absolute.append(guest_path);
            return normalize_guest_path_string(absolute);
        }

        const std::filesystem::path& root() const
        {
            return this->root_;
        }

      private:
        struct path_mapping
        {
            std::filesystem::path guest_root{};
            std::filesystem::path host_root{};
            std::filesystem::path canonical_host_root{};
            bool read_only{};
        };

        std::filesystem::path root_{};
        std::filesystem::path canonical_root_{};
        std::vector<std::filesystem::path> passthrough_prefixes_{};
        std::vector<path_mapping> path_mappings_{};

        // Emulation roots are staged out of symlinks into the host system -- src/tools/make-macos-root.sh
        // links /System/Library entry by entry, plus /Library and /Applications -- so clamping the guest
        // path into the root does not keep the host open() inside it: the kernel follows the link outward
        // and lands on the analyst's real file. Reads through those links are the point of a linked root,
        // but a write is not, so mutation is refused for anything whose real location is outside the root.
        // An explicit writable mapping is the analyst handing a directory over on purpose and still wins.
        bool escapes_root(const std::filesystem::path& host_path) const
        {
            if (this->root_.empty())
            {
                return false;
            }

            return !is_within(canonical_host_path(host_path), this->canonical_root_);
        }

        const path_mapping* find_mapping(const std::filesystem::path& normalized_guest) const
        {
            const path_mapping* best{};
            size_t best_depth{};
            for (const auto& mapping : this->path_mappings_)
            {
                if (!is_within(normalized_guest, mapping.guest_root))
                {
                    continue;
                }

                const auto depth = static_cast<size_t>(std::distance(mapping.guest_root.begin(), mapping.guest_root.end()));
                if (!best || depth > best_depth)
                {
                    best = &mapping;
                    best_depth = depth;
                }
            }

            return best;
        }

        // A guest path that no mapping claims can still land on a read-only mapping's host tree: a
        // passthrough prefix and a root-less file system both hand the guest path back as a host path,
        // so the executable's own host path reaches the bundle behind the mapping's back. Enforcement
        // has to follow where a path resolves to, not only how it was spelled.
        bool is_read_only_host_path(const std::filesystem::path& host_path) const
        {
            const auto normalized_host = host_path.lexically_normal();
            std::optional<std::filesystem::path> canonical_host{};

            for (const auto& mapping : this->path_mappings_)
            {
                if (!mapping.read_only)
                {
                    continue;
                }

                if (is_within(normalized_host, mapping.host_root))
                {
                    return true;
                }

                // The host roots analysts hand over live under /tmp and $TMPDIR, which are symlinks into
                // /private on macOS, so the lexical test alone misses every alias of the same directory.
                if (!canonical_host)
                {
                    canonical_host = canonical_host_path(normalized_host);
                }

                if (is_within(*canonical_host, mapping.canonical_host_root))
                {
                    return true;
                }
            }

            return false;
        }

        static std::filesystem::path canonical_host_path(const std::filesystem::path& path)
        {
            std::error_code error{};
            auto canonical = std::filesystem::weakly_canonical(path, error);
            if (error || canonical.empty())
            {
                return path.lexically_normal();
            }

            return canonical;
        }

        std::optional<std::filesystem::path> translate_mapped_path(const std::filesystem::path& normalized_guest) const
        {
            const auto* mapping = this->find_mapping(normalized_guest);
            if (!mapping)
            {
                return std::nullopt;
            }

            auto relative = normalized_guest.lexically_relative(mapping->guest_root);
            if (relative == ".")
            {
                relative.clear();
            }
            if (!relative.empty() && *relative.begin() == "..")
            {
                return std::nullopt;
            }

            auto host_path = (mapping->host_root / relative).lexically_normal();
            if (!is_within(host_path, mapping->host_root))
            {
                return std::nullopt;
            }

            return host_path;
        }

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

} // namespace sogen
