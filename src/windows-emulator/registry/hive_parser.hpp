#pragma once

#include <ranges>
#include <fstream>
#include <algorithm>

#include <utils/container.hpp>

struct hive_value
{
    uint32_t type{};
    std::string name{};
    std::vector<std::byte> data{};
};

class hive_key
{
  public:
    hive_key(const int subkey_block_offset, const int value_count, const int value_offsets)
        : subkey_block_offset_(subkey_block_offset),
          value_count_(value_count),
          value_offsets_(value_offsets)
    {
    }

    utils::unordered_insensitive_string_map<hive_key>& get_sub_keys(std::ifstream& file)
    {
        this->parse(file);
        return this->sub_keys_;
    }

    const std::string_view* get_sub_key_name(std::ifstream& file, size_t index)
    {
        this->parse(file);

        if (index < 0 || index >= sub_keys_by_index_.size())
        {
            return nullptr;
        }

        return &sub_keys_by_index_[index];
    }

    hive_key* get_sub_key(std::ifstream& file, const std::string_view name)
    {
        auto& sub_keys = this->get_sub_keys(file);
        const auto entry = sub_keys.find(name);

        if (entry == sub_keys.end())
        {
            return nullptr;
        }

        return &entry->second;
    }

    hive_key* get_sub_key(std::ifstream& file, size_t index)
    {
        return get_sub_key(file, *this->get_sub_key_name(file, index));
    }

    const hive_value* get_value(std::ifstream& file, std::string_view name);
    const hive_value* get_value(std::ifstream& file, size_t index);

  private:
    struct raw_hive_value : hive_value
    {
        bool parsed{false};
        int data_offset{};
        size_t data_length{};
    };

    bool parsed_{false};
    utils::unordered_insensitive_string_map<hive_key> sub_keys_{};
    std::vector<std::string_view> sub_keys_by_index_{};
    utils::unordered_insensitive_string_map<raw_hive_value> values_{};
    std::vector<std::string_view> values_by_index_{};

    const int subkey_block_offset_{};
    const int value_count_{};
    const int value_offsets_{};

    void parse(std::ifstream& file);
};

class hive_parser
{
  public:
    explicit hive_parser(const std::filesystem::path& file_path);

    [[nodiscard]] hive_key* get_sub_key(const std::filesystem::path& key)
    {
        hive_key* current_key = &this->root_key_;

        for (const auto& key_part : key)
        {
            if (!current_key)
            {
                return nullptr;
            }

            current_key = current_key->get_sub_key(this->file_, key_part.string());
        }

        return current_key;
    }

    [[nodiscard]] const std::string_view* get_sub_key_name(const std::filesystem::path& key, size_t index)
    {
        auto* target_key = this->get_sub_key(key);
        if (!target_key)
        {
            return nullptr;
        }

        return target_key->get_sub_key_name(this->file_, index);
    }

    [[nodiscard]] const hive_value* get_value(const std::filesystem::path& key, const std::string_view name)
    {
        auto* sub_key = this->get_sub_key(key);
        if (!sub_key)
        {
            return nullptr;
        }

        return sub_key->get_value(this->file_, name);
    }

    [[nodiscard]] const hive_value* get_value(const std::filesystem::path& key, size_t index)
    {
        auto* sub_key = this->get_sub_key(key);
        if (!sub_key)
        {
            return nullptr;
        }

        return sub_key->get_value(this->file_, index);
    }

  private:
    std::ifstream file_{};
    hive_key root_key_;
};
