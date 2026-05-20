#pragma once

#include "../port.hpp"

namespace sogen
{

    std::unique_ptr<port> create_lsa_policy_lookup_port();

} // namespace sogen
