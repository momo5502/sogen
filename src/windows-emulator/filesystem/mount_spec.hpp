#pragma once

#include "filesystem_mode.hpp"
#include "../windows_path.hpp"

namespace sogen
{

    struct mount_spec
    {
        std::filesystem::path host_path{};
        windows_path guest_path{};
        filesystem_mode mode{default_mount_mode};
    };

    // Docker-style "HOST:GUEST[:MODE]". Both host and guest paths may contain colons, so the
    // specification is split from the right: an optional mode, then the guest drive ("X:").
    mount_spec parse_mount_spec(std::string_view spec);
    std::string render_mount_spec(const mount_spec& spec);

} // namespace sogen
