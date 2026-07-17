#pragma once
#include "std_include.hpp"
#include "windows_path.hpp"

namespace sogen
{

    class file_system
    {
      public:
        file_system(const std::filesystem::path& root)
            : root_(canonical(root))
        {
        }

        static bool is_escaping_relative_path(const std::filesystem::path& p)
        {
            return p.empty() || *p.begin() == "..";
        }

        static bool is_subpath(const std::filesystem::path& normal_root, const std::filesystem::path& normal_target)
        {
            // Lexical containment: structural check only, so it does not follow symlinks (unlike
            // std::filesystem::relative, which canonicalizes). Callers pass lexically-normalized
            // paths so that ".." escapes are still caught.
            const auto relative_path = normal_target.lexically_relative(normal_root);
            return !is_escaping_relative_path(relative_path);
        }

        std::set<char> list_drives() const
        {
            std::set<char> drives{};

#ifdef OS_WINDOWS
            if (this->root_.empty())
            {
                const auto drive_bits = GetLogicalDrives();

                for (char drive = 'a'; drive <= 'z'; ++drive)
                {
                    const auto drive_index = drive - 'a';
                    if (drive_bits & (1 << drive_index))
                    {
                        drives.insert(drive);
                    }
                }

                return drives;
            }
#endif

            std::error_code ec{};
            for (const auto& file : std::filesystem::directory_iterator(this->root_, ec))
            {
                const auto filename = file.path().filename().string();
                if (filename.size() == 1)
                {
                    drives.insert(utils::string::char_to_lower(filename.front()));
                }
            }

            return drives;
        }

        std::filesystem::path translate(const windows_path& win_path) const
        {
            if (!win_path.is_absolute())
            {
                throw std::runtime_error("Only absolute paths can be translated: " + win_path.string());
            }

            // Exact file mapping (fast path).
            if (const auto mapping = this->mappings_.find(win_path); mapping != this->mappings_.end())
            {
                return mapping->second;
            }

            // Directory mapping: a mapped ancestor directory mounts its whole subtree.
            if (auto mapped = this->translate_directory_mapping(win_path))
            {
                return std::move(*mapped);
            }

#ifdef OS_WINDOWS
            if (this->root_.empty())
            {
                return win_path.u16string();
            }
#endif

            // Emulation-root translation, confined to the drive root.
            const std::array<char, 2> root_drive{win_path.get_drive().value_or('c'), 0};
            return confine(this->root_ / root_drive.data(), this->root_ / win_path.to_portable_path());
        }

        template <typename F>
        void access_mapped_entries(const windows_path& win_path, const F& accessor) const
        {
            for (const auto& mapping : this->mappings_)
            {
                const auto& mapped_path = mapping.first;
                if (!mapped_path.empty() && mapped_path.parent() == win_path)
                {
                    accessor(mapping);
                }
            }
        }

        void map(windows_path src, std::filesystem::path dest)
        {
            this->mappings_[std::move(src)] = std::move(dest);
        }

      private:
        // Resolve a host path built from a guest-controlled path, but keep it inside `base`: a ".." in the
        // guest path that would escape `base` falls back to `base` itself. The check is purely lexical (it
        // does not follow symlinks), so a crafted path cannot escape. Host-side symlinks placed inside `base`
        // may still point elsewhere (e.g. a mounted game directory); the OS resolves those at open time.
        static std::filesystem::path confine(const std::filesystem::path& base, const std::filesystem::path& candidate)
        {
            if (is_subpath(base.lexically_normal(), candidate.lexically_normal()))
            {
                return weakly_canonical(candidate);
            }

            return base;
        }

        // If a mapped directory is an ancestor of `win_path`, resolve the remaining path against the mapped
        // host directory. The deepest matching mount wins, so nested mounts behave intuitively.
        std::optional<std::filesystem::path> translate_directory_mapping(const windows_path& win_path) const
        {
            const std::filesystem::path* best_dest = nullptr;
            windows_path best_remainder{};
            size_t best_depth = 0;

            for (const auto& [src, dest] : this->mappings_)
            {
                auto remainder = win_path.relative_to(src);
                if (remainder.has_value() && (best_dest == nullptr || src.depth() > best_depth))
                {
                    best_dest = &dest;
                    best_remainder = std::move(*remainder);
                    best_depth = src.depth();
                }
            }

            if (best_dest == nullptr)
            {
                return std::nullopt;
            }

            return confine(*best_dest, *best_dest / best_remainder.to_portable_path());
        }

        std::filesystem::path root_{};
        std::unordered_map<windows_path, std::filesystem::path> mappings_{};
    };

} // namespace sogen
