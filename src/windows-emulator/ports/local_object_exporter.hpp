#pragma once

#include <memory>

namespace sogen
{
    struct port;
    std::unique_ptr<port> create_local_object_exporter_port();
}
