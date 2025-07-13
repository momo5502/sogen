#pragma once

#include <memory>
#include <arch_emulator.hpp>

std::unique_ptr<x86_64_emulator> create_x86_64_emulator();
