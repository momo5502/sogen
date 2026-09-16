#include "macos_bundle.hpp"

#include "plist.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <vector>

#include <utils/io.hpp>

namespace sogen
{
    namespace
    {
        std::string lowercase(std::string value)
        {
            std::ranges::transform(value, value.begin(), [](const unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            return value;
        }

        // A path typed with a trailing separator - which is what shell tab-completion produces for a
        // directory - has an empty filename component, so extension() and filename() both come back
        // empty and the bundle would be unrecognisable by name.
        std::filesystem::path without_trailing_separator(const std::filesystem::path& path)
        {
            if (path.has_filename() || !path.has_relative_path())
            {
                return path;
            }
            return path.parent_path();
        }

        bool is_single_component(const std::string_view name)
        {
            return !name.empty() && name != "." && name != ".." && name.find('/') == std::string_view::npos &&
                   name.find('\\') == std::string_view::npos;
        }

        bool is_within(const std::filesystem::path& path, const std::filesystem::path& prefix)
        {
            auto path_it = path.begin();
            for (auto prefix_it = prefix.begin(); prefix_it != prefix.end(); ++prefix_it, ++path_it)
            {
                if (path_it == path.end() || *path_it != *prefix_it)
                {
                    return false;
                }
            }
            return true;
        }

        std::optional<std::string> read_bundle_string(const std::filesystem::path& info_plist, const std::string_view key)
        {
            std::error_code error{};
            if (!std::filesystem::is_regular_file(info_plist, error))
            {
                return std::nullopt;
            }

            const auto size = std::filesystem::file_size(info_plist, error);
            if (error || size > MAX_PLIST_SIZE)
            {
                return std::nullopt;
            }

            std::vector<std::byte> data{};
            if (!utils::io::read_file(info_plist, &data))
            {
                return std::nullopt;
            }

            return plist_top_level_string(data, key);
        }
    }

    bool is_app_bundle_path(const std::filesystem::path& path)
    {
        return lowercase(without_trailing_separator(path).extension().string()) == ".app";
    }

    std::optional<std::filesystem::path> enclosing_app_bundle(const std::filesystem::path& executable)
    {
        const auto macos_dir = executable.parent_path();
        if (macos_dir.filename() != "MacOS")
        {
            return std::nullopt;
        }

        const auto contents = macos_dir.parent_path();
        if (contents.filename() != "Contents")
        {
            return std::nullopt;
        }

        auto bundle = contents.parent_path();
        if (!is_app_bundle_path(bundle))
        {
            return std::nullopt;
        }

        return bundle;
    }

    std::optional<macos_bundle> resolve_app_bundle(const std::filesystem::path& path, std::string& error)
    {
        error.clear();

        const auto input = without_trailing_separator(path);

        std::error_code code{};
        if (!std::filesystem::is_directory(input, code))
        {
            error = "not a directory: " + input.string();
            return std::nullopt;
        }

        if (!is_app_bundle_path(input))
        {
            error = "not an .app bundle: " + input.string();
            return std::nullopt;
        }

        auto bundle_root = std::filesystem::weakly_canonical(input, code);
        if (code || bundle_root.empty())
        {
            error = "cannot resolve bundle path: " + input.string();
            return std::nullopt;
        }

        macos_bundle bundle{};
        bundle.bundle_root = bundle_root;
        bundle.bundle_name = bundle_root.filename().string();

        const auto info_plist = bundle_root / "Contents" / "Info.plist";
        const auto declared = read_bundle_string(info_plist, "CFBundleExecutable");
        const auto identifier = read_bundle_string(info_plist, "CFBundleIdentifier");

        if (declared && !is_single_component(*declared))
        {
            error = "CFBundleExecutable is not a single path component: '" + *declared + "'";
            return std::nullopt;
        }

        bundle.executable_name = declared ? *declared : bundle_root.stem().string();
        bundle.identifier = identifier.value_or(std::string{});

        if (!is_single_component(bundle.executable_name))
        {
            error = "cannot determine an executable name for " + bundle.bundle_name;
            return std::nullopt;
        }

        bundle.executable = bundle_root / "Contents" / "MacOS" / bundle.executable_name;

        if (!std::filesystem::is_regular_file(bundle.executable, code))
        {
            error = "no regular file at Contents/MacOS/" + bundle.executable_name;
            return std::nullopt;
        }

        const auto canonical_executable = std::filesystem::weakly_canonical(bundle.executable, code);
        if (code || !is_within(canonical_executable, bundle_root))
        {
            error = "Contents/MacOS/" + bundle.executable_name + " resolves outside the bundle";
            return std::nullopt;
        }

        return bundle;
    }
}
