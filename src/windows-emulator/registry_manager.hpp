#pragma once

#include "std_include.hpp"
#include <serialization_helper.hpp>

class hive_parser;

struct registry_key
{
	std::filesystem::path hive{};
	std::filesystem::path path{};

	void serialize(utils::buffer_serializer& buffer) const
	{
		buffer.write(this->hive);
		buffer.write(this->path);
	}

	void deserialize(utils::buffer_deserializer& buffer)
	{
		buffer.read(this->hive);
		buffer.read(this->path);
	}
};

struct registry_value
{
	uint32_t type;
	std::string_view data;
};

class registry_manager
{
public:
	using hive_ptr = std::unique_ptr<hive_parser>;
	using hive_map = std::unordered_map<std::filesystem::path, hive_ptr>;

	registry_manager(const std::filesystem::path& hive_path);
	~registry_manager();

	void serialize(utils::buffer_serializer& buffer) const;
	void deserialize(utils::buffer_deserializer& buffer);

	std::optional<registry_key> get_key(const std::filesystem::path& key);
	std::optional<registry_value> get_value(const registry_key& key, const std::string_view name);

private:
	hive_map hives_{};
	std::unordered_map<std::filesystem::path, std::filesystem::path> path_mapping_{};

	std::filesystem::path normalize_path(const std::filesystem::path& path) const;
	void add_path_mapping(const std::filesystem::path& key, const std::filesystem::path& value);

	hive_map::iterator find_hive(const std::filesystem::path& key);
};
