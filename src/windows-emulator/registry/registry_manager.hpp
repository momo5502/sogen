#pragma once

#include "../std_include.hpp"
#include "hive_parser.hpp"
#include "serialization_helper.hpp"
#include "../handles.hpp"

struct registry_key : ref_counted_object
{
    utils::path_key hive{};
    utils::path_key path{};

    void serialize_object(utils::buffer_serializer& buffer) const override
    {
        buffer.write(this->hive);
        buffer.write(this->path);
    }

    void deserialize_object(utils::buffer_deserializer& buffer) override
    {
        buffer.read(this->hive);
        buffer.read(this->path);
    }
};

struct registry_value
{
    uint32_t type;
    std::string_view name;
    std::span<const std::byte> data;
};

class registry_manager
{
  public:
    using hive_ptr = std::unique_ptr<hive_parser>;
    using hive_map = std::unordered_map<utils::path_key, hive_ptr>;

    registry_manager();
    registry_manager(const std::filesystem::path& hive_path);
    ~registry_manager();

    registry_manager(registry_manager&&) noexcept;
    registry_manager& operator=(registry_manager&&) noexcept;

    registry_manager(const registry_manager&) = delete;
    registry_manager& operator=(const registry_manager&) = delete;

    std::optional<registry_key> get_key(const utils::path_key& key);
    std::optional<registry_value> get_value(const registry_key& key, std::string name);

  private:
    std::filesystem::path hive_path_{};
    hive_map hives_{};
    std::unordered_map<utils::path_key, utils::path_key> path_mapping_{};

    utils::path_key normalize_path(const utils::path_key& path) const;
    void add_path_mapping(const utils::path_key& key, const utils::path_key& value);

    hive_map::iterator find_hive(const utils::path_key& key);

    void setup();
};
