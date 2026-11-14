#pragma once

#include "../file_system.hpp"
#include "../module/mapped_module.hpp"
#include <functional>
#include <span>

namespace sogen::sxs
{
    // Reads the contents a guest image maps, which an image override takes from another file than the image's own.
    using image_reader = std::function<bool(const windows_path& image, std::vector<std::byte>& data)>;

    struct activation_context_source
    {
        std::span<const std::byte> contents;
        windows_path path;
    };

    std::vector<std::byte> build_activation_context(const file_system& file_system, const image_reader& read_image,
                                                    const mapped_module& executable, const activation_context_source& manifest,
                                                    const std::optional<activation_context_source>& config, const windows_path& system_root,
                                                    std::string_view preferred_language);

    std::vector<std::byte> build_process_activation_context(const file_system& file_system, const image_reader& read_image,
                                                            const mapped_module& executable, const windows_path& system_root,
                                                            std::string_view preferred_language);
}
