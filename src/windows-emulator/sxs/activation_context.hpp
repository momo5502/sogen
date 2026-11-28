#pragma once

#include "../file_system.hpp"
#include "../module/mapped_module.hpp"
#include <span>

namespace sogen::sxs
{
    struct activation_context_source
    {
        std::span<const std::byte> contents;
        windows_path path;
    };

    std::vector<std::byte> build_activation_context(const file_system& file_system, const mapped_module& executable,
                                                    const activation_context_source& manifest,
                                                    const std::optional<activation_context_source>& config, const windows_path& system_root,
                                                    std::string_view preferred_language);

    enum class system_default_architecture
    {
        amd64,
        x86,
    };

    std::vector<std::byte> obtain_system_default_activation_context(const std::filesystem::path& emulation_root,
                                                                    system_default_architecture architecture);

    std::vector<std::byte> build_process_activation_context(const file_system& file_system, const mapped_module& executable,
                                                            const windows_path& system_root, std::string_view preferred_language);
}
