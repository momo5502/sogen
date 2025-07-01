#pragma once
#include "windows-emulator/memory_permission_ext.hpp"
#include <cstddef>

struct basic_memory_region
{
    uint64_t start{};
    size_t length{}; // uint64_t?
    nt_memory_permission permissions{};
};

struct memory_region : basic_memory_region
{
    bool committed{};
};
