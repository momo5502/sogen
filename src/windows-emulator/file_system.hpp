#pragma once
#include "std_include.hpp"
#include <utils/path_key.hpp>

class file_system
{
  public:
    static constexpr std::u16string_view nt_prefix = u"\\??\\";

    file_system(std::filesystem::path root, std::u16string working_dir)
        : root_(std::move(root)),
          working_dir_(std::move(working_dir))
    {
    }

    static std::filesystem::path canonicalize_windows_path(const std::filesystem::path& file)
    {
        const auto wide_file = file.u16string();

        if (!wide_file.starts_with(nt_prefix))
        {
            return file;
        }

        return canonicalize_windows_path(wide_file.substr(nt_prefix.size()));
    }

    static std::filesystem::path canonicalize(const std::filesystem::path& path)
    {
        return utils::path_key::canonicalize_path(canonicalize_windows_path(path));
    }

    static std::filesystem::path canonicalize_drive_letter(const std::filesystem::path& path)
    {
        auto canonical_path = canonicalize(path);
        if (canonical_path.empty() || !path.has_root_path())
        {
            return canonical_path;
        }

        return adapt_drive_component(canonical_path);
    }

    std::filesystem::path translate(const std::filesystem::path& win_path) const
    {
        const auto absolute_win_dir = make_absolute_windows_path(win_path.u16string());
        return this->root_ / canonicalize_drive_letter(absolute_win_dir);
    }

    void set_working_directory(std::u16string working_dir)
    {
        this->working_dir_ = std::move(working_dir);
    }

    const std::u16string& get_working_directory() const
    {
        return this->working_dir_;
    }

  private:
    std::filesystem::path root_;
    std::u16string working_dir_;

    std::u16string make_absolute_windows_path(const std::u16string& path) const
    {
        if (!path.starts_with(nt_prefix) && (path.size() < 2 || path[1] != ':'))
        {
            return this->working_dir_ + u'/' + path;
        }

        return path;
    }

    static std::filesystem::path adapt_drive_component(const std::filesystem::path& original_path)
    {
        auto root_name = original_path.root_name().u16string();

        if (!root_name.empty() && root_name.back() == u':')
        {
            root_name.pop_back();
        }

        return root_name + original_path.root_directory().u16string() + original_path.relative_path().u16string();
    }
};
