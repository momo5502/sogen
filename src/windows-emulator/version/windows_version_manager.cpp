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
        if (value->name == "SystemRoot" && value->type == REG_SZ && !value->data.empty())
        {
            const auto* data_ptr = reinterpret_cast<const char16_t*>(value->data.data());
            const auto char_count = value->data.size() / sizeof(char16_t);
            std::u16string system_root(data_ptr, char_count > 0 && data_ptr[char_count - 1] == u'\0' ? char_count - 1 : char_count);
            info_.system_root = windows_path{system_root};
        }
        else if ((value->name == "CurrentBuildNumber" || value->name == "CurrentBuild") && value->type == REG_SZ)
        {
            const auto* s = reinterpret_cast<const char16_t*>(value->data.data());
            info_.windows_build_number = static_cast<uint32_t>(std::strtoul(u16_to_u8(s).c_str(), nullptr, 10));
        }
        else if (value->name == "UBR" && value->type == REG_DWORD && value->data.size() >= sizeof(uint32_t))
        {
            info_.windows_update_build_revision = *reinterpret_cast<const uint32_t*>(value->data.data());
        }
        else if (value->name == "CurrentMajorVersionNumber" && value->type == REG_DWORD && value->data.size() >= sizeof(uint32_t))
        {
            info_.major_version = *reinterpret_cast<const uint32_t*>(value->data.data());
        }
        else if (value->name == "CurrentMinorVersionNumber" && value->type == REG_DWORD && value->data.size() >= sizeof(uint32_t))
        {
            info_.minor_version = *reinterpret_cast<const uint32_t*>(value->data.data());
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

uint64_t windows_version_manager::get_system_dll_init_block_size() const
{
    if (is_build_after_or_equal(WINDOWS_VERSION::WINDOWS_11_24H2))
    {
        return PS_SYSTEM_DLL_INIT_BLOCK_SIZE_V3;
    }
    if (is_build_after_or_equal(WINDOWS_VERSION::WINDOWS_10_2004))
    {
        return PS_SYSTEM_DLL_INIT_BLOCK_SIZE_V3_2004;
    }
    if (is_build_after_or_equal(WINDOWS_VERSION::WINDOWS_10_1709))
    {
        return PS_SYSTEM_DLL_INIT_BLOCK_SIZE_V2;
    }
    if (is_build_after_or_equal(WINDOWS_VERSION::WINDOWS_10_1703))
    {
        return PS_SYSTEM_DLL_INIT_BLOCK_SIZE_V2_1703;
    }
    return PS_SYSTEM_DLL_INIT_BLOCK_SIZE_V1;
}

void windows_version_manager::serialize(utils::buffer_serializer& buffer) const
{
    buffer.write(info_.major_version);
    buffer.write(info_.minor_version);
    buffer.write(info_.windows_build_number);
    buffer.write(info_.windows_update_build_revision);
}

void windows_version_manager::deserialize(utils::buffer_deserializer& buffer)
{
    buffer.read(info_.major_version);
    buffer.read(info_.minor_version);
    buffer.read(info_.windows_build_number);
    buffer.read(info_.windows_update_build_revision);
}
