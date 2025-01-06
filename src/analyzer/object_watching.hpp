#pragma once

#include "reflect_type_info.hpp"

template <typename T>
emulator_hook* watch_object(windows_emulator& emu, emulator_object<T> object, const bool cache_logging = false)
{
    const reflect_type_info<T> info{};

    return emu.emu().hook_memory_read(
        object.value(), object.size(),
        [i = std::move(info), object, &emu, cache_logging](const uint64_t address, size_t, uint64_t) {
            const auto rip = emu.emu().read_instruction_pointer();
            const auto* mod = emu.process().mod_manager.find_by_address(rip);
            const auto is_main_access = mod == emu.process().executable;

            if (!emu.verbose_calls && !is_main_access)
            {
                return;
            }

            if (cache_logging)
            {
                static std::unordered_set<uint64_t> logged_addresses{};
                if (is_main_access && !logged_addresses.insert(address).second)
                {
                    return;
                }
            }

            const auto offset = address - object.value();
            emu.log.print(is_main_access ? color::green : color::dark_gray,
                          "Object access: %s - 0x%llX (%s) at 0x%llX (%s)\n", i.get_type_name().c_str(), offset,
                          i.get_member_name(offset).c_str(), rip, mod ? mod->name.c_str() : "<N/A>");
        });
}
