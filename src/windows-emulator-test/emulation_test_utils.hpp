#pragma once

#ifdef __MINGW64__
#include <unistd.h>
#endif

#include <cstdlib>
#include <gtest/gtest.h>
#include <windows_emulator.hpp>
#include <backend_selection.hpp>

#include <network/static_socket_factory.hpp>

#define ASSERT_NOT_TERMINATED(win_emu)                           \
    do                                                           \
    {                                                            \
        ASSERT_FALSE((win_emu).process.exit_status.has_value()); \
    } while (false)

#define ASSERT_TERMINATED_WITH_STATUS(win_emu, status)          \
    do                                                          \
    {                                                           \
        ASSERT_TRUE((win_emu).process.exit_status.has_value()); \
        ASSERT_EQ(*(win_emu).process.exit_status, status);      \
    } while (false)

#define ASSERT_TERMINATED_SUCCESSFULLY(win_emu) ASSERT_TERMINATED_WITH_STATUS(win_emu, STATUS_SUCCESS)

namespace test
{
    inline bool enable_verbose_logging()
    {
        const auto* env = getenv("EMULATOR_VERBOSE");
        return env && (env == "1"sv || env == "true"sv);
    }

    inline std::filesystem::path get_emulator_root()
    {
        const auto* env = getenv("EMULATOR_ROOT");
        if (!env)
        {
            throw std::runtime_error("No EMULATOR_ROOT set!");
        }

        return env;
    }

    struct sample_configuration
    {
        bool print_time{false};
    };

    namespace
    {
        network::address make_ipv6_address(const char* value)
        {
            in6_addr addr{};
            if (inet_pton(AF_INET6, value, &addr) != 1)
            {
                throw std::runtime_error("Invalid IPv6 address");
            }

            network::address result{};
            result.set_ipv6(addr);
            return result;
        }

        struct sample_dns_lookup final : network::dns_lookup
        {
            std::vector<network::address> resolve_host(const std::string_view hostname, const std::optional<int> family) override
            {
                if (hostname != "google.com")
                {
                    return {};
                }

                std::vector<network::address> results{};
                if (!family || *family == AF_INET)
                {
                    results.emplace_back("203.0.113.10", AF_INET);
                    results.emplace_back("203.0.113.20", AF_INET);
                }

                if (!family || *family == AF_INET6)
                {
                    results.push_back(make_ipv6_address("2001:db8::10"));
                    results.push_back(make_ipv6_address("2001:db8::20"));
                }

                return results;
            }
        };

        std::unique_ptr<network::dns_lookup> create_sample_dns_lookup()
        {
            return std::make_unique<sample_dns_lookup>();
        }
    }

    inline application_settings get_sample_app_settings(const sample_configuration& config)
    {
        application_settings settings{.application = "C:\\test-sample.exe"};

        if (config.print_time)
        {
            settings.arguments.emplace_back(u"-time");
        }

        return settings;
    }

    inline windows_emulator create_emulator(emulator_settings settings, emulator_callbacks callbacks = {},
                                            emulator_interfaces interfaces = {})
    {
        const auto is_verbose = enable_verbose_logging();

        if (is_verbose)
        {
            callbacks.on_stdout = [](const std::string_view data) {
                std::cout << data; //
            };
        }

        settings.emulation_root = get_emulator_root();

        settings.path_mappings["C:\\a.txt"] =
            std::filesystem::temp_directory_path() / ("emulator-test-file-" + std::to_string(getpid()) + ".txt");

        if (!interfaces.socket_factory)
        {
            interfaces.socket_factory = network::create_static_socket_factory();
        }

        return windows_emulator{
            create_x86_64_emulator(),
            settings,
            std::move(callbacks),
            std::move(interfaces),
        };
    }

    inline windows_emulator create_sample_emulator(emulator_settings settings, const sample_configuration& config = {},
                                                   emulator_callbacks callbacks = {}, emulator_interfaces interfaces = {})
    {
        const auto is_verbose = enable_verbose_logging();

        if (is_verbose)
        {
            callbacks.on_stdout = [](const std::string_view data) {
                std::cout << data; //
            };
        }

        settings.emulation_root = get_emulator_root();

        settings.path_mappings["C:\\a.txt"] =
            std::filesystem::temp_directory_path() / ("emulator-test-file-" + std::to_string(getpid()) + ".txt");

        if (!interfaces.socket_factory)
        {
            interfaces.socket_factory = network::create_static_socket_factory();
        }

        if (!interfaces.dns_lookup)
        {
            interfaces.dns_lookup = create_sample_dns_lookup();
        }

        return windows_emulator{
            create_x86_64_emulator(), get_sample_app_settings(config), settings, std::move(callbacks), std::move(interfaces),
        };
    }

    inline windows_emulator create_sample_emulator(const sample_configuration& config = {})
    {
        emulator_settings settings{
            .use_relative_time = true,
        };

        return create_sample_emulator(std::move(settings), config);
    }

    inline windows_emulator create_empty_emulator(emulator_interfaces interfaces = {})
    {
        emulator_settings settings{
            .use_relative_time = true,
        };

        return create_emulator(std::move(settings), {}, std::move(interfaces));
    }

    inline void bisect_emulation(windows_emulator& emu)
    {
        utils::buffer_serializer start_state{};
        emu.serialize(start_state);

        emu.start();
        const auto limit = emu.get_executed_instructions();

        const auto reset_emulator = [&] {
            utils::buffer_deserializer deserializer{start_state};
            emu.deserialize(deserializer);
        };

        const auto get_state_for_count = [&](const size_t count) {
            reset_emulator();
            emu.start(count);

            utils::buffer_serializer state{};
            emu.serialize(state);
            return state;
        };

        const auto has_diff_after_count = [&](const size_t count) {
            const auto s1 = get_state_for_count(count);
            const auto s2 = get_state_for_count(count);

            return s1.get_diff(s2).has_value();
        };

        if (!has_diff_after_count(static_cast<size_t>(limit)))
        {
            puts("Emulation has no diff");
        }

        auto upper_bound = limit;
        decltype(upper_bound) lower_bound = 0;

        printf("Bounds: %" PRIx64 " - %" PRIx64 "\n", lower_bound, upper_bound);

        while (lower_bound + 1 < upper_bound)
        {
            const auto diff = (upper_bound - lower_bound);
            const auto pivot = lower_bound + (diff / 2);

            const auto has_diff = has_diff_after_count(static_cast<size_t>(pivot));

            auto* bound = has_diff ? &upper_bound : &lower_bound;
            *bound = pivot;

            printf("Bounds: %" PRIx64 " - %" PRIx64 "\n", lower_bound, upper_bound);
        }

        (void)get_state_for_count(static_cast<size_t>(lower_bound));

        const auto rip = emu.emu().read_instruction_pointer();

        printf("Diff detected after 0x%" PRIx64 " instructions at 0x%" PRIx64 " (%s)\n", lower_bound, rip, emu.mod_manager.find_name(rip));
    }
}
