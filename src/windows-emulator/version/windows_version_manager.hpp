#pragma once

#include "../windows_path.hpp"
#include <serialization.hpp>
#include <optional>
#include <cstdint>

class logger;
class registry_manager;

struct windows_version_info
{
    windows_path system_root{};
    uint32_t major_version{0};
    uint32_t minor_version{0};
    uint32_t windows_build_number{0};
    uint32_t windows_update_build_revision{0};
};

class windows_version_manager
{
  public:
    void load_from_registry(registry_manager& registry, const logger& logger);

    bool is_build_before(uint32_t build, std::optional<uint32_t> ubr = std::nullopt) const;
    bool is_build_before_or_equal(uint32_t build, std::optional<uint32_t> ubr = std::nullopt) const;
    bool is_build_after_or_equal(uint32_t build, std::optional<uint32_t> ubr = std::nullopt) const;
    bool is_build_after(uint32_t build, std::optional<uint32_t> ubr = std::nullopt) const;
    bool is_build_within(uint32_t start_build, uint32_t end_build, std::optional<uint32_t> start_ubr = std::nullopt,
                         std::optional<uint32_t> end_ubr = std::nullopt) const;

    uint64_t get_system_dll_init_block_size() const;

    void serialize(utils::buffer_serializer& buffer) const;
    void deserialize(utils::buffer_deserializer& buffer);

    const windows_path& get_system_root() const
    {
        return info_.system_root;
    }
    void set_system_root(const windows_path& value)
    {
        info_.system_root = value;
    }

    uint32_t get_major_version() const
    {
        return info_.major_version;
    }
    void set_major_version(uint32_t value)
    {
        info_.major_version = value;
    }

    uint32_t get_minor_version() const
    {
        return info_.minor_version;
    }
    void set_minor_version(uint32_t value)
    {
        info_.minor_version = value;
    }

    uint32_t get_windows_build_number() const
    {
        return info_.windows_build_number;
    }
    void set_windows_build_number(uint32_t value)
    {
        info_.windows_build_number = value;
    }

    uint32_t get_windows_update_build_revision() const
    {
        return info_.windows_update_build_revision;
    }
    void set_windows_update_build_revision(uint32_t value)
    {
        info_.windows_update_build_revision = value;
    }

  private:
    windows_version_info info_{};
};
