#pragma once

#include "reflect_type_info.hpp"

template <typename T>
emulator_hook* watch_object(windows_emulator& emu, emulator_object<T> object)
{
	const reflect_type_info<T> info{};

	return emu.emu().hook_memory_read(object.value(), object.size(),
	                                  [i = std::move(info), object, &emu](
	                                  const uint64_t address, size_t, uint64_t)
	                                  {
		                                  const auto rip = emu.emu().read_instruction_pointer();
		                                  const auto* mod = emu.process().module_manager.find_by_address(rip);
		                                  const auto is_main_access = mod == emu.process().executable;

		                                  if (!emu.verbose_calls && !is_main_access)
		                                  {
			                                  return;
		                                  }

		                                  const auto offset = address - object.value();
		                                  emu.logger.print(is_main_access ? color::green : color::dark_gray,
		                                                   "Object access: %s - 0x%llX (%s) at 0x%llX (%s)\n",
		                                                   i.get_type_name().c_str(),
		                                                   offset,
		                                                   i.get_member_name(offset).c_str(), rip,
		                                                   mod ? mod->name.c_str() : "<N/A>");
	                                  });
}
