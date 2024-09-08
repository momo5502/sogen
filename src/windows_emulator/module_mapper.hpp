#pragma once

#include "process_context.hpp"
#include <x64_emulator.hpp>

mapped_binary* map_file(process_context& context, x64_emulator& emu, std::filesystem::path file);
