#include "emulation_test_utils.hpp"

#include <snapshot.hpp>

#include <filesystem>
#include <vector>

namespace sogen::test
{
    // A controller made to keep the process running saves it when asked and lets it run to its end; one that is not
    // returns once the process is saved.
    TEST(SnapshotControllerTest, KeepsTheProcessRunningAfterASaveWhenMadeTo)
    {
#if defined(OS_EMSCRIPTEN) && !defined(__EMSCRIPTEN_PTHREADS__)
        // The controller starts its watcher thread on construction.
        GTEST_SKIP() << "Requires host threads";
#endif
        const auto directory = std::filesystem::temp_directory_path() / "sogen-snapshot-controller-test";
        std::filesystem::remove_all(directory);

        for (const bool keep_running : {true, false})
        {
            auto emu = create_sample_emulator();
            std::vector<snapshot::snapshot_outcome> outcomes{};
            snapshot::controller snapshots{
                emu, {}, directory, [&](const snapshot::snapshot_outcome& outcome) { outcomes.push_back(outcome); }, keep_running};

            ASSERT_TRUE(snapshots.request(directory / (keep_running ? "kept.snap" : "returned.snap")));
            EXPECT_EQ(!keep_running, snapshots.run());

            ASSERT_EQ(1u, outcomes.size());
            EXPECT_EQ("", outcomes[0].error);
            EXPECT_TRUE(std::filesystem::is_regular_file(outcomes[0].path));
            EXPECT_EQ(keep_running, emu.process.exit_status.has_value());
        }

        std::filesystem::remove_all(directory);
    }
}
