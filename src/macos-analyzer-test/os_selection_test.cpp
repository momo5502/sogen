#include <gtest/gtest.h>

#include <os_selection.hpp>

#include <array>
#include <string>
#include <vector>

namespace
{
    std::vector<char*> to_argv(std::vector<std::string>& storage)
    {
        std::vector<char*> argv{};
        argv.reserve(storage.size());

        for (auto& entry : storage)
        {
            argv.push_back(entry.data());
        }

        return argv;
    }

    TEST(AnalyzerOsSelection, ParsesTheThreeNames)
    {
        EXPECT_EQ(sogen::parse_analyzer_os("windows"), sogen::analyzer_os::windows);
        EXPECT_EQ(sogen::parse_analyzer_os("linux"), sogen::analyzer_os::linux);
        EXPECT_EQ(sogen::parse_analyzer_os("macos"), sogen::analyzer_os::macos);
        EXPECT_FALSE(sogen::parse_analyzer_os("plan9").has_value());
    }

    TEST(AnalyzerOsSelection, ClassifiesMagicBytes)
    {
        const std::array<uint8_t, 4> mz{'M', 'Z', 0x90, 0x00};
        const std::array<uint8_t, 4> elf{0x7F, 'E', 'L', 'F'};
        const std::array<uint8_t, 4> macho64{0xCF, 0xFA, 0xED, 0xFE};
        const std::array<uint8_t, 4> macho32{0xCE, 0xFA, 0xED, 0xFE};
        const std::array<uint8_t, 4> fat{0xCA, 0xFE, 0xBA, 0xBE};
        const std::array<uint8_t, 4> fat_swapped{0xBE, 0xBA, 0xFE, 0xCA};
        const std::array<uint8_t, 4> nonsense{0x11, 0x22, 0x33, 0x44};

        EXPECT_EQ(sogen::detect_analyzer_os_from_magic(mz), sogen::analyzer_os::windows);
        EXPECT_EQ(sogen::detect_analyzer_os_from_magic(elf), sogen::analyzer_os::linux);
        EXPECT_EQ(sogen::detect_analyzer_os_from_magic(macho64), sogen::analyzer_os::macos);
        EXPECT_EQ(sogen::detect_analyzer_os_from_magic(macho32), sogen::analyzer_os::macos);
        EXPECT_EQ(sogen::detect_analyzer_os_from_magic(fat), sogen::analyzer_os::macos);
        EXPECT_EQ(sogen::detect_analyzer_os_from_magic(fat_swapped), sogen::analyzer_os::macos);
        EXPECT_FALSE(sogen::detect_analyzer_os_from_magic(nonsense).has_value());
    }

    TEST(AnalyzerOsSelection, ShortHeaderIsNotClassified)
    {
        const std::array<uint8_t, 1> one{'M'};
        EXPECT_FALSE(sogen::detect_analyzer_os_from_magic(one).has_value());
        EXPECT_FALSE(sogen::detect_analyzer_os_from_magic({}).has_value());
    }

    TEST(AnalyzerOsSelection, DetectsACommittedMachOFixture)
    {
        const std::string path = std::string(MACOS_FIXTURE_ROOT) + "/macho_static_arm64";
        EXPECT_EQ(sogen::detect_analyzer_os_from_file(path.c_str()), sogen::analyzer_os::macos);

        const std::string fat_path = std::string(MACOS_FIXTURE_ROOT) + "/macho_fat_arm64_arm64e";
        EXPECT_EQ(sogen::detect_analyzer_os_from_file(fat_path.c_str()), sogen::analyzer_os::macos);
    }

    TEST(AnalyzerOsSelection, MissingFileIsNotClassified)
    {
        EXPECT_FALSE(sogen::detect_analyzer_os_from_file("/sogen-trace-fixture/definitely-not-here").has_value());
        EXPECT_FALSE(sogen::detect_analyzer_os_from_file(MACOS_FIXTURE_ROOT).has_value()) << "a directory is not an executable";
    }

    TEST(AnalyzerOsSelection, ExplicitFlagWinsAndIsStripped)
    {
        std::vector<std::string> storage{"analyzer", "--os=macos", "-s", "sample.exe"};
        auto argv = to_argv(storage);

        const auto invocation = sogen::select_analyzer_invocation(static_cast<int>(argv.size()), argv.data(), nullptr);

        EXPECT_EQ(invocation.os, sogen::analyzer_os::macos);
        ASSERT_EQ(invocation.arguments.size(), 3u);
        EXPECT_STREQ(invocation.arguments[0], "analyzer");
        EXPECT_STREQ(invocation.arguments[1], "-s");
        EXPECT_STREQ(invocation.arguments[2], "sample.exe");
    }

    TEST(AnalyzerOsSelection, SeparatedFlagFormIsAlsoStripped)
    {
        std::vector<std::string> storage{"analyzer", "--os", "linux", "sample"};
        auto argv = to_argv(storage);

        const auto invocation = sogen::select_analyzer_invocation(static_cast<int>(argv.size()), argv.data(), nullptr);

        EXPECT_EQ(invocation.os, sogen::analyzer_os::linux);
        ASSERT_EQ(invocation.arguments.size(), 2u);
        EXPECT_STREQ(invocation.arguments[1], "sample");
    }

    TEST(AnalyzerOsSelection, FileMagicDecidesWhenNoFlagIsGiven)
    {
        const std::string path = std::string(MACOS_FIXTURE_ROOT) + "/macho_static_arm64";
        std::vector<std::string> storage{"analyzer", "-e", MACOS_FIXTURE_ROOT, path};
        auto argv = to_argv(storage);

        const auto invocation = sogen::select_analyzer_invocation(static_cast<int>(argv.size()), argv.data(), nullptr);

        EXPECT_EQ(invocation.os, sogen::analyzer_os::macos) << "an option value that is a directory must not be sniffed";
        EXPECT_EQ(invocation.arguments.size(), 4u);
    }

    TEST(AnalyzerOsSelection, LegacyEnvironmentVariableStillSelectsLinux)
    {
        std::vector<std::string> storage{"analyzer", "sample"};
        auto argv = to_argv(storage);

        EXPECT_EQ(sogen::select_analyzer_invocation(static_cast<int>(argv.size()), argv.data(), "1").os, sogen::analyzer_os::linux);
        EXPECT_EQ(sogen::select_analyzer_invocation(static_cast<int>(argv.size()), argv.data(), "0").os, sogen::analyzer_os::windows);
        EXPECT_EQ(sogen::select_analyzer_invocation(static_cast<int>(argv.size()), argv.data(), nullptr).os, sogen::analyzer_os::windows);
    }

    TEST(AnalyzerOsSelection, EnvironmentFlagsAreResolvedIndependentlyOfArgv)
    {
        EXPECT_EQ(sogen::select_analyzer_os(nullptr, nullptr), sogen::analyzer_os::windows);
        EXPECT_EQ(sogen::select_analyzer_os("1", nullptr), sogen::analyzer_os::linux);
        EXPECT_EQ(sogen::select_analyzer_os(nullptr, "1"), sogen::analyzer_os::macos);
    }

    TEST(AnalyzerOsSelection, MacosEnvironmentFlagWinsOverLinux)
    {
        EXPECT_EQ(sogen::select_analyzer_os("1", "1"), sogen::analyzer_os::macos);
    }

    // The playground writes these itself, so anything other than exactly "1" is a bug on its side and
    // must not be read as consent to switch guests.
    TEST(AnalyzerOsSelection, OnlyExactlyOneIsTruthy)
    {
        EXPECT_EQ(sogen::select_analyzer_os(nullptr, "10"), sogen::analyzer_os::windows);
        EXPECT_EQ(sogen::select_analyzer_os(nullptr, ""), sogen::analyzer_os::windows);
        EXPECT_EQ(sogen::select_analyzer_os(nullptr, "true"), sogen::analyzer_os::windows);
        EXPECT_EQ(sogen::select_analyzer_os(nullptr, " 1"), sogen::analyzer_os::windows);
        EXPECT_EQ(sogen::select_analyzer_os("0", "0"), sogen::analyzer_os::windows);
    }

    TEST(AnalyzerOsSelection, EnvironmentSelectionIsUsableInConstantExpression)
    {
        static_assert(sogen::select_analyzer_os(nullptr, "1") == sogen::analyzer_os::macos);
        static_assert(sogen::select_analyzer_os("1", nullptr) == sogen::analyzer_os::linux);
        static_assert(sogen::select_analyzer_os(nullptr, nullptr) == sogen::analyzer_os::windows);
        SUCCEED();
    }

    TEST(AnalyzerOsSelection, MacosEnvironmentVariableSelectsMacos)
    {
        std::vector<std::string> storage{"analyzer", "sample"};
        auto argv = to_argv(storage);
        const auto argc = static_cast<int>(argv.size());

        EXPECT_EQ(sogen::select_analyzer_invocation(argc, argv.data(), nullptr, "1").os, sogen::analyzer_os::macos);
        EXPECT_EQ(sogen::select_analyzer_invocation(argc, argv.data(), "1", "1").os, sogen::analyzer_os::macos);
        EXPECT_EQ(sogen::select_analyzer_invocation(argc, argv.data(), nullptr, "0").os, sogen::analyzer_os::windows);
    }

    // The environment is the playground's coarse hint; an explicit flag or a real Mach-O header on the
    // command line is better evidence and has to outrank it.
    TEST(AnalyzerOsSelection, ExplicitFlagAndFileMagicBothOutrankTheEnvironment)
    {
        std::vector<std::string> flagged{"analyzer", "--os=windows", "sample"};
        auto flagged_argv = to_argv(flagged);
        EXPECT_EQ(sogen::select_analyzer_invocation(static_cast<int>(flagged_argv.size()), flagged_argv.data(), nullptr, "1").os,
                  sogen::analyzer_os::windows);

        std::vector<std::string> sniffed{"analyzer", std::string(MACOS_FIXTURE_ROOT) + "/macho_static_arm64"};
        auto sniffed_argv = to_argv(sniffed);
        EXPECT_EQ(sogen::select_analyzer_invocation(static_cast<int>(sniffed_argv.size()), sniffed_argv.data(), "1", nullptr).os,
                  sogen::analyzer_os::macos);
    }

    TEST(AnalyzerOsSelection, InvalidOsNameFallsThroughToDetection)
    {
        std::vector<std::string> storage{"analyzer", "--os=plan9", "sample"};
        auto argv = to_argv(storage);

        const auto invocation = sogen::select_analyzer_invocation(static_cast<int>(argv.size()), argv.data(), "1");

        EXPECT_EQ(invocation.os, sogen::analyzer_os::linux);
        EXPECT_EQ(invocation.arguments.size(), 3u) << "an unrecognised --os value is left in argv so CLI11 reports it";
    }
}
