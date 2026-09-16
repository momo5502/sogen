#include "macos_launch_target.hpp"

#include "macos_bundle.hpp"

namespace sogen
{
    namespace
    {
        macos_launch_target from_bundle(const std::filesystem::path& bundle_path)
        {
            macos_launch_target target{};
            target.kind = macos_input_kind::app_bundle;

            std::string error{};
            const auto bundle = resolve_app_bundle(bundle_path, error);
            if (!bundle)
            {
                target.diagnostic = error;
                return target;
            }

            const auto guest_root = std::string{MACOS_BUNDLE_GUEST_PREFIX} + "/" + bundle->bundle_name;

            target.host_executable = bundle->executable;
            target.guest_executable = guest_root + "/Contents/MacOS/" + bundle->executable_name;
            // A LaunchServices-started bundle runs with cwd "/" - measured on macOS 26.6.1 by launching a
            // probe .app with `open` and reading getcwd().
            target.working_directory = "/";
            target.bundle_identifier = bundle->identifier;
            target.bundle = macos_bundle_mapping{.guest_root = guest_root, .host_root = bundle->bundle_root};
            return target;
        }
    }

    macos_launch_target resolve_macos_launch_target(const std::filesystem::path& input)
    {
        const auto kind = classify_macos_input(input);

        if (kind == macos_input_kind::app_bundle)
        {
            return from_bundle(input);
        }

        if (kind == macos_input_kind::mach_o || kind == macos_input_kind::fat_mach_o)
        {
            std::error_code error{};
            const auto absolute = std::filesystem::absolute(input, error).lexically_normal();
            if (error)
            {
                macos_launch_target target{};
                target.kind = kind;
                target.diagnostic = "cannot resolve '" + input.string() + "'";
                return target;
            }

            if (const auto enclosing = enclosing_app_bundle(absolute))
            {
                auto target = from_bundle(*enclosing);
                if (target.runnable() || !target.diagnostic.empty())
                {
                    return target;
                }
            }

            macos_launch_target target{};
            target.kind = kind;
            target.host_executable = absolute;
            target.guest_executable = absolute.generic_string();
            target.working_directory = absolute.parent_path().generic_string();
            return target;
        }

        macos_launch_target target{};
        target.kind = kind;
        target.diagnostic = describe_unsupported_input(input, kind);
        return target;
    }

    void apply_macos_launch_target(const macos_launch_target& target, guest_file_system& fs)
    {
        if (!target.bundle)
        {
            return;
        }

        fs.add_path_mapping(target.bundle->guest_root, target.bundle->host_root, true);
    }
}
