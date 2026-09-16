#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include <sys/stat.h>

#include <macos_input.hpp>
#include <utils/io.hpp>

#include "fixture_utils.hpp"

namespace
{
    void write_bytes(const std::filesystem::path& path, const std::vector<std::byte>& data)
    {
        std::filesystem::create_directories(path.parent_path());
        ASSERT_TRUE(sogen::utils::io::write_file(path, data));
    }

    std::vector<std::byte> udif_image(const size_t payload_size)
    {
        std::vector<std::byte> data(payload_size + 512, std::byte{0x11});
        const std::array<char, 4> magic{'k', 'o', 'l', 'y'};
        for (size_t i = 0; i < magic.size(); ++i)
        {
            data[payload_size + i] = static_cast<std::byte>(magic[i]);
        }
        data[payload_size + 7] = std::byte{0x04};
        data[payload_size + 9] = std::byte{0x02};
        return data;
    }

    std::vector<std::byte> bytes_of(const std::string_view text)
    {
        std::vector<std::byte> data{};
        for (const char ch : text)
        {
            data.push_back(static_cast<std::byte>(ch));
        }
        return data;
    }

    TEST(MacosInput, ClassifiesAThinMachO)
    {
        const sogen::test::temp_directory dir{"input-macho"};
        const auto path = dir.path() / "program";
        write_bytes(path, {std::byte{0xcf}, std::byte{0xfa}, std::byte{0xed}, std::byte{0xfe}, std::byte{0x0c}, std::byte{0x00},
                           std::byte{0x00}, std::byte{0x01}});
        EXPECT_EQ(sogen::classify_macos_input(path), sogen::macos_input_kind::mach_o);
    }

    TEST(MacosInput, ClassifiesAFatMachO)
    {
        const sogen::test::temp_directory dir{"input-fat"};
        const auto path = dir.path() / "universal";
        write_bytes(path, {std::byte{0xca}, std::byte{0xfe}, std::byte{0xba}, std::byte{0xbe}, std::byte{0x00}, std::byte{0x00},
                           std::byte{0x00}, std::byte{0x02}});
        EXPECT_EQ(sogen::classify_macos_input(path), sogen::macos_input_kind::fat_mach_o);
    }

    TEST(MacosInput, ClassifiesAUdifDiskImageByItsTrailer)
    {
        const sogen::test::temp_directory dir{"input-dmg"};
        const auto path = dir.path() / "sample.dmg";
        write_bytes(path, udif_image(4096));
        EXPECT_EQ(sogen::classify_macos_input(path), sogen::macos_input_kind::disk_image);
    }

    TEST(MacosInput, ClassifiesAnEncryptedDiskImageByItsHeader)
    {
        const sogen::test::temp_directory dir{"input-enc"};
        const auto path = dir.path() / "secret.dmg";
        auto data = bytes_of("encrcdsa");
        data.resize(1024, std::byte{0});
        write_bytes(path, data);
        EXPECT_EQ(sogen::classify_macos_input(path), sogen::macos_input_kind::encrypted_disk_image);
    }

    TEST(MacosInput, TreatsADmgExtensionWithoutAKolyTrailerAsADiskImage)
    {
        const sogen::test::temp_directory dir{"input-rawdmg"};
        const auto path = dir.path() / "raw.dmg";
        std::vector<std::byte> data(2048, std::byte{0x5a});
        write_bytes(path, data);
        EXPECT_EQ(sogen::classify_macos_input(path), sogen::macos_input_kind::disk_image);
    }

    TEST(MacosInput, ClassifiesAnAppBundleAndAPlainDirectory)
    {
        const sogen::test::temp_directory dir{"input-bundle"};
        std::filesystem::create_directories(dir.path() / "Widget.app" / "Contents" / "MacOS");
        EXPECT_EQ(sogen::classify_macos_input(dir.path() / "Widget.app"), sogen::macos_input_kind::app_bundle);
        EXPECT_EQ(sogen::classify_macos_input(dir.path()), sogen::macos_input_kind::directory);
    }

    TEST(MacosInput, ClassifiesAMissingPath)
    {
        const sogen::test::temp_directory dir{"input-missing"};
        EXPECT_EQ(sogen::classify_macos_input(dir.path() / "absent"), sogen::macos_input_kind::missing);
    }

    TEST(MacosInput, NeverReadsANonRegularFile)
    {
        EXPECT_EQ(sogen::classify_macos_input("/dev/zero"), sogen::macos_input_kind::special_file);
    }

    TEST(MacosInput, ClassifiesAnEmptyFileAsUnknown)
    {
        const sogen::test::temp_directory dir{"input-empty"};
        const auto path = dir.path() / "empty";
        write_bytes(path, {});
        EXPECT_EQ(sogen::classify_macos_input(path), sogen::macos_input_kind::unknown);
    }

    // The brief's koly fixture is named ".dmg", so the extension alone already yields disk_image.
    // These two pin the trailer check itself: same bytes, same name, koly present versus absent.
    TEST(MacosInput, ClassifiesAUdifDiskImageThatIsNotNamedDmg)
    {
        const sogen::test::temp_directory dir{"input-koly-noext"};
        const auto path = dir.path() / "payload.bin";
        write_bytes(path, udif_image(4096));
        EXPECT_EQ(sogen::classify_macos_input(path), sogen::macos_input_kind::disk_image);
    }

    TEST(MacosInput, DoesNotClassifyANonDmgFileWithoutAKolyTrailerAsADiskImage)
    {
        const sogen::test::temp_directory dir{"input-nokoly-noext"};
        const auto path = dir.path() / "payload.bin";
        write_bytes(path, std::vector<std::byte>(4096 + 512, std::byte{0x11}));
        EXPECT_EQ(sogen::classify_macos_input(path), sogen::macos_input_kind::unknown);
    }

    TEST(MacosInput, IgnoresAKolyMagicThatIsNotAtTheTrailerOffset)
    {
        const sogen::test::temp_directory dir{"input-koly-misplaced"};
        const auto path = dir.path() / "payload.bin";

        std::vector<std::byte> data(4096 + 512, std::byte{0x11});
        const std::array<char, 4> magic{'k', 'o', 'l', 'y'};
        for (size_t i = 0; i < magic.size(); ++i)
        {
            data[i] = static_cast<std::byte>(magic[i]);
            data[data.size() - 256 + i] = static_cast<std::byte>(magic[i]);
        }

        write_bytes(path, data);
        EXPECT_EQ(sogen::classify_macos_input(path), sogen::macos_input_kind::unknown);
    }

    TEST(MacosInput, ClassifiesA32BitMachOAndAFat64MachO)
    {
        const sogen::test::temp_directory dir{"input-magics"};

        const auto thin32 = dir.path() / "thin32";
        write_bytes(thin32, {std::byte{0xce}, std::byte{0xfa}, std::byte{0xed}, std::byte{0xfe}, std::byte{0x07}, std::byte{0x00},
                             std::byte{0x00}, std::byte{0x00}});
        EXPECT_EQ(sogen::classify_macos_input(thin32), sogen::macos_input_kind::mach_o);

        const auto fat64 = dir.path() / "fat64";
        write_bytes(fat64, {std::byte{0xca}, std::byte{0xfe}, std::byte{0xba}, std::byte{0xbf}, std::byte{0x00}, std::byte{0x00},
                            std::byte{0x00}, std::byte{0x02}});
        EXPECT_EQ(sogen::classify_macos_input(fat64), sogen::macos_input_kind::fat_mach_o);
    }

    TEST(MacosInput, RecognisesADmgExtensionCaseInsensitively)
    {
        const sogen::test::temp_directory dir{"input-dmg-case"};
        const auto path = dir.path() / "sample.DMG";
        write_bytes(path, std::vector<std::byte>(2048, std::byte{0x5a}));
        EXPECT_EQ(sogen::classify_macos_input(path), sogen::macos_input_kind::disk_image);
    }

    TEST(MacosInput, PrefersTheMachOHeaderOverADmgExtension)
    {
        const sogen::test::temp_directory dir{"input-macho-dmg"};
        const auto path = dir.path() / "disguised.dmg";
        write_bytes(path, {std::byte{0xcf}, std::byte{0xfa}, std::byte{0xed}, std::byte{0xfe}, std::byte{0x0c}, std::byte{0x00},
                           std::byte{0x00}, std::byte{0x01}});
        EXPECT_EQ(sogen::classify_macos_input(path), sogen::macos_input_kind::mach_o);
    }

    TEST(MacosInput, ClassifiesAnAppBundlePathWithATrailingSeparator)
    {
        const sogen::test::temp_directory dir{"input-bundle-slash"};
        std::filesystem::create_directories(dir.path() / "Widget.app" / "Contents" / "MacOS");
        const auto with_separator = (dir.path() / "Widget.app").string() + "/";
        EXPECT_EQ(sogen::classify_macos_input(with_separator), sogen::macos_input_kind::app_bundle);
    }

    TEST(MacosInput, FollowsSymlinksAndRefusesTheOnesPointingAtDevices)
    {
        const sogen::test::temp_directory dir{"input-symlink"};

        const auto target = dir.path() / "program";
        write_bytes(target, {std::byte{0xcf}, std::byte{0xfa}, std::byte{0xed}, std::byte{0xfe}, std::byte{0x0c}, std::byte{0x00},
                             std::byte{0x00}, std::byte{0x01}});

        std::error_code code{};
        std::filesystem::create_symlink(target, dir.path() / "link-to-program", code);
        ASSERT_FALSE(code) << code.message();
        EXPECT_EQ(sogen::classify_macos_input(dir.path() / "link-to-program"), sogen::macos_input_kind::mach_o);

        std::filesystem::create_symlink("/dev/zero", dir.path() / "link-to-device", code);
        ASSERT_FALSE(code) << code.message();
        EXPECT_EQ(sogen::classify_macos_input(dir.path() / "link-to-device"), sogen::macos_input_kind::special_file);

        std::filesystem::create_symlink(dir.path() / "nowhere", dir.path() / "dangling", code);
        ASSERT_FALSE(code) << code.message();
        EXPECT_EQ(sogen::classify_macos_input(dir.path() / "dangling"), sogen::macos_input_kind::missing);
    }

    // Opening a FIFO with no writer blocks forever: classification must decide from stat alone.
    TEST(MacosInput, DoesNotOpenAFifo)
    {
        const sogen::test::temp_directory dir{"input-fifo"};
        const auto path = dir.path() / "pipe";
        ASSERT_EQ(::mkfifo(path.string().c_str(), 0600), 0);
        EXPECT_EQ(sogen::classify_macos_input(path), sogen::macos_input_kind::special_file);
    }

    TEST(MacosInput, ClassifiesAFileShorterThanAMagic)
    {
        const sogen::test::temp_directory dir{"input-tiny"};
        const auto path = dir.path() / "tiny";
        write_bytes(path, {std::byte{0xcf}, std::byte{0xfa}, std::byte{0xed}});
        EXPECT_EQ(sogen::classify_macos_input(path), sogen::macos_input_kind::unknown);
    }

    TEST(MacosInput, DescribesADiskImageWithTheCommandsThatMountIt)
    {
        const auto text = sogen::describe_unsupported_input("/tmp/sample.dmg", sogen::macos_input_kind::disk_image);
        EXPECT_NE(text.find("Apple Disk Image"), std::string::npos);
        EXPECT_NE(text.find("hdiutil attach -readonly -nobrowse"), std::string::npos);
        EXPECT_NE(text.find("hdiutil detach"), std::string::npos);
        EXPECT_NE(text.find("run-macos-dmg.sh"), std::string::npos);
        EXPECT_NE(text.find("/tmp/sample.dmg"), std::string::npos);
    }

    TEST(MacosInput, DescribesAnEncryptedDiskImage)
    {
        const auto text = sogen::describe_unsupported_input("/tmp/secret.dmg", sogen::macos_input_kind::encrypted_disk_image);
        EXPECT_NE(text.find("encrypted"), std::string::npos);
        EXPECT_NE(text.find("password"), std::string::npos);
    }

    TEST(MacosInput, DescribesTheOtherUnsupportedKinds)
    {
        for (const auto kind : {sogen::macos_input_kind::missing, sogen::macos_input_kind::special_file, sogen::macos_input_kind::directory,
                                sogen::macos_input_kind::unknown})
        {
            EXPECT_FALSE(sogen::describe_unsupported_input("/tmp/thing", kind).empty());
        }

        EXPECT_TRUE(sogen::describe_unsupported_input("/tmp/thing", sogen::macos_input_kind::mach_o).empty());
        EXPECT_TRUE(sogen::describe_unsupported_input("/tmp/thing", sogen::macos_input_kind::app_bundle).empty());
    }
}
