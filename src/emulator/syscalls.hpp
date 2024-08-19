#pragma once

#include "unicorn.hpp"
#include "process_context.hpp"

void handle_syscall(const unicorn& uc, process_context& context);
