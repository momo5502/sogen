#include "linux_emulation_test_utils.hpp"

#include <array>

namespace linux_test
{
    namespace
    {
        struct fixture_expectation
        {
            const char* relative_path;
            bool should_run;
        };

        std::filesystem::path fixture_path(const std::filesystem::path& root, const char* relative)
        {
            return root / "elf" / relative;
        }
    }

    TEST(LinuxElfLoaderTortureTest, FixtureManifestExpectations)
    {
        const auto fixture_root = get_linux_torture_fixture_root();
        const auto emulation_root = get_linux_test_root();

        const std::array<fixture_expectation, 10> fixtures = {{
            {"valid/base_hello.elf", true},
            {"malformed/truncated_ehdr.elf", false},
            {"malformed/truncated_phdr_table.elf", false},
            {"malformed/malformed_pt_dynamic.elf", false},
            {"malformed/wrong_class.elf", false},
            {"malformed/wrong_data_endianness.elf", false},
            {"malformed/wrong_machine.elf", false},
            {"near-valid/pt_load_overlap_conflict.elf", true},
            {"near-valid/sparse_bss_extreme.elf", true},
            {"near-valid/alignment_edge_mismatch.elf", false},
        }};

        for (const auto& fixture : fixtures)
        {
            const auto path = fixture_path(fixture_root, fixture.relative_path);
            SCOPED_TRACE(path.string());

            ASSERT_TRUE(std::filesystem::exists(path));

            const auto result = run_linux_loader_fixture(path, emulation_root);

            if (fixture.should_run)
            {
                EXPECT_FALSE(result.rejected()) << result.diagnostics;
                EXPECT_TRUE(result.exit_status.has_value()) << "Expected fixture to terminate cleanly";
                if (result.exit_status.has_value())
                {
                    EXPECT_EQ(*result.exit_status, 0) << result.diagnostics;
                }
            }
            else
            {
                EXPECT_TRUE(result.rejected()) << "Fixture unexpectedly accepted";
            }
        }
    }
}
