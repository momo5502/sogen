#include "../std_include.hpp"
#include "module_manager.hpp"
#include "module_mapping.hpp"

module_manager::module_manager(emulator& emu)
	: emu_(&emu)
{
}

mapped_module* module_manager::map_module(std::filesystem::path file)
{
	auto mod = map_module_from_file(*this->emu_, std::move(file));
	if (!mod)
	{
		return nullptr;
	}

	const auto image_base = mod->image_base;
	const auto entry = this->modules_.try_emplace(image_base, std::move(*mod));
	return &entry.first->second;
}
