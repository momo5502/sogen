#pragma once
#include "memory_permission.hpp"
#include <cstddef>

template<typename PermissionType = memory_permission>
struct basic_memory_region
{
    uint64_t start{};
    size_t length{}; // uint64_t?
    PermissionType permissions{};
};

struct memory_region : basic_memory_region<>
{
    bool committed{};
};
