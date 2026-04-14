#include "emulation_test_utils.hpp"

namespace test
{
    namespace
    {
        network::address make_recording_ipv6_address(const char* value)
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

        struct recording_dns_lookup final : network::dns_lookup
        {
            std::string last_hostname{};
            std::vector<int> families{};

            std::vector<network::address> resolve_host(const std::string_view hostname, const std::optional<int> family) override
            {
                last_hostname.assign(hostname);
                if (family)
                {
                    families.push_back(*family);
                }

                if (hostname != "example.com")
                {
                    return {};
                }

                if (family && *family == AF_INET)
                {
                    return {network::address{"198.51.100.10", AF_INET}};
                }

                if (family && *family == AF_INET6)
                {
                    return {make_recording_ipv6_address("2001:db8::1234")};
                }

                return {};
            }
        };
    }

    TEST(EmulationTest, BasicEmulationWorks)
    {
        auto emu = create_sample_emulator();
        emu.start();

        ASSERT_TERMINATED_SUCCESSFULLY(emu);
    }

    TEST(EmulationTest, CountedEmulationWorks)
    {
        constexpr auto count = 200000;

        auto emu = create_sample_emulator();
        emu.start(count);

        ASSERT_EQ(emu.get_executed_instructions(), count);
    }

    TEST(EmulationTest, CountedEmulationIsAccurate)
    {
        auto emu = create_sample_emulator();
        emu.start();

        ASSERT_TERMINATED_SUCCESSFULLY(emu);

        const auto executedInstructions = emu.get_executed_instructions();

        auto new_emu = create_sample_emulator();

        constexpr auto offset = 1;
        const auto instructionsToExecute = executedInstructions - offset;

        new_emu.start(static_cast<size_t>(instructionsToExecute));

        ASSERT_EQ(new_emu.get_executed_instructions(), instructionsToExecute);
        ASSERT_NOT_TERMINATED(new_emu);

        new_emu.start(offset);

        ASSERT_TERMINATED_SUCCESSFULLY(new_emu);
        ASSERT_EQ(new_emu.get_executed_instructions(), executedInstructions);
    }

    TEST(EmulationTest, SampleDnsUsesInjectedLookup)
    {
        std::string output_buffer{};
        emulator_callbacks callbacks{};
        callbacks.on_stdout = [&output_buffer](const std::string_view data) { output_buffer.append(data); };

        auto fake = std::make_unique<recording_dns_lookup>();
        auto* fake_ptr = fake.get();

        emulator_settings settings{
            .use_relative_time = true,
        };

        auto emu = create_sample_emulator(std::move(settings), {}, std::move(callbacks),
                                          emulator_interfaces{
                                              .dns_lookup = std::move(fake),
                                          });
        emu.start();

        ASSERT_TERMINATED_SUCCESSFULLY(emu);
        ASSERT_NE(output_buffer.find("Running test 'DNS': Success"), std::string::npos);
        ASSERT_EQ(fake_ptr->last_hostname, "example.com");
        ASSERT_TRUE(std::ranges::find(fake_ptr->families, AF_INET) != fake_ptr->families.end());
        ASSERT_TRUE(std::ranges::find(fake_ptr->families, AF_INET6) != fake_ptr->families.end());
    }
}
