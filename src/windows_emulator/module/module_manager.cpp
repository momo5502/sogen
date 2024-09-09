#include "../std_include.hpp"
#include "module_manager.hpp"
#include "module_mapping.hpp"

module_manager::module_manager(emulator& emu)
	: emu_(&emu)
{
}

mapped_module* module_manager::map_module(const std::filesystem::path& file)
{
	auto mod = map_module_from_file(*this->emu_, file);
	if (!mod)
	{
		printf("Failed to map %s\n", file.generic_string().c_str());
		return nullptr;
	}

	printf("Mapped %s at %llX\n", mod->path.generic_string().c_str(), mod->image_base);

	const auto image_base = mod->image_base;
	const auto entry = this->modules_.try_emplace(image_base, std::move(*mod));
	return &entry.first->second;
}
