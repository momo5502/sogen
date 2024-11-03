#pragma once

#include <string>
#include <fstream>
#include <vector>
#include <ranges>
#include <cwctype>
#include <optional>
#include <utils/container.hpp>

// Based on this implementation: https://github.com/reahly/windows-hive-parser

struct offset_entry_t
{
	long offset;
	long hash;
};

struct offsets_t
{
	long block_size;
	char block_type[2];
	short count;
	offset_entry_t entries[0];
};

struct key_block_t
{
	long block_size;
	char block_type[2];
	char dummya[18];
	int subkey_count;
	char dummyb[4];
	int subkeys;
	char dummyc[4];
	int value_count;
	int offsets;
	char dummyd[28];
	short len;
	short du;
	char name[255];
};

struct value_block_t
{
	long block_size;
	char block_type[2];
	short name_len;
	long size;
	long offset;
	long value_type;
	short flags;
	short dummy;
	char name[255];
};

namespace detail
{
	inline std::vector<char> read_file(const std::filesystem::path& file_path)
	{
		std::ifstream file(file_path, std::ios::binary);
		if (!file.is_open())
		{
			return {};
		}

		return {std::istreambuf_iterator(file), std::istreambuf_iterator<char>()};
	}
}

class hive_key_t
{
	key_block_t* key_block;
	uintptr_t main_root;

public:
	explicit hive_key_t(): key_block(nullptr), main_root(0)
	{
	}

	explicit hive_key_t(key_block_t* a, const uintptr_t b): key_block(a), main_root(b)
	{
	}

	[[nodiscard]] std::vector<std::string_view> subkeys_list() const
	{
		const auto item = reinterpret_cast<offsets_t*>(this->main_root + key_block->subkeys);
		if (item->block_type[1] != 'f' && item->block_type[1] != 'h')
			return {};

		std::vector<std::string_view> out;
		for (auto i = 0; i < key_block->subkey_count; i++)
		{
			const auto subkey = reinterpret_cast<key_block_t*>(item->entries[i].offset + this->main_root);
			if (!subkey)
				continue;

			out.emplace_back(subkey->name, subkey->len);
		}

		return out;
	}

	[[nodiscard]] std::vector<std::string_view> keys_list() const
	{
		if (!key_block->value_count)
			return {};

		std::vector<std::string_view> out;
		for (auto i = 0; i < key_block->value_count; i++)
		{
			const auto value = reinterpret_cast<value_block_t*>(reinterpret_cast<int*>(key_block->offsets + this->
				main_root + 4)[i] + this->main_root);
			if (!value)
				continue;

			out.emplace_back(value->name, value->name_len);
		}

		return out;
	}

	using value = std::pair<long, std::string_view>;

	std::optional<value> get_key_value(const std::string_view& name)
	{
		for (auto i = 0; i < key_block->value_count; i++)
		{
			const auto value = reinterpret_cast<value_block_t*>(reinterpret_cast<int*>(key_block->offsets + this->
				main_root + 4)[i] + this->main_root);
			if (!value || std::string_view(value->name, value->name_len) != name)
				continue;

			auto data = reinterpret_cast<char*>(this->main_root + value->offset + 4);
			if (value->size & 1 << 31)
				data = reinterpret_cast<char*>(&value->offset);

			return std::make_pair(value->value_type, std::string_view(data, value->size & 0xffff));
		}

		return std::nullopt;
	}
};

class hive_parser
{
	struct hive_subpaths_t
	{
		std::string path;
		hive_key_t data;
	};

	struct hive_cache_t
	{
		hive_key_t data;
		std::vector<hive_subpaths_t> subpaths;
	};

	key_block_t* main_key_block_data;
	uintptr_t main_root;
	std::vector<char> file_data;
	utils::unordered_string_map<hive_cache_t> subkey_cache;

	void reclusive_search(const key_block_t* key_block_data, const std::string& current_path,
	                      const bool is_reclusive = false)
	{
		if (!key_block_data)
			return;

		const auto item = reinterpret_cast<offsets_t*>(main_root + key_block_data->subkeys);
		if (item->block_type[1] != 'f' && item->block_type[1] != 'h')
			return;

		for (auto i = 0; i < item->count; i++)
		{
			const auto subkey = reinterpret_cast<key_block_t*>(item->entries[i].offset + main_root);
			if (!subkey)
				continue;

			std::string_view subkey_name(subkey->name, subkey->len);
			std::string full_path = current_path.empty()
				                        ? std::string(subkey_name)
				                        : std::string(current_path).append("/").append(subkey_name);
			std::ranges::transform(full_path, full_path.begin(), ::tolower);

			if (!is_reclusive)
				subkey_cache.try_emplace(full_path, hive_cache_t{
					                         hive_key_t{subkey, main_root}, std::vector<hive_subpaths_t>{}
				                         });

			const auto extract_main_key = [ ](const std::string_view str) -> std::string_view
			{
				const size_t slash_pos = str.find('/');
				if (slash_pos == std::string::npos)
					return str;

				return str.substr(0, slash_pos);
			};

			if (subkey->subkey_count > 0)
			{
				reclusive_search(subkey, full_path, true);
				const auto entry = subkey_cache.find(extract_main_key(full_path));
				if (entry == subkey_cache.end())
				{
					throw std::out_of_range("Invalid key");
				}

				entry->second.subpaths.emplace_back(hive_subpaths_t{
					full_path, hive_key_t{subkey, main_root}
				});
			}
			else
			{
				const auto entry = subkey_cache.find(extract_main_key(full_path));
				if (entry == subkey_cache.end())
				{
					throw std::out_of_range("Invalid key");
				}

				entry->second.subpaths.emplace_back(full_path, hive_key_t{subkey, main_root});
			}
		}
	}

public:
	explicit hive_parser(const std::filesystem::path& file_path)
		: hive_parser(detail::read_file(file_path))
	{
	}

	explicit hive_parser(std::vector<char> input_data)
		: file_data(std::move(input_data))
	{
		if (file_data.size() < 0x1020)
			return;

		if (file_data.at(0) != 'r' && file_data.at(1) != 'e' && file_data.at(2) != 'g' && file_data.at(3) != 'f')
			return;

		main_key_block_data = reinterpret_cast<key_block_t*>(reinterpret_cast<uintptr_t>(file_data.data() + 0x1020));
		main_root = reinterpret_cast<uintptr_t>(main_key_block_data) - 0x20;

		reclusive_search(main_key_block_data, "");
	}

	[[nodiscard]] bool success() const
	{
		return !subkey_cache.empty();
	}

	[[nodiscard]] std::optional<hive_key_t> get_subkey(const std::string_view key_name,
	                                                   const std::string_view path) const
	{
		if (!subkey_cache.contains(key_name))
			return std::nullopt;

		const auto hive_block = subkey_cache.find(key_name);
		if (hive_block == subkey_cache.end())
		{
			throw std::out_of_range("Invalid key");
		}

		for (const auto& hive : hive_block->second.subpaths)
		{
			if (hive.path == path)
				return hive.data;
		}

		return std::nullopt;
	}
};
