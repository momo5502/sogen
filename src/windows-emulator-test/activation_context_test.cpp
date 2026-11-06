#include <gtest/gtest.h>

#include <sxs/activation_context.hpp>

#include <fstream>
#include <random>

namespace sogen::test
{
    namespace
    {
        class ActivationContextTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                this->root = std::filesystem::temp_directory_path() /
                             ("sogen-sxs-test-" + std::to_string(std::random_device{}()) + std::to_string(std::random_device{}()));
                std::filesystem::create_directories(this->root / "c" / "Windows" / "WinSxS" / "Manifests");
            }

            void TearDown() override
            {
                std::error_code ec{};
                std::filesystem::remove_all(this->root, ec);
            }

            void install(const std::string& stem, const std::string_view contents)
            {
                const auto winsxs = this->root / "c" / "Windows" / "WinSxS";
                std::filesystem::create_directories(winsxs / stem);
                std::ofstream stream(winsxs / "Manifests" / (stem + ".manifest"), std::ios::binary);
                stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
            }

            static std::vector<std::byte> build(const file_system& files, const std::string_view manifest)
            {
                mapped_module executable{};
                executable.module_path = windows_path{u"C:\\application\\program.exe"};
                executable.machine = IMAGE_FILE_MACHINE_AMD64;
                return sxs::build_activation_context(files, executable,
                                                     {.contents = std::as_bytes(std::span{manifest}), .path = executable.module_path},
                                                     std::nullopt, windows_path{u"C:\\Windows"}, "en-US");
            }

            static bool contains_string(const std::vector<std::byte>& blob, const std::u16string_view value)
            {
                return !std::ranges::search(blob, std::as_bytes(std::span{value})).empty();
            }

            std::filesystem::path root{};
        };
    }

    TEST_F(ActivationContextTest, SelectsPreferredLanguageAndSeesChangesBetweenRequests)
    {
        constexpr std::string_view manifest =
            R"(<assembly><dependency><dependentAssembly><assemblyIdentity name="Microsoft.Windows.Common-Controls" )"
            R"(version="6.0.0.0" processorArchitecture="amd64" publicKeyToken="6595b64144ccf1df" language="*"/>)"
            R"(</dependentAssembly></dependency></assembly>)";
        this->install("amd64_microsoft.windows.common-controls_6595b64144ccf1df_6.0.1.0_none_aaa",
                      R"(<assembly><assemblyIdentity name="Microsoft.Windows.Common-Controls" version="6.0.1.0" )"
                      R"(processorArchitecture="amd64" publicKeyToken="6595b64144ccf1df"/><file name="neutral.dll"/></assembly>)");
        file_system files{this->root};
        const auto neutral = this->build(files, manifest);
        EXPECT_TRUE(contains_string(neutral, u"neutral.dll"));

        this->install("amd64_microsoft.windows.common-controls_6595b64144ccf1df_6.0.0.1_en-us_bbb",
                      R"(<assembly><assemblyIdentity name="Microsoft.Windows.Common-Controls" version="6.0.0.1" )"
                      R"(processorArchitecture="amd64" publicKeyToken="6595b64144ccf1df" language="en-US"/>)"
                      R"(<file name="preferred.dll"/></assembly>)");
        const auto preferred = this->build(files, manifest);
        EXPECT_TRUE(contains_string(preferred, u"preferred.dll"));
        EXPECT_FALSE(contains_string(preferred, u"neutral.dll"));

        this->install("amd64_microsoft.windows.common-controls_6595b64144ccf1df_6.0.0.1_en-us_bbb",
                      R"(<assembly><assemblyIdentity name="Microsoft.Windows.Common-Controls" version="6.0.0.1" )"
                      R"(processorArchitecture="amd64" publicKeyToken="6595b64144ccf1df" language="en-US"/>)"
                      R"(<file name="updated.dll"/></assembly>)");
        const auto updated = this->build(files, manifest);
        EXPECT_TRUE(contains_string(updated, u"updated.dll"));
        EXPECT_FALSE(contains_string(updated, u"preferred.dll"));
    }

    TEST_F(ActivationContextTest, AppliesPublisherPolicyAndResolvesNestedDependencies)
    {
        constexpr std::string_view manifest =
            R"(<assembly><dependency><dependentAssembly><assemblyIdentity name="Sogen.First" version="1.0.0.0" )"
            R"(processorArchitecture="amd64"/></dependentAssembly></dependency></assembly>)";
        this->install("amd64_policy.1.0.sogen.first_abcd_1.0.0.0_none_aaa",
                      R"(<assembly><dependency><dependentAssembly><assemblyIdentity name="Sogen.First" )"
                      R"(processorArchitecture="amd64"/><bindingRedirect oldVersion="1.0.0.0" newVersion="2.0.0.0"/>)"
                      R"(</dependentAssembly></dependency></assembly>)");
        this->install("amd64_sogen.first_abcd_2.0.0.0_none_bbb",
                      R"(<assembly><assemblyIdentity name="Sogen.First" version="2.0.0.0" processorArchitecture="amd64"/>)"
                      R"(<file name="first.dll"/><dependency><dependentAssembly><assemblyIdentity name="Sogen.Second" )"
                      R"(version="1.0.0.0" processorArchitecture="amd64"/></dependentAssembly></dependency></assembly>)");
        this->install("amd64_sogen.second_abcd_1.0.0.0_none_ccc",
                      R"(<assembly><assemblyIdentity name="Sogen.Second" version="1.0.0.0" processorArchitecture="amd64"/>)"
                      R"(<file name="second.dll"/></assembly>)");

        const auto blob = this->build(file_system{this->root}, manifest);
        EXPECT_TRUE(contains_string(blob, u"first.dll"));
        EXPECT_TRUE(contains_string(blob, u"second.dll"));
    }
}
