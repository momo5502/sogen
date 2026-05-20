#pragma once

#include "registry_manager.hpp"

namespace sogen
{

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

} // namespace sogen
