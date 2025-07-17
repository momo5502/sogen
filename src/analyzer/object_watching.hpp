#pragma once

#include "reflect_type_info.hpp"
#include <set>
#include <cinttypes>

template <typename T>
emulator_hook* watch_object(windows_emulator& emu, const std::set<std::string, std::less<>>& modules,
                            emulator_object<T> object, const auto verbose)
{
    const reflect_type_info<T> info{};

    return emu.emu().hook_memory_read(
        object.value(), static_cast<size_t>(object.size()),
        [i = std::move(info), object, &emu, verbose, modules](const uint64_t address, const void*, size_t) {
            const auto rip = emu.emu().read_instruction_pointer();
            const auto* mod = emu.mod_manager.find_by_address(rip);
            const auto is_main_access = mod == emu.mod_manager.executable || modules.contains(mod->name);

            if (!verbose && !is_main_access)
            {
                return;
            }

            if (!verbose)
            {
                static std::unordered_set<uint64_t> logged_addresses{};
                if (is_main_access && !logged_addresses.insert(address).second)
                {
                    return;
                }
            }

            const auto offset = address - object.value();
            const auto* mod_name = mod ? mod->name.c_str() : "<N/A>";
            const auto& type_name = i.get_type_name();
            const auto member_name = i.get_member_name(static_cast<size_t>(offset));

            emu.log.print(is_main_access ? color::green : color::dark_gray,
                          "Object access: %s - 0x%" PRIx64 " (%s) at 0x%" PRIx64 " (%s)\n", type_name.c_str(), offset,
                          member_name.c_str(), rip, mod_name);
        });
}
