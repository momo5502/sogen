#pragma once
#include "../port.hpp"

namespace sogen
{

    std::unique_ptr<port> create_dns_resolver();

} // namespace sogen
