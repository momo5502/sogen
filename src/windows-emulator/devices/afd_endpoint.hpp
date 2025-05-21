#pragma once
#include "../io_device.hpp"

std::unique_ptr<io_device> create_afd_endpoint();
std::unique_ptr<io_device> create_afd_async_connect_hlp();
