#pragma once

#include "reflect_type_info.hpp"
#include <set>
#include <cinttypes>

template <typename T>
emulator_hook* watch_object(windows_emulator& emu, const std::set<std::string, std::less<>>& modules, emulator_object<T> object,
                            const auto verbose)
{
    const reflect_type_info<T> info{};

    return emu.emu().hook_memory_read(
        object.value(), static_cast<size_t>(object.size()),
        [i = std::move(info), object, &emu, verbose, modules](const uint64_t address, const void*, const size_t size) {
            const auto rip = emu.emu().read_instruction_pointer();
            const auto* mod = emu.mod_manager.find_by_address(rip);
            const auto is_main_access = !mod || (mod == emu.mod_manager.executable || modules.contains(mod->name));

            if (!verbose && !is_main_access)
            {
                return;
            }

            if (!verbose)
            {
                static std::unordered_set<uint64_t> logged_addresses{};

                bool is_new = false;
                for (size_t j = 0; j < size; ++j)
                {
                    is_new |= logged_addresses.insert(address + j).second;
                }

                if (!is_new)
                {
                    return;
                }
            }

            const auto start_offset = address - object.value();
            const auto end_offset = start_offset + size;
            const auto* mod_name = mod ? mod->name.c_str() : "<N/A>";
            const auto& type_name = i.get_type_name();

            for (auto offset = start_offset; offset < end_offset;)
            {
                const auto member_info = i.get_member_info(static_cast<size_t>(offset));
                if (!member_info.has_value())
                {
                    const auto remaining_size = end_offset - offset;
                    emu.log.print(is_main_access ? color::green : color::dark_gray,
                                  "Object access: %s - 0x%" PRIx64 " 0x%" PRIx64 " (<N/A>) at 0x%" PRIx64 " (%s)\n", type_name.c_str(),
                                  offset, remaining_size, rip, mod_name);
                    break;
                }

                const auto remaining_size = end_offset - offset;
                const auto member_end = member_info->offset + member_info->size;
                const auto member_access_size = member_end - offset;
                const auto access_size = std::min(remaining_size, member_access_size);

                emu.log.print(is_main_access ? color::green : color::dark_gray,
                              "Object access: %s - 0x%" PRIx64 " 0x%" PRIx64 " (%s) at 0x%" PRIx64 " (%s)\n", type_name.c_str(), offset,
                              access_size, member_info->get_diff_name(static_cast<size_t>(offset)).c_str(), rip, mod_name);

                offset = member_end;
            }
        });
}

template <typename T>
emulator_hook* watch_object(windows_emulator& emu, const std::set<std::string, std::less<>>& modules, const uint64_t address,
                            const auto verbose)
{
    return watch_object<T>(emu, modules, emulator_object<T>{emu.emu(), address}, verbose);
}
