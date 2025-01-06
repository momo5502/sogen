#include "hive_parser.hpp"
#include <utils/string.hpp>

// Based on this implementation: https://github.com/reahly/windows-hive-parser

namespace
{
    constexpr uint64_t MAIN_ROOT_OFFSET = 0x1000;
    constexpr uint64_t MAIN_KEY_BLOCK_OFFSET = MAIN_ROOT_OFFSET + 0x20;

    struct offset_entry_t
    {
        int32_t offset;
        int32_t hash;
    };

    struct offsets_t
    {
        int32_t block_size;
        char block_type[2];
        int16_t count;
        offset_entry_t entries[1];
    };

    struct key_block_t
    {
        int32_t block_size;
        char block_type[2];
        uint8_t dummya[18];
        int32_t subkey_count;
        uint8_t dummyb[4];
        int32_t subkeys;
        uint8_t dummyc[4];
        int32_t value_count;
        int32_t offsets;
        uint8_t dummyd[28];
        int16_t len;
        int16_t du;
        char name[255];
    };

    struct value_block_t
    {
        int32_t block_size;
        char block_type[2];
        int16_t name_len;
        int32_t size;
        int32_t offset;
        int32_t value_type;
        int16_t flags;
        int16_t dummy;
        char name[255];
    };

    bool read_file_data_safe(std::ifstream& file, const uint64_t offset, void* buffer, const size_t size)
    {
        if (file.bad())
        {
            return false;
        }

        file.clear();

        if (!file.good())
        {
            return false;
        }

        file.seekg(static_cast<std::streamoff>(offset));

        if (!file.good())
        {
            return false;
        }

        file.read(static_cast<char*>(buffer), static_cast<std::streamsize>(size));

        return file.good();
    }

    void read_file_data(std::ifstream& file, const uint64_t offset, void* buffer, const size_t size)
    {
        if (!read_file_data_safe(file, offset, buffer, size))
        {
            throw std::runtime_error("Failed to read file data");
        }
    }

    std::vector<std::byte> read_file_data(std::ifstream& file, const uint64_t offset, const size_t size)
    {
        std::vector<std::byte> result{};
        result.resize(size);

        read_file_data(file, offset, result.data(), size);
        return result;
    }

    std::string read_file_data_string(std::ifstream& file, const uint64_t offset, const size_t size)
    {
        std::string result{};
        result.resize(size);

        read_file_data(file, offset, result.data(), size);
        return result;
    }

    template <typename T>
        requires(std::is_trivially_copyable_v<T>)
    T read_file_object(std::ifstream& file, const uint64_t offset, const size_t array_index = 0)
    {
        T obj{};
        read_file_data(file, offset + (array_index * sizeof(T)), &obj, sizeof(T));
        return obj;
    }

    hive_key parse_root_block(std::ifstream& file, const std::filesystem::path& file_path)
    {
        try
        {
            if (read_file_data_string(file, 0, 4) != "regf")
            {
                throw std::runtime_error("Invalid signature");
            }

            const auto key_block = read_file_object<key_block_t>(file, MAIN_KEY_BLOCK_OFFSET);

            return {key_block.subkeys, key_block.value_count, key_block.offsets};
        }
        catch (const std::exception& e)
        {
            throw std::runtime_error("Bad hive file '" + file_path.string() + "': " + e.what());
        }
    }
}

const hive_value* hive_key::get_value(std::ifstream& file, const std::string_view name)
{
    this->parse(file);

    const auto entry = this->values_.find(name);
    if (entry == this->values_.end())
    {
        return nullptr;
    }

    auto& value = entry->second;

    if (!value.parsed)
    {
        value.data = read_file_data(file, MAIN_ROOT_OFFSET + value.data_offset, value.data_length);
        value.parsed = true;
    }

    return &value;
}

void hive_key::parse(std::ifstream& file)
{
    if (this->parsed_)
    {
        return;
    }

    this->parsed_ = true;

    // Values

    for (auto i = 0; i < this->value_count_; i++)
    {
        const auto offset = read_file_object<int>(file, MAIN_ROOT_OFFSET + this->value_offsets_ + 4, i);
        const auto value = read_file_object<value_block_t>(file, MAIN_ROOT_OFFSET + offset);

        std::string value_name(value.name, std::min(value.name_len, static_cast<short>(sizeof(value.name))));

        raw_hive_value raw_value{};
        raw_value.parsed = false;
        raw_value.type = value.value_type;
        raw_value.name = value_name;
        raw_value.data_length = value.size & 0xffff;
        raw_value.data_offset = value.offset + 4;

        if (value.size & 1 << 31)
        {
            raw_value.data_offset = offset + static_cast<int>(offsetof(value_block_t, offset));
        }

        utils::string::to_lower_inplace(value_name);
        this->values_[std::move(value_name)] = std::move(raw_value);
    }

    // Subkeys

    const auto item = read_file_object<offsets_t>(file, MAIN_ROOT_OFFSET + this->subkey_block_offset_);

    if (item.block_type[1] != 'f' && item.block_type[1] != 'h')
    {
        return;
    }

    const auto entry_offsets = this->subkey_block_offset_ + offsetof(offsets_t, entries);

    for (short i = 0; i < item.count; ++i)
    {
        const auto offset_entry = read_file_object<offset_entry_t>(file, MAIN_ROOT_OFFSET + entry_offsets, i);

        const auto subkey_block_offset = MAIN_ROOT_OFFSET + offset_entry.offset;
        const auto subkey = read_file_object<key_block_t>(file, subkey_block_offset);

        std::string subkey_name(subkey.name, std::min(subkey.len, static_cast<int16_t>(sizeof(subkey.name))));
        utils::string::to_lower_inplace(subkey_name);

        this->sub_keys_.emplace(std::move(subkey_name), hive_key{subkey.subkeys, subkey.value_count, subkey.offsets});
    }
}

hive_parser::hive_parser(const std::filesystem::path& file_path)
    : file_(file_path, std::ios::binary),
      root_key_(parse_root_block(file_, file_path))
{
}
