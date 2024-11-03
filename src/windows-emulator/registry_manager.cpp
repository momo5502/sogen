#include "registry_manager.hpp"
#include <hive_parser.hh>

namespace
{
	std::filesystem::path canonicalize_path(const std::filesystem::path& key)
	{
		auto path = key.lexically_normal().wstring();
		std::ranges::transform(path, path.begin(), std::towlower);
		return {std::move(path)};
	}

	bool is_subpath(const std::filesystem::path& root, const std::filesystem::path& p)
	{
		auto root_it = root.begin();
		auto p_it = p.begin();

		for (; root_it != root.end(); ++root_it, ++p_it)
		{
			if (p_it == p.end() || *root_it != *p_it)
			{
				return false;
			}
		}

		return true;
	}

	void register_hive(registry_manager::hive_map& hives,
	                   const std::filesystem::path& key, const std::filesystem::path& file)
	{
		auto hive = std::make_unique<hive_parser>(file);
		if (hive && hive->success())
		{
			hives[canonicalize_path(key)] = std::move(hive);
		}
	}
}

registry_manager::~registry_manager() = default;

registry_manager::registry_manager(const std::filesystem::path& hive_path)
{
	const std::filesystem::path root = R"(\registry)";
	const std::filesystem::path machine = root / "machine";

	register_hive(this->hives_, machine / "system", hive_path / "SYSTEM");
	register_hive(this->hives_, machine / "security", hive_path / "SECURITY");
	register_hive(this->hives_, machine / "sam", hive_path / "SAM");
	register_hive(this->hives_, machine / "software", hive_path / "SOFTWARE");
	register_hive(this->hives_, machine / "system", hive_path / "SYSTEM");
	register_hive(this->hives_, machine / "hardware", hive_path / "HARDWARE");

	register_hive(this->hives_, root / "user", hive_path / "NTUSER.dat");

	this->add_path_mapping(machine / "system" / "CurrentControlSet", machine / "system" / "ControlSet001");
}

void registry_manager::serialize(utils::buffer_serializer& buffer) const
{
	(void)buffer;
}

void registry_manager::deserialize(utils::buffer_deserializer& buffer)
{
	(void)buffer;
}

std::filesystem::path registry_manager::normalize_path(const std::filesystem::path& path) const
{
	auto canonical_path = canonicalize_path(path);

	for (const auto& mapping : this->path_mapping_)
	{
		if (is_subpath(mapping.first, canonical_path))
		{
			return mapping.second / canonical_path.lexically_relative(mapping.first);
		}
	}

	return canonical_path;
}

void registry_manager::add_path_mapping(const std::filesystem::path& key, const std::filesystem::path& value)
{
	this->path_mapping_[canonicalize_path(key)] = canonicalize_path(value);
}

std::optional<registry_key> registry_manager::get_key(const std::filesystem::path& key)
{
	const auto normal_key = this->normalize_path(key);
	const auto iterator = this->find_hive(normal_key);
	if (iterator == this->hives_.end())
	{
		return {};
	}

	registry_key reg_key{};
	reg_key.hive = iterator->first;
	reg_key.path = normal_key.lexically_relative(reg_key.hive);

	if (reg_key.path.empty())
	{
		return {std::move(reg_key)};
	}

	const auto entry = iterator->second->get_subkey(reg_key.path.begin()->string(), reg_key.path.generic_string());
	if (!entry)
	{
		return {};
	}

	return {std::move(reg_key)};
}

std::optional<registry_value> registry_manager::get_value(const registry_key& key, const std::string_view name)
{
	const auto iterator = this->hives_.find(key.hive);
	if (iterator == this->hives_.end())
	{
		return {};
	}

	auto entry = iterator->second->get_subkey(key.path.begin()->string(), key.path.generic_string());
	if (!entry)
	{
		return {};
	}

	const auto value = entry->get_key_value(name);
	if (!value)
	{
		return {};
	}

	registry_value v{};
	v.type = value->first;
	v.data = value->second;

	return v;
}

registry_manager::hive_map::iterator registry_manager::find_hive(const std::filesystem::path& key)
{
	for (auto i = this->hives_.begin(); i != this->hives_.end(); ++i)
	{
		if (is_subpath(i->first, key))
		{
			return i;
		}
	}

	return this->hives_.end();
}
