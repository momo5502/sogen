#pragma once
#include "memory_permission.hpp"

struct memory_region
{
	uint64_t start;
	size_t length;
	memory_permission pemissions;
};
