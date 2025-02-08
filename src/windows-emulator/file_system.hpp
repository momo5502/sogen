#pragma once
#include "std_include.hpp"
#include "windows_path.hpp"

#include <platform/compiler.hpp>

class file_system
{
  public:
    file_system(std::filesystem::path root)
        : root_(std::move(root))
    {
    }

    std::filesystem::path translate(const windows_path& win_path) const
    {
        if (!win_path.is_absolute())
        {
            throw std::runtime_error("Only absolute paths can be translated!");
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

        // TODO: Sanitize path to prevent traversal!
        return this->root_ / win_path.to_portable_path();
    }

    windows_path local_to_windows_path(const std::filesystem::path& local_path) const
    {
        const auto absolute_local_path = absolute(local_path);
        const auto relative_path = relative(absolute_local_path, this->root_);

        if (relative_path.empty() || *relative_path.begin() == "..")
        {
            throw std::runtime_error("Path '" + local_path.string() + "' is not within the root filesystem!");
        }

        char drive{};
        std::list<std::u16string> folders{};

        for (auto i = relative_path.begin(); i != relative_path.end(); ++i)
        {
            if (i == relative_path.begin())
            {
                const auto str = i->string();
                assert(str.size() == 1);
                drive = str[0];
            }
            else
            {
                folders.push_back(i->u16string());
            }
        }

        return windows_path{drive, std::move(folders)};
    }

    void map(windows_path src, std::filesystem::path dest)
    {
        this->mappings_[std::move(src)] = std::move(dest);
    }

  private:
    std::filesystem::path root_{};
    std::unordered_map<windows_path, std::filesystem::path> mappings_{};
};
