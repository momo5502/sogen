#include "../std_include.hpp"
#include "registry_manager.hpp"

#include <serialization_helper.hpp>

#include "hive_parser.hpp"

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

    void register_optional_hive(registry_manager::hive_map& hives, const utils::path_key& key, const std::filesystem::path& file)
    {
        if (!std::filesystem::is_regular_file(file))
        {
            return;
        }

        hives[key] = std::make_unique<hive_parser>(file);
    }

    std::pair<utils::path_key, bool> perform_path_substitution(const std::map<utils::path_key, utils::path_key>& path_mapping,
                                                               utils::path_key path)
    {
        for (const auto& mapping : path_mapping)
        {
            if (path == mapping.first)
            {
                return {mapping.second, true};
            }

            if (is_subpath(mapping.first.get(), path.get()))
            {
                return {mapping.second.get() / path.get().lexically_relative(mapping.first.get()), true};
            }
        }

        return {std::move(path), false};
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
    this->overlay_values_.clear();
    this->hives_.clear();

    const std::filesystem::path root = R"(\registry)";
    const std::filesystem::path machine = root / "machine";

    register_hive(this->hives_, machine / "system", this->hive_path_ / "SYSTEM");
    register_hive(this->hives_, machine / "security", this->hive_path_ / "SECURITY");
    register_hive(this->hives_, machine / "sam", this->hive_path_ / "SAM");
    register_hive(this->hives_, machine / "software", this->hive_path_ / "SOFTWARE");
    register_hive(this->hives_, machine / "system", this->hive_path_ / "SYSTEM");
    register_optional_hive(this->hives_, machine / "hardware", this->hive_path_ / "HARDWARE");

    register_hive(this->hives_, root / "user", this->hive_path_ / "NTUSER.DAT");

    this->add_path_mapping(machine / "system" / "CurrentControlSet", machine / "system" / "ControlSet001");
    this->add_path_mapping(machine / "system" / "ControlSet001" / "Control" / "ComputerName" / "ActiveComputerName",
                           machine / "system" / "ControlSet001" / "Control" / "ComputerName" / "ComputerName");
}

utils::path_key registry_manager::normalize_path(utils::path_key path) const
{
    for (size_t i = 0; i < 10; ++i)
    {
        auto [new_path, changed] = perform_path_substitution(this->path_mapping_, std::move(path));
        path = std::move(new_path);

        if (!changed)
        {
            break;
        }
    }

    return path;
}

void registry_manager::add_path_mapping(const utils::path_key& key, const utils::path_key& value)
{
    this->path_mapping_[key] = value;
}

utils::path_key registry_manager::get_full_key_path(const registry_key& key)
{
    return utils::path_key{key.hive.get() / key.path.get()};
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

    auto path = reg_key.path.get();
    const auto* entry = iterator->second->get_sub_key(path);

    if (!entry)
    {
        constexpr std::wstring_view wowPrefix = L"wow6432node";

        const auto pathStr = path.wstring();
        if (pathStr.starts_with(wowPrefix))
        {
            path = pathStr.substr(wowPrefix.size() + 1);
            reg_key.path = path;
            entry = iterator->second->get_sub_key(path);
        }
    }

    if (!entry)
    {
        return std::nullopt;
    }

    return {std::move(reg_key)};
}

std::optional<registry_value> registry_manager::get_value(const registry_key& key, const std::string_view name)
{
    if (const auto overlay_entry = this->overlay_values_.find(registry_manager::get_full_key_path(key));
        overlay_entry != this->overlay_values_.end())
    {
        if (const auto value_entry = overlay_entry->second.values.find(std::string{name});
            value_entry != overlay_entry->second.values.end())
        {
            registry_value v{};
            v.type = value_entry->second.type;
            v.name = value_entry->first;
            v.data = value_entry->second.data;
            return v;
        }
    }

    const auto iterator = this->hives_.find(key.hive);
    if (iterator == this->hives_.end())
    {
        return std::nullopt;
    }

    const auto* entry = iterator->second->get_value(key.path.get(), name);
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

std::optional<registry_value> registry_manager::get_value(const registry_key& key, const size_t index)
{
    const auto iterator = this->hives_.find(key.hive);
    if (iterator == this->hives_.end())
    {
        return std::nullopt;
    }

    const auto* entry = iterator->second->get_value(key.path.get(), index);
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

void registry_manager::set_value(const registry_key& key, std::string name, const uint32_t type, const std::span<const std::byte> data)
{
    auto& bucket = this->overlay_values_[registry_manager::get_full_key_path(key)];
    auto& value = bucket.values[std::move(name)];
    value.type = type;
    value.data.assign(data.begin(), data.end());
}

void registry_manager::serialize_runtime_state(utils::buffer_serializer& buffer) const
{
    buffer.write_map(this->overlay_values_);
}

void registry_manager::deserialize_runtime_state(utils::buffer_deserializer& buffer)
{
    buffer.read_map(this->overlay_values_);
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

std::optional<exposed_hive_key> registry_manager::get_hive_key(const registry_key& key)
{
    const auto iterator = this->hives_.find(key.hive);
    if (iterator == this->hives_.end())
    {
        return std::nullopt;
    }

    auto* hive_key = iterator->second->get_sub_key(key.path.get());
    if (!hive_key)
    {
        return std::nullopt;
    }

    return exposed_hive_key{
        .key = *hive_key,
        .file = iterator->second->get_file(),
    };
}

std::optional<std::string_view> registry_manager::get_sub_key_name(const registry_key& key, const size_t index)
{
    const auto iterator = this->hives_.find(key.hive);
    if (iterator == this->hives_.end())
    {
        return std::nullopt;
    }

    const auto* name = iterator->second->get_sub_key_name(key.path.get(), index);
    if (!name)
    {
        return std::nullopt;
    }

    return *name;
}

std::optional<std::u16string> registry_manager::read_u16string(const registry_key& key, size_t index)
{
    const auto value_opt = this->get_value(key, index);
    if (!value_opt)
    {
        return {};
    }

    const auto& value = value_opt.value();

    if (value.type != REG_SZ && value.type != REG_EXPAND_SZ)
    {
        return {};
    }

    if (value.data.empty() || value.data.size() % 2 != 0)
    {
        return {};
    }

    const auto char_count = value.data.size() / sizeof(char16_t);
    const auto* data_ptr = reinterpret_cast<const char16_t*>(value.data.data());
    if (data_ptr[char_count - 1] != u'\0')
    {
        return {};
    }

    auto s = std::u16string(data_ptr, char_count - 1);
    return s;
}
