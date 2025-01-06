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

    utils::unordered_string_map<hive_key>& get_sub_keys(std::ifstream& file)
    {
        this->parse(file);
        return this->sub_keys_;
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

    const hive_value* get_value(std::ifstream& file, const std::string_view name);

  private:
    struct raw_hive_value : hive_value
    {
        bool parsed{false};
        int data_offset{};
        size_t data_length{};
    };

    bool parsed_{false};
    utils::unordered_string_map<hive_key> sub_keys_{};
    utils::unordered_string_map<raw_hive_value> values_{};

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

    [[nodiscard]] const hive_value* get_value(const std::filesystem::path& key, const std::string_view name)
    {
        auto* sub_key = this->get_sub_key(key);
        if (!sub_key)
        {
            return nullptr;
        }

        return sub_key->get_value(this->file_, name);
    }

  private:
    std::ifstream file_{};
    hive_key root_key_;
};
