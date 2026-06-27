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

            const auto mapping = this->mappings_.find(win_path);
            if (mapping != this->mappings_.end())
            {
                return mapping->second;
            }

#ifdef OS_WINDOWS
            if (this->root_.empty())
            {
                return win_path.u16string();
            }
#endif

            const std::array<char, 2> root_drive{win_path.get_drive().value_or('c'), 0};
            auto root = this->root_ / root_drive.data();

            auto path = this->root_ / win_path.to_portable_path();
            // Confine the guest-controlled path to the drive root by resolving "." and ".."
            // lexically, without following symlinks, so a crafted path cannot escape. Host-side
            // symlinks placed inside the root may still point elsewhere (e.g. a mounted game
            // directory); the OS resolves them when the file is opened.
            if (is_subpath(root, path.lexically_normal()))
            {
                return weakly_canonical(path);
            }

            return root;
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
        std::filesystem::path root_{};
        std::unordered_map<windows_path, std::filesystem::path> mappings_{};
    };

} // namespace sogen
