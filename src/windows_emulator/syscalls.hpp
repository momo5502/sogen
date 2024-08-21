#pragma once

#include <x64_emulator.hpp>
#include "process_context.hpp"

void handle_syscall(x64_emulator& emu, process_context& context);
