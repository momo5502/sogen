#pragma once

#include <x64_emulator.hpp>
#include "mapped_module.hpp"

mapped_module map_module_from_data(emulator& emu, std::span<const uint8_t> data, std::filesystem::path file);
mapped_module map_module_from_file(emulator& emu, std::filesystem::path file);

bool unmap_module(emulator& emu, const mapped_module& mod);
