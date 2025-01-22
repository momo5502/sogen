#include "registry_manager.hpp"

#include <serialization_helper.hpp>

#include "hive_parser.hpp"
#include <utils/string.hpp>

namespace
{
    bool is_subpath(const utils::path_key& root, const utils::path_key& p)
    {
        auto root_it = root.get().begin();
        auto p_it = p.get().begin();

        for (; root_it != root.get().end(); ++root_it, ++p_it)
        {
            if (p_it == p.get().end() || *root_it != *p_it)
            {
                return false;
            }
        }

        return true;
    }

    void register_hive(registry_manager::hive_map& hives, const utils::path_key& key, const std::filesystem::path& file)
    {
        hives[key] = std::make_unique<hive_parser>(file);
    }
}

registry_manager::registry_manager() = default;
registry_manager::~registry_manager() = default;
registry_manager::registry_manager(registry_manager&&) noexcept = default;
registry_manager& registry_manager::operator=(registry_manager&&) noexcept = default;

registry_manager::registry_manager(const std::filesystem::path& hive_path)
    : hive_path_(absolute(hive_path))
{
    this->setup();
}

void registry_manager::setup()
{
    this->path_mapping_.clear();
    this->hives_.clear();

    const std::filesystem::path root = R"(\registry)";
    const std::filesystem::path machine = root / "machine";

    register_hive(this->hives_, machine / "system", this->hive_path_ / "SYSTEM");
    register_hive(this->hives_, machine / "security", this->hive_path_ / "SECURITY");
    register_hive(this->hives_, machine / "sam", this->hive_path_ / "SAM");
    register_hive(this->hives_, machine / "software", this->hive_path_ / "SOFTWARE");
    register_hive(this->hives_, machine / "system", this->hive_path_ / "SYSTEM");
    register_hive(this->hives_, machine / "hardware", this->hive_path_ / "HARDWARE");

    register_hive(this->hives_, root / "user", this->hive_path_ / "NTUSER.DAT");

    this->add_path_mapping(machine / "system" / "CurrentControlSet", machine / "system" / "ControlSet001");
}

void registry_manager::serialize(utils::buffer_serializer& buffer) const
{
    buffer.write(this->hive_path_);
}

void registry_manager::deserialize(utils::buffer_deserializer& buffer)
{
    buffer.read(this->hive_path_);
    this->setup();
}

utils::path_key registry_manager::normalize_path(const utils::path_key& path) const
{
    const utils::path_key canonical_path = path;

    for (const auto& mapping : this->path_mapping_)
    {
        if (is_subpath(mapping.first.get(), canonical_path.get()))
        {
            return mapping.second.get() / canonical_path.get().lexically_relative(mapping.first.get());
        }
    }

    return canonical_path.get();
}

void registry_manager::add_path_mapping(const utils::path_key& key, const utils::path_key& value)
{
    this->path_mapping_[key] = value;
}

std::optional<registry_key> registry_manager::get_key(const utils::path_key& key)
{
    const auto normal_key = this->normalize_path(key);

    if (is_subpath(normal_key, utils::path_key{"\\registry\\machine"}))
    {
        registry_key reg_key{};
        reg_key.hive = normal_key;
        return {std::move(reg_key)};
    }

    const auto iterator = this->find_hive(normal_key);
    if (iterator == this->hives_.end())
    {
        return {};
    }

    registry_key reg_key{};
    reg_key.hive = iterator->first.get();
    reg_key.path = normal_key.get().lexically_relative(reg_key.hive.get());

    if (reg_key.path.get().empty())
    {
        return {std::move(reg_key)};
    }

    const auto entry = iterator->second->get_sub_key(reg_key.path.get());
    if (!entry)
    {
        return std::nullopt;
    }

    return {std::move(reg_key)};
}

std::optional<registry_value> registry_manager::get_value(const registry_key& key, std::string name)
{
    utils::string::to_lower_inplace(name);

    const auto iterator = this->hives_.find(key.hive);
    if (iterator == this->hives_.end())
    {
        return std::nullopt;
    }

    auto* entry = iterator->second->get_value(key.path.get(), name);
    if (!entry)
    {
        return std::nullopt;
    }

    registry_value v{};
    v.type = entry->type;
    v.name = entry->name;
    v.data = entry->data;

    return v;
}

registry_manager::hive_map::iterator registry_manager::find_hive(const utils::path_key& key)
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
