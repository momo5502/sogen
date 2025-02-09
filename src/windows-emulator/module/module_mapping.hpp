#pragma once

#include "mapped_module.hpp"
#include "../memory_manager.hpp"

mapped_module map_module_from_data(memory_manager& memory, std::span<const uint8_t> data, std::filesystem::path file);
mapped_module map_module_from_file(memory_manager& memory, std::filesystem::path file);

bool unmap_module(memory_manager& memory, const mapped_module& mod);
