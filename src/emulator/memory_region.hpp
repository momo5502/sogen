#pragma once
#include "memory_permission.hpp"
#include <cstddef>

struct basic_memory_region
{
    uint64_t start{};
    size_t length{};
    memory_permission permissions{};
};

struct memory_region : basic_memory_region
{
    bool committed{};
};
