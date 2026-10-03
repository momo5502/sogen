#pragma once

#include "../file_system.hpp"
#include "../module/mapped_module.hpp"

namespace sogen::sxs
{
    std::vector<std::byte> build_process_activation_context(const file_system& file_system, const mapped_module& executable,
                                                            const windows_path& system_root, std::string_view preferred_language);
}
