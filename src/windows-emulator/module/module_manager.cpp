#include "../std_include.hpp"
#include "module_manager.hpp"
#include "module_mapping.hpp"
#include "windows-emulator/logger.hpp"

static void serialize(utils::buffer_serializer& buffer, const exported_symbol& sym)
{
	buffer.write(sym.name);
	buffer.write(sym.ordinal);
	buffer.write(sym.rva);
	buffer.write(sym.address);
}

static void deserialize(utils::buffer_deserializer& buffer, exported_symbol& sym)
{
	buffer.read(sym.name);
	buffer.read(sym.ordinal);
	buffer.read(sym.rva);
	buffer.read(sym.address);
}

static void serialize(utils::buffer_serializer& buffer, const mapped_module& mod)
{
	buffer.write_string(mod.name);
	buffer.write_string(mod.path.wstring());

	buffer.write(mod.image_base);
	buffer.write(mod.size_of_image);
	buffer.write(mod.entry_point);

	buffer.write_vector(mod.exports);
	buffer.write_map(mod.address_names);
}

static void deserialize(utils::buffer_deserializer& buffer, mapped_module& mod)
{
	mod.name = buffer.read_string();
	mod.path = buffer.read_string<wchar_t>();

	buffer.read(mod.image_base);
	buffer.read(mod.size_of_image);
	buffer.read(mod.entry_point);

	buffer.read_vector(mod.exports);
	buffer.read_map(mod.address_names);
}

module_manager::module_manager(emulator& emu)
	: emu_(&emu)
{
}

mapped_module* module_manager::map_module(const std::filesystem::path& file, logger& logger)
{
	for (auto& mod : this->modules_)
	{
		if (mod.second.path == file)
		{
			return &mod.second;
		}
	}

	auto mod = map_module_from_file(*this->emu_, file);
	if (!mod)
	{
		logger.error("Failed to map %s\n", file.generic_string().c_str());
		return nullptr;
	}

	logger.log("Mapped %s at 0x%llX\n", mod->path.generic_string().c_str(), mod->image_base);

	const auto image_base = mod->image_base;
	const auto entry = this->modules_.try_emplace(image_base, std::move(*mod));
	return &entry.first->second;
}

void module_manager::serialize(utils::buffer_serializer& buffer) const
{
	buffer.write_map(this->modules_);
}

void module_manager::deserialize(utils::buffer_deserializer& buffer)
{
	buffer.read_map(this->modules_);
}
