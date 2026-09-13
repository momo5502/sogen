#include <gtest/gtest.h>

#include "macos_test_utils.hpp"

#include <serialization.hpp>

#include <filesystem>
#include <fstream>

namespace
{
    constexpr uint64_t guest_base = 0x300000000ULL;

    TEST(MacosEmulator, MapsTheCommpageOnConstruction)
    {
        const auto emu = macos_test::make_emulator();

        EXPECT_TRUE(emu->commpage.is_mapped());
        EXPECT_EQ(emu->commpage.get_base(), sogen::MACOS_COMMPAGE_BASE);
        EXPECT_EQ(emu->commpage.get_readonly_base(), sogen::MACOS_COMMPAGE_RO_BASE);
        EXPECT_EQ(emu->last_stop_reason(), sogen::stop_reason::none);
        EXPECT_EQ(emu->get_executed_instructions(), 0u);
    }

    TEST(MacosEmulator, CountsRetiredInstructionsAndReportsTheInstructionLimit)
    {
        const auto emu = macos_test::make_emulator();

        macos_test::write_guest_code(*emu, guest_base,
                                     {
                                         0xD503201F, // nop
                                         0xD503201F, // nop
                                         0xD503201F, // nop
                                         0xD503201F, // nop
                                     });

        emu->start(2);

        EXPECT_EQ(emu->last_stop_reason(), sogen::stop_reason::instruction_limit);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::pc), guest_base + 8);
    }

    TEST(MacosEmulator, AnUnmappedWriteIsClassifiedAsAMemoryViolation)
    {
        const auto emu = macos_test::make_emulator();

        macos_test::write_guest_code(*emu, guest_base,
                                     {
                                         0xD2C00023, // mov x3, #0x100000000
                                         0xF9000064, // str x4, [x3]
                                     });

        emu->start(8);

        EXPECT_EQ(emu->last_stop_reason(), sogen::stop_reason::unhandled_memory_violation) << "detail: " << emu->last_stop_detail();
        EXPECT_NE(emu->last_stop_detail().find("unmapped write"), std::string::npos) << "detail: " << emu->last_stop_detail();
        EXPECT_NE(emu->last_stop_detail().find("address=0x100000000"), std::string::npos) << "detail: " << emu->last_stop_detail();
    }

    TEST(MacosEmulator, AWriteToAReadOnlyPageIsAProtectionViolationNotAnUnmappedOne)
    {
        const auto emu = macos_test::make_emulator();
        constexpr uint64_t readonly_base = 0x400000000ULL;

        ASSERT_TRUE(emu->memory.allocate_memory(readonly_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read));

        macos_test::write_guest_code(*emu, guest_base,
                                     {
                                         0xD2C00083, // mov x3, #0x400000000
                                         0xF9000064, // str x4, [x3]
                                     });

        emu->start(8);

        EXPECT_EQ(emu->last_stop_reason(), sogen::stop_reason::unhandled_memory_violation) << "detail: " << emu->last_stop_detail();
        EXPECT_NE(emu->last_stop_detail().find("protection write"), std::string::npos) << "detail: " << emu->last_stop_detail();
    }

    TEST(MacosEmulator, StateSurvivesASerializationRoundTrip)
    {
        const auto emu = macos_test::make_emulator();

        macos_test::write_guest_code(*emu, guest_base,
                                     {
                                         0xD503201F, // nop
                                         0xD503201F, // nop
                                         0xD503201F, // nop
                                     });

        emu->start(2);
        ASSERT_EQ(emu->last_stop_reason(), sogen::stop_reason::instruction_limit);

        sogen::utils::buffer_serializer serializer{};
        emu->serialize(serializer, false);

        const auto restored = macos_test::make_emulator();
        sogen::utils::buffer_deserializer deserializer{serializer.get_buffer()};
        restored->deserialize(deserializer, false);

        EXPECT_EQ(restored->get_executed_instructions(), emu->get_executed_instructions());
        EXPECT_EQ(restored->last_stop_reason(), sogen::stop_reason::instruction_limit);
        EXPECT_EQ(restored->last_stop_detail(), emu->last_stop_detail());
        EXPECT_EQ(restored->emu().reg(sogen::arm64_register::pc), guest_base + 8);

        uint32_t instruction{};
        restored->memory.read_memory(guest_base, &instruction, sizeof(instruction));
        EXPECT_EQ(instruction, 0xD503201Fu);
    }

    TEST(MacosEmulator, TheCommpageIsRebuiltRatherThanRestored)
    {
        const auto emu = macos_test::make_emulator();

        sogen::utils::buffer_serializer serializer{};
        emu->serialize(serializer, false);

        const auto restored = macos_test::make_emulator();
        sogen::utils::buffer_deserializer deserializer{serializer.get_buffer()};
        restored->deserialize(deserializer, false);

        ASSERT_TRUE(restored->commpage.is_mapped());
        ASSERT_EQ(restored->commpage.get_base(), sogen::MACOS_COMMPAGE_BASE);

        std::array<char, 16> signature{};
        restored->memory.read_memory(sogen::MACOS_COMMPAGE_BASE + sogen::commpage_offset::SIGNATURE, signature.data(), signature.size());
        EXPECT_STREQ(signature.data(), restored->system_info.signature.c_str());

        uint64_t memory_size{};
        restored->memory.read_memory(sogen::MACOS_COMMPAGE_BASE + sogen::commpage_offset::MEMORY_SIZE, &memory_size, sizeof(memory_size));
        EXPECT_EQ(memory_size, restored->system_info.memory_size);
    }

    TEST(MacosEmulator, RejectsAnUnknownStateVersion)
    {
        sogen::utils::buffer_serializer serializer{};
        serializer.write(std::string{"macos-emulator-state-v0"});

        const auto emu = macos_test::make_emulator();
        sogen::utils::buffer_deserializer deserializer{serializer.get_buffer()};

        EXPECT_THROW(emu->deserialize(deserializer, false), std::runtime_error);
    }

    // The append bit is decoded inside guest_fd_table::deserialize, which macos_process_context runs on
    // a temporary. A test that builds its own table with the Darwin flag never exercises that path.
    TEST(MacosEmulator, ARestoredAppendingDescriptorStillAppends)
    {
        const auto path = std::filesystem::temp_directory_path() / "sogen-emulator-append.txt";
        {
            std::ofstream seed{path, std::ios::binary | std::ios::trunc};
            seed << "seed";
        }

        const auto emu = macos_test::make_emulator();

        sogen::guest_fd entry{};
        entry.type = sogen::fd_type::file;
        entry.host_path = path.string();
        entry.flags = sogen::macos_open::MACOS_O_RDWR | sogen::macos_open::MACOS_O_APPEND;
        entry.handle = std::fopen(path.string().c_str(), "a+b");
        ASSERT_NE(entry.handle, nullptr);
        const auto fd = emu->process.fds.allocate(std::move(entry));

        // Leaving the offset at end-of-file would let a truncating reopen write the same bytes.
        std::fseek(emu->process.fds.get(fd)->handle, 0, SEEK_SET);

        sogen::utils::buffer_serializer serializer{};
        emu->serialize(serializer, false);

        const auto restored = macos_test::make_emulator();
        sogen::utils::buffer_deserializer deserializer{serializer.get_buffer()};
        restored->deserialize(deserializer, false);

        auto* reopened = restored->process.fds.get(fd);
        ASSERT_NE(reopened, nullptr);
        ASSERT_NE(reopened->handle, nullptr);

        std::fputs("-tail", reopened->handle);
        std::fflush(reopened->handle);

        std::ifstream check{path, std::ios::binary};
        std::string contents{std::istreambuf_iterator<char>{check}, std::istreambuf_iterator<char>{}};
        std::filesystem::remove(path);

        EXPECT_EQ(contents, "seed-tail") << "the production restore path decoded the append bit with Linux's mask";
    }

    // A per-instruction UC_HOOK_CODE callback is a std::function call per guest instruction; reaching
    // main() under dyld is 10^7 to 10^8 of them. The block hook carries an exact count on A64, so the
    // instruction total is unchanged while the callback fires once per block instead.
    TEST(MacosEmulator, InstructionCountingDoesNotInstallAPerInstructionHook)
    {
        const auto emu = macos_test::make_emulator();

        macos_test::write_guest_code(*emu, guest_base,
                                     {
                                         0xD503201F, // nop
                                         0xD503201F, // nop
                                         0xD503201F, // nop
                                         0xD503201F, // nop
                                         0xD4200000, // brk #0
                                     });

        emu->start(0);

        EXPECT_EQ(emu->last_stop_reason(), sogen::stop_reason::unhandled_cpu_exception);
        EXPECT_EQ(emu->get_executed_instructions(), 5u);
        EXPECT_EQ(emu->get_executed_basic_blocks(), 1u);
    }

    TEST(MacosEmulator, TheBlockCounterTravelsInASnapshot)
    {
        const auto emu = macos_test::make_emulator();

        macos_test::write_guest_code(*emu, guest_base,
                                     {
                                         0xD503201F, // nop
                                         0xD503201F, // nop
                                         0xD4200000, // brk #0
                                     });
        emu->start(0);
        ASSERT_NE(emu->get_executed_basic_blocks(), 0u);

        sogen::utils::buffer_serializer serializer{};
        emu->serialize(serializer, false);

        const auto restored = macos_test::make_emulator();
        sogen::utils::buffer_deserializer deserializer{serializer.get_buffer()};
        restored->deserialize(deserializer, false);

        EXPECT_EQ(restored->get_executed_basic_blocks(), emu->get_executed_basic_blocks());
        EXPECT_EQ(restored->get_executed_instructions(), emu->get_executed_instructions());
    }
}
