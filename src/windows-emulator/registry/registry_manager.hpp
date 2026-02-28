#pragma once

#include "../std_include.hpp"
#include "hive_parser.hpp"
#include "serialization_helper.hpp"
#include "../handles.hpp"

#ifndef OS_WINDOWS
#define REG_NONE (0ul) // No value type
#define REG_SZ   (1ul) // Unicode nul terminated string
#define REG_EXPAND_SZ \
    (2ul)                                    // Unicode nul terminated string
                                             // (with environment variable references)
#define REG_BINARY                     (3ul) // Free form binary
#define REG_DWORD                      (4ul) // 32-bit number
#define REG_DWORD_LITTLE_ENDIAN        (4ul) // 32-bit number (same as REG_DWORD)
#define REG_DWORD_BIG_ENDIAN           (5ul) // 32-bit number
#define REG_LINK                       (6ul) // Symbolic Link (unicode)
#define REG_MULTI_SZ                   (7ul) // Multiple Unicode strings
#define REG_RESOURCE_LIST              (8ul) // Resource list in the resource map
#define REG_FULL_RESOURCE_DESCRIPTOR   (9ul) // Resource list in the hardware description
#define REG_RESOURCE_REQUIREMENTS_LIST (10ul)
#define REG_QWORD                      (11ul) // 64-bit number
#define REG_QWORD_LITTLE_ENDIAN        (11ul) // 64-bit number (same as REG_QWORD)
#endif

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

    std::u16string to_string() const
    {
        return this->hive.get().u16string() + u"\\" + this->path.get().u16string();
    }
};

struct registry_value
{
    uint32_t type;
    std::string_view name;
    std::span<const std::byte> data;

    bool is_dword() const
    {
        return type == REG_DWORD;
    }

    bool is_string() const
    {
        return type == REG_SZ;
    }

    std::optional<DWORD> as_dword() const
    {
        DWORD value{};
        if (!is_dword() || data.size() < sizeof(value))
        {
            return std::nullopt;
        }

        memcpy(&value, data.data(), sizeof(value));
        return value;
    }

    std::optional<std::u16string> as_string() const
    {
        if (!is_string())
        {
            return std::nullopt;
        }

        const auto* data_ptr = reinterpret_cast<const char16_t*>(data.data());
        const auto char_count = data.size() / sizeof(char16_t);
        const auto count = char_count > 0 && data_ptr[char_count - 1] == u'\0' ? char_count - 1 : char_count;
        return std::u16string(data_ptr, count);
    }
};

struct exposed_hive_key
{
    hive_key& key;
    std::ifstream& file;
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
    std::optional<registry_value> get_value(const registry_key& key, std::string_view name);
    std::optional<registry_value> get_value(const registry_key& key, size_t index);
    void set_value(const registry_key& key, std::string name, uint32_t type, std::span<const std::byte> data);

    std::optional<std::string_view> get_sub_key_name(const registry_key& key, size_t index);

    std::optional<exposed_hive_key> get_hive_key(const registry_key& key);

    std::optional<std::u16string> read_u16string(const registry_key& key, size_t index);
    void serialize_runtime_state(utils::buffer_serializer& buffer) const;
    void deserialize_runtime_state(utils::buffer_deserializer& buffer);

  private:
    struct overlay_value
    {
        uint32_t type{};
        std::vector<std::byte> data{};

        void serialize(utils::buffer_serializer& buffer) const
        {
            buffer.write(this->type);
            buffer.write_vector(this->data);
        }

        void deserialize(utils::buffer_deserializer& buffer)
        {
            buffer.read(this->type);
            buffer.read_vector(this->data);
        }
    };

    struct overlay_bucket
    {
        utils::insensitive_string_map<overlay_value> values{};

        void serialize(utils::buffer_serializer& buffer) const
        {
            buffer.write_map(this->values);
        }

        void deserialize(utils::buffer_deserializer& buffer)
        {
            buffer.read_map(this->values);
        }
    };

    std::filesystem::path hive_path_{};
    hive_map hives_{};
    std::map<utils::path_key, utils::path_key> path_mapping_{};
    std::map<utils::path_key, overlay_bucket> overlay_values_{};

    utils::path_key normalize_path(utils::path_key path) const;
    static utils::path_key get_full_key_path(const registry_key& key);
    void add_path_mapping(const utils::path_key& key, const utils::path_key& value);

    hive_map::iterator find_hive(const utils::path_key& key);

    void setup();
};
