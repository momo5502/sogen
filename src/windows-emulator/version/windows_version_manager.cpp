#include "../std_include.hpp"
#include "windows_version_manager.hpp"
#include "../registry/registry_manager.hpp"
#include "../logger.hpp"
#include <platform/kernel_mapped.hpp>

void windows_version_manager::load_from_registry(registry_manager& registry, const logger& logger)
{
    constexpr auto version_key_path = R"(\Registry\Machine\Software\Microsoft\Windows NT\CurrentVersion)";

    const auto version_key = registry.get_key({version_key_path});
    if (!version_key)
    {
        throw std::runtime_error("Failed to get CurrentVersion registry key");
    }

    for (size_t i = 0; const auto value = registry.get_value(*version_key, i); ++i)
    {
        if (value->name == "SystemRoot" && value->is_string())
        {
            info_.system_root = windows_path{value->as_string().value_or({})};
        }
        else if ((value->name == "CurrentBuildNumber" || value->name == "CurrentBuild") && value->is_string())
        {
            const auto str = value->as_string().value_or(u"0");
            info_.windows_build_number = static_cast<uint32_t>(std::strtoul(u16_to_u8(str).c_str(), nullptr, 10));
        }
        else if (value->name == "UBR" && value->is_dword())
        {
            info_.windows_update_build_revision = value->as_dword().value_or(0);
        }
        else if (value->name == "CurrentMajorVersionNumber" && value->is_dword())
        {
            info_.major_version = value->as_dword().value_or(0);
        }
        else if (value->name == "CurrentMinorVersionNumber" && value->is_dword())
        {
            info_.minor_version = value->as_dword().value_or(0);
        }
    }

    if (info_.system_root.u16string().empty())
    {
        throw std::runtime_error("SystemRoot not found in registry");
    }

    if (info_.windows_build_number == 0)
    {
        logger.error("Failed to get CurrentBuildNumber from registry\n");
    }

    if (info_.windows_update_build_revision == 0)
    {
        logger.error("Failed to get UBR from registry\n");
    }
}

bool windows_version_manager::is_build_before(uint32_t build, std::optional<uint32_t> ubr) const
{
    if (info_.windows_build_number != build)
    {
        return info_.windows_build_number < build;
    }
    return ubr.has_value() && info_.windows_update_build_revision < *ubr;
}

bool windows_version_manager::is_build_before_or_equal(uint32_t build, std::optional<uint32_t> ubr) const
{
    if (info_.windows_build_number != build)
    {
        return info_.windows_build_number < build;
    }
    return !ubr.has_value() || info_.windows_update_build_revision <= *ubr;
}

bool windows_version_manager::is_build_after_or_equal(uint32_t build, std::optional<uint32_t> ubr) const
{
    if (info_.windows_build_number != build)
    {
        return info_.windows_build_number > build;
    }
    return !ubr.has_value() || info_.windows_update_build_revision >= *ubr;
}

bool windows_version_manager::is_build_after(uint32_t build, std::optional<uint32_t> ubr) const
{
    if (info_.windows_build_number != build)
    {
        return info_.windows_build_number > build;
    }
    return ubr.has_value() && info_.windows_update_build_revision > *ubr;
}

bool windows_version_manager::is_build_within(uint32_t start_build, uint32_t end_build, std::optional<uint32_t> start_ubr,
                                              std::optional<uint32_t> end_ubr) const
{
    return is_build_after_or_equal(start_build, start_ubr) && is_build_before(end_build, end_ubr);
}

void windows_version_manager::serialize(utils::buffer_serializer& buffer) const
{
    buffer.write(info_.system_root);
    buffer.write(info_.major_version);
    buffer.write(info_.minor_version);
    buffer.write(info_.windows_build_number);
    buffer.write(info_.windows_update_build_revision);
}

void windows_version_manager::deserialize(utils::buffer_deserializer& buffer)
{
    buffer.read(info_.system_root);
    buffer.read(info_.major_version);
    buffer.read(info_.minor_version);
    buffer.read(info_.windows_build_number);
    buffer.read(info_.windows_update_build_revision);
}
