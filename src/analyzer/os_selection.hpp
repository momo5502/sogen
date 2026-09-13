#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace sogen
{
    enum class analyzer_os : uint8_t
    {
        windows,
        linux,
        macos,
    };

    struct analyzer_invocation
    {
        analyzer_os os{analyzer_os::windows};
        std::vector<char*> arguments{};
    };

    inline std::optional<analyzer_os> parse_analyzer_os(const std::string_view name)
    {
        if (name == "windows")
        {
            return analyzer_os::windows;
        }

        if (name == "linux")
        {
            return analyzer_os::linux;
        }

        if (name == "macos")
        {
            return analyzer_os::macos;
        }

        return std::nullopt;
    }

    // The browser playground has no argv to speak of, so it selects the guest through the environment.
    // Only exactly "1" counts: the playground writes these itself, and a value it did not intend must not
    // be read as consent to run a different emulator.
    constexpr bool is_environment_flag_set(const char* value)
    {
        return value != nullptr && value[0] == '1' && value[1] == '\0';
    }

    constexpr analyzer_os select_analyzer_os(const char* linux_environment, const char* macos_environment)
    {
        if (is_environment_flag_set(macos_environment))
        {
            return analyzer_os::macos;
        }

        if (is_environment_flag_set(linux_environment))
        {
            return analyzer_os::linux;
        }

        return analyzer_os::windows;
    }

    inline std::optional<analyzer_os> detect_analyzer_os_from_magic(const std::span<const uint8_t> header)
    {
        if (header.size() >= 2 && header[0] == 0x4D && header[1] == 0x5A)
        {
            return analyzer_os::windows;
        }

        if (header.size() < 4)
        {
            return std::nullopt;
        }

        if (header[0] == 0x7F && header[1] == 0x45 && header[2] == 0x4C && header[3] == 0x46)
        {
            return analyzer_os::linux;
        }

        const uint32_t magic = static_cast<uint32_t>(header[0]) | (static_cast<uint32_t>(header[1]) << 8) |
                               (static_cast<uint32_t>(header[2]) << 16) | (static_cast<uint32_t>(header[3]) << 24);

        switch (magic)
        {
        case 0xFEEDFACFu:
        case 0xFEEDFACEu:
        case 0xBEBAFECAu:
        case 0xCAFEBABEu:
        case 0xBFBAFECAu:
        case 0xCAFEBABFu:
            return analyzer_os::macos;
        default:
            return std::nullopt;
        }
    }

    inline std::optional<analyzer_os> detect_analyzer_os_from_file(const char* path)
    {
        if (path == nullptr || path[0] == '\0')
        {
            return std::nullopt;
        }

        FILE* file = fopen(path, "rb");
        if (file == nullptr)
        {
            return std::nullopt;
        }

        std::array<uint8_t, 4> header{};
        const auto read = fread(header.data(), 1, header.size(), file);
        (void)fclose(file);

        return detect_analyzer_os_from_magic(std::span<const uint8_t>(header.data(), read));
    }

    // Precedence: an explicit --os, then the first sniffable file on the command line, then the
    // EMULATOR_LINUX / EMULATOR_MACOS variables the browser playground sets. The environment is last
    // because it is the coarsest evidence: the playground sets it once per session, whereas a flag or a
    // Mach-O header describes the binary actually being run. --os is removed from argv because none of
    // the three front-ends declare it, and CLI11 rejects what it does not know.
    inline analyzer_invocation select_analyzer_invocation(const int argc, char** argv, const char* linux_environment,
                                                          const char* macos_environment = nullptr)
    {
        analyzer_invocation invocation{};
        std::optional<analyzer_os> explicit_os{};
        std::optional<analyzer_os> detected_os{};

        for (int i = 0; i < argc; ++i)
        {
            char* argument = argv[i];
            if (argument == nullptr)
            {
                continue;
            }

            const std::string_view text{argument};

            if (text.starts_with("--os="))
            {
                const auto parsed = parse_analyzer_os(text.substr(5));
                if (parsed)
                {
                    explicit_os = parsed;
                    continue;
                }
            }
            else if (text == "--os" && i + 1 < argc && argv[i + 1] != nullptr)
            {
                const auto parsed = parse_analyzer_os(std::string_view{argv[i + 1]});
                if (parsed)
                {
                    explicit_os = parsed;
                    ++i;
                    continue;
                }
            }
            else if (i > 0 && !text.empty() && text.front() != '-' && !detected_os)
            {
                detected_os = detect_analyzer_os_from_file(argument);
            }

            invocation.arguments.push_back(argument);
        }

        if (explicit_os)
        {
            invocation.os = *explicit_os;
        }
        else if (detected_os)
        {
            invocation.os = *detected_os;
        }
        else
        {
            invocation.os = select_analyzer_os(linux_environment, macos_environment);
        }

        return invocation;
    }
}
