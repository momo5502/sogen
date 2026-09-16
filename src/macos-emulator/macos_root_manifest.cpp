#include "macos_root_manifest.hpp"

#include <charconv>

#include <utils/io.hpp>

namespace sogen
{
    namespace
    {
        std::string_view trim(std::string_view value)
        {
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t' || value.front() == '\r'))
            {
                value.remove_prefix(1);
            }
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r'))
            {
                value.remove_suffix(1);
            }
            return value;
        }

        template <typename T>
        bool parse_number(const std::string_view text, T& out)
        {
            if (text.empty())
            {
                return false;
            }

            T value{};
            const auto* first = text.data();
            const auto* last = text.data() + text.size();
            const auto result = std::from_chars(first, last, value);
            if (result.ec != std::errc{} || result.ptr != last)
            {
                return false;
            }

            out = value;
            return true;
        }

        bool is_printable_line(const std::string_view line)
        {
            for (const char ch : line)
            {
                const auto value = static_cast<unsigned char>(ch);
                if (value < 0x20 && ch != '\t')
                {
                    return false;
                }
            }
            return true;
        }
    }

    std::optional<macos_root_manifest> parse_macos_root_manifest(const std::span<const std::byte> data)
    {
        if (data.empty() || data.size() > MAX_MANIFEST_SIZE)
        {
            return std::nullopt;
        }

        const std::string_view text{reinterpret_cast<const char*>(data.data()), data.size()};

        macos_root_manifest manifest{};
        bool has_schema = false;

        size_t offset = 0;
        while (offset <= text.size())
        {
            const auto newline = text.find('\n', offset);
            const auto end = newline == std::string_view::npos ? text.size() : newline;
            const auto line = trim(text.substr(offset, end - offset));
            offset = end + 1;

            if (line.empty() || line.front() == '#')
            {
                if (newline == std::string_view::npos)
                {
                    break;
                }
                continue;
            }

            if (!is_printable_line(line))
            {
                return std::nullopt;
            }

            const auto separator = line.find('=');
            if (separator == std::string_view::npos)
            {
                return std::nullopt;
            }

            const auto key = trim(line.substr(0, separator));
            const auto value = trim(line.substr(separator + 1));

            if (key == "schema")
            {
                if (!parse_number(value, manifest.schema))
                {
                    return std::nullopt;
                }
                has_schema = true;
            }
            else if (key == "cache_file_count")
            {
                if (!parse_number(value, manifest.cache_file_count))
                {
                    return std::nullopt;
                }
            }
            else if (key == "cache_total_bytes")
            {
                if (!parse_number(value, manifest.cache_total_bytes))
                {
                    return std::nullopt;
                }
            }
            else if (key == "optional")
            {
                manifest.optional_items.emplace_back(value);
            }
            else if (key == "tool")
            {
                manifest.tool = value;
            }
            else if (key == "tool_version")
            {
                manifest.tool_version = value;
            }
            else if (key == "created")
            {
                manifest.created = value;
            }
            else if (key == "mode")
            {
                manifest.mode = value;
            }
            else if (key == "arch")
            {
                manifest.arch = value;
            }
            else if (key == "product_name")
            {
                manifest.product_name = value;
            }
            else if (key == "product_version")
            {
                manifest.product_version = value;
            }
            else if (key == "build_version")
            {
                manifest.build_version = value;
            }
            else if (key == "kernel_version")
            {
                manifest.kernel_version = value;
            }
            else if (key == "hardware_model")
            {
                manifest.hardware_model = value;
            }
            else if (key == "cache_guest_dir")
            {
                manifest.cache_guest_dir = value;
            }
            else if (key == "cache_base_name")
            {
                manifest.cache_base_name = value;
            }
            else if (key == "cache_uuid")
            {
                manifest.cache_uuid = value;
            }
            else if (key == "dyld_guest_path")
            {
                manifest.dyld_guest_path = value;
            }
            else if (key == "dyld_sha256")
            {
                manifest.dyld_sha256 = value;
            }

            if (newline == std::string_view::npos)
            {
                break;
            }
        }

        if (!has_schema || manifest.schema != MACOS_ROOT_MANIFEST_SCHEMA)
        {
            return std::nullopt;
        }

        return manifest;
    }

    std::optional<macos_root_manifest> load_macos_root_manifest(const std::filesystem::path& root)
    {
        const auto path = root / MACOS_ROOT_MANIFEST_NAME;

        std::error_code error{};
        if (!std::filesystem::is_regular_file(path, error))
        {
            return std::nullopt;
        }

        const auto size = std::filesystem::file_size(path, error);
        if (error || size > MAX_MANIFEST_SIZE)
        {
            return std::nullopt;
        }

        std::vector<std::byte> data{};
        if (!utils::io::read_file(path, &data))
        {
            return std::nullopt;
        }

        return parse_macos_root_manifest(data);
    }

    std::string format_macos_root_manifest(const macos_root_manifest& manifest)
    {
        std::string text{};

        const auto line = [&](const std::string_view key, const std::string_view value) {
            text += key;
            text.push_back('=');
            text += value;
            text.push_back('\n');
        };

        line("schema", std::to_string(manifest.schema));
        line("tool", manifest.tool);
        line("tool_version", manifest.tool_version);
        line("created", manifest.created);
        line("mode", manifest.mode);
        line("arch", manifest.arch);
        line("product_name", manifest.product_name);
        line("product_version", manifest.product_version);
        line("build_version", manifest.build_version);
        line("kernel_version", manifest.kernel_version);
        line("hardware_model", manifest.hardware_model);
        line("cache_guest_dir", manifest.cache_guest_dir);
        line("cache_base_name", manifest.cache_base_name);
        line("cache_uuid", manifest.cache_uuid);
        line("cache_file_count", std::to_string(manifest.cache_file_count));
        line("cache_total_bytes", std::to_string(manifest.cache_total_bytes));
        line("dyld_guest_path", manifest.dyld_guest_path);
        line("dyld_sha256", manifest.dyld_sha256);

        for (const auto& item : manifest.optional_items)
        {
            line("optional", item);
        }

        return text;
    }

    std::string describe_macos_root_manifest(const macos_root_manifest& manifest)
    {
        return manifest.product_name + " " + manifest.product_version + " build " + manifest.build_version + ", " + manifest.arch +
               " cache " + manifest.cache_uuid + " (" + std::to_string(manifest.cache_file_count) + " files, " +
               std::to_string(manifest.cache_total_bytes) + " bytes, " + manifest.mode + " mode) on " + manifest.hardware_model;
    }
}
