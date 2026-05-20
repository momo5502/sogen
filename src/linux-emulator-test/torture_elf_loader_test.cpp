#include "linux_emulation_test_utils.hpp"

#include <array>

namespace sogen::linux_test
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
            {.relative_path = "valid/base_hello.elf", .should_run = true},
            {.relative_path = "malformed/truncated_ehdr.elf", .should_run = false},
            {.relative_path = "malformed/truncated_phdr_table.elf", .should_run = false},
            {.relative_path = "malformed/malformed_pt_dynamic.elf", .should_run = false},
            {.relative_path = "malformed/wrong_class.elf", .should_run = false},
            {.relative_path = "malformed/wrong_data_endianness.elf", .should_run = false},
            {.relative_path = "malformed/wrong_machine.elf", .should_run = false},
            {.relative_path = "near-valid/pt_load_overlap_conflict.elf", .should_run = true},
            {.relative_path = "near-valid/sparse_bss_extreme.elf", .should_run = true},
            {.relative_path = "near-valid/alignment_edge_mismatch.elf", .should_run = false},
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
