#pragma once

#include "reflect_type_info.hpp"
#include <optional>
#include <string>
#include <set>
#include <memory>
#include <cinttypes>

struct object_access_info
{
    bool main_access{};
    std::string type_name{};
    uint64_t offset{};
    uint64_t size{};
    std::optional<std::string> member_name{};
    uint64_t rip{};
    std::string module_name{};
};

struct object_watching_state
{
    std::unordered_set<uint64_t> logged_addresses{};
};

template <typename T, typename Callback>
emulator_hook* watch_object(windows_emulator& emu, const std::set<std::string, std::less<>>& modules, emulator_object<T> object,
                            const auto verbose, Callback&& on_access,
                            std::shared_ptr<object_watching_state> shared_state = std::make_unique<object_watching_state>())
{
    const reflect_type_info<T> info{};

    return emu.emu().hook_memory_read(
        object.value(), static_cast<size_t>(object.size()),
        [i = std::move(info), object, &emu, verbose, modules, state = std::move(shared_state),
         on_access = std::forward<Callback>(on_access)](const uint64_t address, const void*, const size_t size) {
            const auto rip = emu.emu().read_instruction_pointer();
            const auto* mod = emu.mod_manager.find_by_address(rip);
            const auto is_main_access = !mod || (mod == emu.mod_manager.executable || modules.contains(mod->name));

            if (!verbose && !is_main_access)
            {
                return;
            }

            if (!verbose)
            {
                bool is_new = false;
                for (size_t j = 0; j < size; ++j)
                {
                    is_new |= state->logged_addresses.insert(address + j).second;
                }

                if (!is_new)
                {
                    return;
                }
            }

            const auto start_offset = address - object.value();
            const auto end_offset = start_offset + size;
            const auto mod_name = mod ? mod->name : "<N/A>"s;
            const auto& type_name = i.get_type_name();

            for (auto offset = start_offset; offset < end_offset;)
            {
                const auto member_info = i.get_member_info(static_cast<size_t>(offset));
                if (!member_info.has_value())
                {
                    const auto remaining_size = end_offset - offset;
                    on_access(object_access_info{
                        .main_access = is_main_access,
                        .type_name = type_name,
                        .offset = offset,
                        .size = remaining_size,
                        .member_name = std::nullopt,
                        .rip = rip,
                        .module_name = mod_name,
                    });
                    break;
                }

                const auto remaining_size = end_offset - offset;
                const auto member_end = member_info->offset + member_info->size;
                const auto member_access_size = member_end - offset;
                const auto access_size = std::min(remaining_size, member_access_size);

                on_access(object_access_info{
                    .main_access = is_main_access,
                    .type_name = type_name,
                    .offset = offset,
                    .size = access_size,
                    .member_name = member_info->get_diff_name(static_cast<size_t>(offset)),
                    .rip = rip,
                    .module_name = mod_name,
                });

                offset = member_end;
            }
        });
}

template <typename T, typename Callback>
emulator_hook* watch_object(windows_emulator& emu, const std::set<std::string, std::less<>>& modules, const uint64_t address,
                            const auto verbose, Callback&& on_access,
                            std::shared_ptr<object_watching_state> state = std::make_unique<object_watching_state>())
{
    return watch_object<T>(emu, modules, emulator_object<T>{emu.emu(), address}, verbose, std::forward<Callback>(on_access),
                           std::move(state));
}
