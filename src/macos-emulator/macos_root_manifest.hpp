#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sogen
{
    constexpr std::string_view MACOS_ROOT_MANIFEST_NAME = "sogen-macos-root.manifest";
    constexpr size_t MAX_MANIFEST_SIZE = 64u * 1024u;
    constexpr uint32_t MACOS_ROOT_MANIFEST_SCHEMA = 1;

    struct macos_root_manifest
    {
        uint32_t schema{};
        std::string tool{};
        std::string tool_version{};
        std::string created{};
        std::string mode{};
        std::string arch{};
        std::string product_name{};
        std::string product_version{};
        std::string build_version{};
        std::string kernel_version{};
        std::string hardware_model{};
        std::string cache_guest_dir{};
        std::string cache_base_name{};
        std::string cache_uuid{};
        uint32_t cache_file_count{};
        uint64_t cache_total_bytes{};
        std::string dyld_guest_path{};
        std::string dyld_sha256{};
        std::vector<std::string> optional_items{};
    };

    std::optional<macos_root_manifest> parse_macos_root_manifest(std::span<const std::byte> data);

    std::optional<macos_root_manifest> load_macos_root_manifest(const std::filesystem::path& root);

    std::string format_macos_root_manifest(const macos_root_manifest& manifest);

    std::string describe_macos_root_manifest(const macos_root_manifest& manifest);
}
