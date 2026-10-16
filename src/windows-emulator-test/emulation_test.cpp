#include "emulation_test_utils.hpp"

namespace sogen::test
{
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

    TEST(EmulationTest, SocketActivityReportsGuestAddresses)
    {
        std::vector<std::pair<network::socket_activity, std::string>> activities{};

        emulator_callbacks callbacks{};
        callbacks.on_socket_activity = [&](const network::socket_activity activity, const network::address& address) {
            activities.emplace_back(activity, address.to_string());
        };

        auto emu = create_sample_emulator({.use_relative_time = true}, {}, std::move(callbacks));
        emu.start();

        ASSERT_TERMINATED_SUCCESSFULLY(emu);

        const auto contains = [&](const network::socket_activity activity, const std::string_view address) {
            return std::ranges::find(activities, std::pair{activity, std::string(address)}) != activities.end();
        };

        EXPECT_TRUE(contains(network::socket_activity::bind, "127.0.0.1:28970"));
        EXPECT_TRUE(contains(network::socket_activity::sendto, "127.0.0.1:28970"));
    }
} // namespace sogen::test
