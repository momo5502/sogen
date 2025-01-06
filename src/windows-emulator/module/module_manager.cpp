#include "../std_include.hpp"
#include "module_manager.hpp"
#include "module_mapping.hpp"
#include "windows-emulator/logger.hpp"

namespace
{
    std::filesystem::path canonicalize_module_path(const std::filesystem::path& file)
    {
        constexpr std::u16string_view nt_prefix = u"\\??\\";
        const auto wide_file = file.u16string();

        if (!wide_file.starts_with(nt_prefix))
        {
            return canonical(absolute(file));
        }

        return canonicalize_module_path(wide_file.substr(nt_prefix.size()));
    }
}

namespace utils
{
    static void serialize(buffer_serializer& buffer, const exported_symbol& sym)
    {
        buffer.write(sym.name);
        buffer.write(sym.ordinal);
        buffer.write(sym.rva);
        buffer.write(sym.address);
    }

    static void deserialize(buffer_deserializer& buffer, exported_symbol& sym)
    {
        buffer.read(sym.name);
        buffer.read(sym.ordinal);
        buffer.read(sym.rva);
        buffer.read(sym.address);
    }

    static void serialize(buffer_serializer& buffer, const mapped_module& mod)
    {
        buffer.write_string(mod.name);
        buffer.write(mod.path.u16string());

        buffer.write(mod.image_base);
        buffer.write(mod.size_of_image);
        buffer.write(mod.entry_point);

        buffer.write_vector(mod.exports);
        buffer.write_map(mod.address_names);
    }

    static void deserialize(buffer_deserializer& buffer, mapped_module& mod)
    {
        mod.name = buffer.read_string();
        mod.path = buffer.read_string<std::u16string::value_type>();

        buffer.read(mod.image_base);
        buffer.read(mod.size_of_image);
        buffer.read(mod.entry_point);

        buffer.read_vector(mod.exports);
        buffer.read_map(mod.address_names);
    }
}

module_manager::module_manager(emulator& emu)
    : emu_(&emu)
{
}

mapped_module* module_manager::map_module(const std::filesystem::path& file, logger& logger)
{
    auto canonical_file = canonicalize_module_path(file);

    for (auto& mod : this->modules_)
    {
        if (mod.second.path == canonical_file)
        {
            return &mod.second;
        }
    }

    try
    {
        auto mod = map_module_from_file(*this->emu_, std::move(canonical_file));

        logger.log("Mapped %s at 0x%" PRIx64 "\n", mod.path.generic_string().c_str(), mod.image_base);

        const auto image_base = mod.image_base;
        const auto entry = this->modules_.try_emplace(image_base, std::move(mod));
        return &entry.first->second;
    }
    catch (const std::exception& e)
    {
        logger.error("Failed to map %s: %s\n", file.generic_string().c_str(), e.what());
        return nullptr;
    }
    catch (...)
    {
        logger.error("Failed to map %s: Unknown error\n", file.generic_string().c_str());
        return nullptr;
    }
}

void module_manager::serialize(utils::buffer_serializer& buffer) const
{
    buffer.write_map(this->modules_);
}

void module_manager::deserialize(utils::buffer_deserializer& buffer)
{
    buffer.read_map(this->modules_);
}

bool module_manager::unmap(const uint64_t address)
{
    const auto mod = this->modules_.find(address);
    if (mod == this->modules_.end())
    {
        return false;
    }

    unmap_module(*this->emu_, mod->second);
    this->modules_.erase(mod);

    return true;
}
