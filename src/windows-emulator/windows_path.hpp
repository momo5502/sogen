#pragma once

#include <list>
#include <optional>
#include <filesystem>
#include <utils/string.hpp>

#include <serialization.hpp>

namespace windows_path_detail
{
    constexpr std::u16string_view unc_prefix = u"\\??\\";

    inline std::u16string_view strip_unc_prefix(const std::u16string_view path)
    {
        if (!path.starts_with(unc_prefix))
        {
            return path;
        }

        return path.substr(unc_prefix.size());
    }

    inline bool is_slash(const char16_t chr)
    {
        return chr == u'\\' || chr == u'/';
    }
}

class windows_path
{
  public:
    friend std::hash<windows_path>;

    windows_path() = default;

    windows_path(const std::filesystem::path& path)
    {
        const auto full_path = path.u16string();
        const auto canonical_path = windows_path_detail::strip_unc_prefix(full_path);

        std::u16string folder{};

        for (const auto chr : canonical_path)
        {
            if (chr == u':' && this->folders_.empty() && !this->drive_.has_value() && folder.size() == 1)
            {
                this->drive_ = static_cast<char>(folder[0]);
                folder.clear();
                continue;
            }

            if (windows_path_detail::is_slash(chr))
            {
                if (folder.empty())
                {
                    continue;
                }

                this->folders_.push_back(std::move(folder));
                folder = {};
                continue;
            }

            folder.push_back(chr);
        }

        if (!folder.empty())
        {
            this->folders_.push_back(folder);
        }

        this->canonicalize();
    }

    template <typename T>
        requires(!std::is_same_v<std::remove_cvref_t<T>, windows_path> &&
                 !std::is_same_v<std::remove_cvref_t<T>, std::filesystem::path>)
    windows_path(T&& path_like)
        : windows_path(std::filesystem::path(std::forward<T>(path_like)))
    {
    }

    windows_path(const std::optional<char> drive, std::list<std::u16string> folders)
        : drive_(drive),
          folders_(std::move(folders))
    {
        this->canonicalize();
    }

    bool is_absolute() const
    {
        return this->drive_.has_value();
    }

    bool is_relative() const
    {
        return !this->is_absolute();
    }

    std::u16string u16string() const
    {
        std::u16string path{};
        if (this->drive_)
        {
            path.push_back(static_cast<char16_t>(*this->drive_));
            path.push_back(u':');
        }

        for (const auto& folder : this->folders_)
        {
            if (!path.empty())
            {
                path.push_back(u'\\');
            }

            path.append(folder);
        }

        return path;
    }

    std::string string() const
    {
        return u16_to_u8(this->u16string());
    }

    std::u16string to_unc_path() const
    {
        if (this->is_relative())
        {
            return this->u16string();
        }

        return std::u16string(windows_path_detail::unc_prefix) + this->u16string();
    }

    std::filesystem::path to_portable_path() const
    {
        std::u16string path{};
        if (this->drive_)
        {
            path.push_back(static_cast<char16_t>(*this->drive_));
        }

        for (const auto& folder : this->folders_)
        {
            if (!path.empty())
            {
                path.push_back(u'/');
            }

            path.append(folder);
        }

        return path;
    }

    std::u16string to_device_path() const
    {
        if (is_relative())
        {
            throw std::runtime_error("Device path can not be computed for relative paths!");
        }

        const auto drive_index = *this->drive_ - 'a';
        const auto drive_number = std::to_string(drive_index + 1);
        const std::u16string number(drive_number.begin(), drive_number.end());

        std::u16string path = u"\\Device\\HarddiskVolume";
        path.append(number);
        path.push_back(u'\\');
        path.append(this->without_drive().u16string());

        return path;
    }

    std::optional<char> get_drive() const
    {
        return this->drive_;
    }

    windows_path without_drive() const
    {
        return windows_path{std::nullopt, this->folders_};
    }

    windows_path operator/(const windows_path& path) const
    {
        if (path.is_absolute())
        {
            return path;
        }

        auto folders = this->folders_;

        for (const auto& folder : path.folders_)
        {
            folders.push_back(folder);
        }

        return {this->drive_, std::move(folders)};
    }

    windows_path& operator/=(const windows_path& path)
    {
        *this = *this / path;
        return *this;
    }

    windows_path parent() const
    {
        auto folders = this->folders_;
        if (!folders.empty())
        {
            folders.pop_back();
        }

        return {this->drive_, std::move(folders)};
    }

    void serialize(utils::buffer_serializer& buffer) const
    {
        buffer.write_optional(this->drive_);
        buffer.write_list(this->folders_);
    }

    void deserialize(utils::buffer_deserializer& buffer)
    {
        buffer.read_optional(this->drive_);
        buffer.read_list(this->folders_);
    }

    bool operator==(const windows_path& other) const
    {
        return this->drive_ == other.drive_ && this->folders_ == other.folders_;
    }

    bool operator!=(const windows_path& other) const
    {
        return !this->operator==(other);
    }

    bool empty() const
    {
        return this->is_relative() && this->folders_.empty();
    }

  private:
    std::optional<char> drive_{};
    std::list<std::u16string> folders_{};

    void canonicalize()
    {
        if (this->drive_.has_value())
        {
            this->drive_ = utils::string::char_to_lower(*this->drive_);
        }

        for (auto& folder : this->folders_)
        {
            for (auto& chr : folder)
            {
                chr = utils::string::char_to_lower(chr);
            }
        }
    }
};

template <>
struct std::hash<windows_path>
{
    std::size_t operator()(const windows_path& k) const noexcept
    {
        auto hash = std::hash<bool>()(k.drive_.has_value());

        if (k.drive_.has_value())
        {
            hash ^= std::hash<char>()(*k.drive_);
        }

        for (const auto& folder : k.folders_)
        {
            hash ^= std::hash<std::u16string>()(folder);
        }

        return hash;
    }
};
