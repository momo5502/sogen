#pragma once

#include "reflect_type_info.hpp"

template <typename T>
emulator_hook* watch_object(windows_emulator& emu, emulator_object<T> object)
{
	const reflect_type_info<T> info{};

	return emu.emu().hook_memory_read(object.value(), object.size(),
	                                  [i = std::move(info), object, &emu](const uint64_t address, size_t, uint64_t)
	                                  {
		                                  const auto rip = emu.emu().read_instruction_pointer();

		                                  const auto offset = address - object.value();
		                                  printf("%s: %llX (%s) at %llX (%s)\n", i.get_type_name().c_str(), offset,
		                                         i.get_member_name(offset).c_str(), rip,
		                                         emu.process().module_manager.find_name(rip));
	                                  });
}
