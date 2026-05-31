#pragma once

#include "registry_manager.hpp"

namespace sogen::registry_utils
{

    inline std::optional<std::u16string> decode_registry_string(const registry_value& value)
    {
        if (value.type != REG_SZ && value.type != REG_EXPAND_SZ)
        {
            return std::nullopt;
        }

        if (value.data.empty() || value.data.size() % sizeof(char16_t) != 0)
        {
            return std::nullopt;
        }

        const auto* data = reinterpret_cast<const char16_t*>(value.data.data());
        auto char_count = value.data.size() / sizeof(char16_t);
        while (char_count > 0 && data[char_count - 1] == u'\0')
        {
            --char_count;
        }

        return std::u16string(data, char_count);
    }

    inline std::optional<std::u16string> read_registry_string(registry_manager& registry, const registry_key& key,
                                                              const std::string_view value_name)
    {
        const auto value = registry.get_value(key, value_name);
        if (!value)
        {
            return std::nullopt;
        }

        return decode_registry_string(*value);
    }

    inline std::optional<std::u16string> read_registry_string(registry_manager& registry, const std::filesystem::path& key_path,
                                                              const std::string_view value_name)
    {
        const auto key = registry.get_key(utils::path_key{key_path});
        if (!key)
        {
            return std::nullopt;
        }

        return read_registry_string(registry, *key, value_name);
    }

    inline std::string get_user_sid_string(registry_manager& registry)
    {
        const std::filesystem::path profile_list_path = R"(\Registry\Machine\Software\Microsoft\Windows NT\CurrentVersion\ProfileList)";

        const auto profile_list_key = registry.get_key(profile_list_path);
        if (!profile_list_key)
        {
            throw std::runtime_error("Failed to get ProfileList registry key");
        }

        for (size_t i = 0;; ++i)
        {
            const auto value = registry.get_sub_key_name(*profile_list_key, i);
            if (!value.has_value())
            {
                break;
            }

            const auto profile_key = registry.get_key(profile_list_path / *value);
            if (!profile_key.has_value())
            {
                continue;
            }

            const auto full_profile = registry.get_value(*profile_key, "FullProfile");
            if (full_profile && full_profile->as_dword().value_or(0) == 1)
            {
                return std::string(*value);
            }
        }

        return "S-1-5-18";
    }

    inline std::u16string get_user_profile_path(registry_manager& registry)
    {
        const std::filesystem::path profile_list_path = R"(\Registry\Machine\Software\Microsoft\Windows NT\CurrentVersion\ProfileList)";

        try
        {
            const auto sid = get_user_sid_string(registry);
            if (auto profile_path = read_registry_string(registry, profile_list_path / sid, "ProfileImagePath");
                profile_path && !profile_path->empty())
            {
                return *profile_path;
            }
        }
        catch (const std::exception&)
        {
        }

        return u"C:\\Users\\momo";
    }

    inline std::u16string get_user_name(registry_manager& registry)
    {
        const auto basename_from_windows_path = [&](std::u16string path) -> std::u16string {
            while (!path.empty() && (path.back() == u'\\' || path.back() == u'/'))
            {
                path.pop_back();
            }

            const auto slash = path.find_last_of(u"\\/");
            if (slash == std::u16string::npos)
            {
                return path;
            }

            return path.substr(slash + 1);
        };

        if (auto name = basename_from_windows_path(get_user_profile_path(registry)); !name.empty())
        {
            return name;
        }

        return u"momo";
    }

    inline std::u16string get_account_domain(registry_manager& registry)
    {
        const std::array<std::filesystem::path, 2> key_paths = {
            R"(\Registry\Machine\System\CurrentControlSet\Control\ComputerName\ActiveComputerName)",
            R"(\Registry\Machine\System\CurrentControlSet\Control\ComputerName\ComputerName)",
        };

        for (const auto& key_path : key_paths)
        {
            if (auto computer_name = read_registry_string(registry, key_path, "ComputerName"); computer_name && !computer_name->empty())
            {
                return *computer_name;
            }
        }

        return u"momo";
    }

} // namespace sogen
