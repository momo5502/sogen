#pragma once
#include "../io_device.hpp"

namespace sogen
{

    std::unique_ptr<io_device> create_afd_endpoint(bool is_32_bit);
    std::unique_ptr<io_device> create_afd_async_connect_hlp(bool is_32_bit);

} // namespace sogen
