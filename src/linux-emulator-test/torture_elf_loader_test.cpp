#include "linux_emulation_test_utils.hpp"
#include "module/elf_mapping.hpp"

#include <array>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <vector>

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

        elf::Elf64_Phdr base_load_segment()
        {
            elf::Elf64_Phdr phdr{};
            phdr.p_type = elf::PT_LOAD;
            phdr.p_flags = elf::PF_R | elf::PF_X;
            phdr.p_vaddr = 0x400000;
            phdr.p_memsz = 0x1000;
            phdr.p_align = 0x1000;
            return phdr;
        }

        std::vector<std::byte> make_minimal_elf(const std::initializer_list<elf::Elf64_Phdr> phdrs)
        {
            std::vector<std::byte> data(sizeof(elf::Elf64_Ehdr) + (sizeof(elf::Elf64_Phdr) * phdrs.size()));

            elf::Elf64_Ehdr ehdr{};
            ehdr.e_ident[elf::EI_MAG0] = static_cast<uint8_t>(elf::ELFMAG0);
            ehdr.e_ident[elf::EI_MAG1] = static_cast<uint8_t>(elf::ELFMAG1);
            ehdr.e_ident[elf::EI_MAG2] = static_cast<uint8_t>(elf::ELFMAG2);
            ehdr.e_ident[elf::EI_MAG3] = static_cast<uint8_t>(elf::ELFMAG3);
            ehdr.e_ident[elf::EI_CLASS] = elf::ELFCLASS64;
            ehdr.e_ident[elf::EI_DATA] = elf::ELFDATA2LSB;
            ehdr.e_ident[elf::EI_VERSION] = elf::EV_CURRENT;
            ehdr.e_type = elf::ET_EXEC;
            ehdr.e_machine = elf::EM_X86_64;
            ehdr.e_version = elf::EV_CURRENT;
            ehdr.e_entry = phdrs.begin()->p_vaddr;
            ehdr.e_phoff = sizeof(elf::Elf64_Ehdr);
            ehdr.e_ehsize = sizeof(elf::Elf64_Ehdr);
            ehdr.e_phentsize = sizeof(elf::Elf64_Phdr);
            ehdr.e_phnum = static_cast<uint16_t>(phdrs.size());

            std::memcpy(data.data(), &ehdr, sizeof(ehdr));
            std::memcpy(data.data() + static_cast<size_t>(ehdr.e_phoff), phdrs.begin(), sizeof(elf::Elf64_Phdr) * phdrs.size());
            return data;
        }
    }

    TEST(LinuxElfLoaderTortureTest, RejectsLoadSegmentFileSizeLargerThanMemorySize)
    {
        auto phdr = base_load_segment();
        phdr.p_filesz = phdr.p_memsz + 1;

        const auto elf_data = make_minimal_elf({phdr});

        EXPECT_THROW(read_elf_module_metadata(elf_data, "p_filesz_gt_p_memsz.elf", 0), std::runtime_error);
    }

    TEST(LinuxElfLoaderTortureTest, RejectsWrappingLoadSegmentFileRange)
    {
        auto phdr = base_load_segment();
        phdr.p_offset = std::numeric_limits<uint64_t>::max();
        phdr.p_filesz = 1;
        phdr.p_memsz = 1;
        phdr.p_align = 1;

        const auto elf_data = make_minimal_elf({phdr});

        EXPECT_THROW(read_elf_module_metadata(elf_data, "wrapping_p_offset.elf", 0), std::runtime_error);
    }

    TEST(LinuxElfLoaderTortureTest, RejectsWrappingDynamicSegmentFileRange)
    {
        const auto load_phdr = base_load_segment();
        elf::Elf64_Phdr dynamic_phdr{};
        dynamic_phdr.p_type = elf::PT_DYNAMIC;
        dynamic_phdr.p_offset = std::numeric_limits<uint64_t>::max();
        dynamic_phdr.p_filesz = 1;

        const auto elf_data = make_minimal_elf({load_phdr, dynamic_phdr});

        EXPECT_THROW(read_elf_module_metadata(elf_data, "wrapping_pt_dynamic.elf", 0), std::runtime_error);
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
