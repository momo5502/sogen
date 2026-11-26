#include "std_include.hpp"

#include <sxs/activation_context.hpp>

#include "activation_context_captures.hpp"

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

            static uint32_t read_u32(const std::vector<std::byte>& blob, const size_t offset)
            {
                uint32_t value{};
                std::memcpy(&value, blob.data() + offset, sizeof(value));
                return value;
            }

            static std::u16string read_string(const std::vector<std::byte>& blob, const size_t offset, const size_t byte_length)
            {
                std::u16string value(byte_length / sizeof(char16_t), u'\0');
                std::memcpy(value.data(), blob.data() + offset, byte_length);
                return value;
            }

            struct toc_entry
            {
                uint32_t offset{};
                uint32_t length{};
                uint32_t format{};
            };

            static std::optional<toc_entry> find_toc_entry(const std::vector<std::byte>& blob, const uint32_t id)
            {
                const auto toc_offset = read_u32(blob, 16);
                const auto toc_count = read_u32(blob, toc_offset + 4);
                const auto toc_entries = read_u32(blob, toc_offset + 8);
                for (uint32_t index = 0; index < toc_count; ++index)
                {
                    const size_t entry = toc_entries + index * 16;
                    if (read_u32(blob, entry) == id)
                    {
                        return toc_entry{
                            .offset = read_u32(blob, entry + 4), .length = read_u32(blob, entry + 8), .format = read_u32(blob, entry + 12)};
                    }
                }
                return std::nullopt;
            }

            struct window_class_entry
            {
                std::u16string key{};
                uint32_t pseudokey{};
                uint32_t key_offset{};
                uint32_t key_length{};
                uint32_t data_offset{};
                uint32_t data_length{};
                uint32_t roster_index{};
                std::u16string name{};
                uint32_t name_offset{};
                std::u16string module{};
                uint32_t module_offset{};
                bool reachable_through_hash_table{};
            };

            struct window_class_section
            {
                size_t header_size{};
                uint32_t flags{};
                std::vector<window_class_entry> entries{};
            };

            static std::optional<window_class_section> read_window_class_section(const std::vector<std::byte>& blob)
            {
                constexpr uint32_t window_class_section_id = 3;
                const auto toc_offset = read_u32(blob, 16);
                const auto toc_count = read_u32(blob, toc_offset + 4);
                const auto toc_entries = read_u32(blob, toc_offset + 8);
                for (uint32_t index = 0; index < toc_count; ++index)
                {
                    const size_t toc_entry = toc_entries + index * 16;
                    if (read_u32(blob, toc_entry) != window_class_section_id)
                    {
                        continue;
                    }

                    const size_t section = read_u32(blob, toc_entry + 4);
                    window_class_section result{.header_size = read_u32(blob, section + 4), .flags = read_u32(blob, section + 16)};
                    const auto count = read_u32(blob, section + 20);
                    const size_t entries = section + read_u32(blob, section + 24);
                    const auto search_structure = read_u32(blob, section + 32);
                    const size_t hash_table = section + search_structure;
                    const auto bucket_count = search_structure == 0 ? 1 : read_u32(blob, hash_table);
                    const size_t buckets = section + (search_structure == 0 ? 0 : read_u32(blob, hash_table + 4));
                    for (uint32_t entry_index = 0; entry_index < count; ++entry_index)
                    {
                        const size_t entry = entries + entry_index * 24;
                        window_class_entry parsed{.pseudokey = read_u32(blob, entry),
                                                  .key_offset = read_u32(blob, entry + 4),
                                                  .key_length = read_u32(blob, entry + 8),
                                                  .data_offset = read_u32(blob, entry + 12),
                                                  .data_length = read_u32(blob, entry + 16),
                                                  .roster_index = read_u32(blob, entry + 20)};
                        parsed.key = read_string(blob, section + parsed.key_offset, parsed.key_length);
                        const size_t data = section + parsed.data_offset;
                        parsed.name_offset = read_u32(blob, data + 12);
                        parsed.name = read_string(blob, data + parsed.name_offset, read_u32(blob, data + 8));
                        parsed.module_offset = read_u32(blob, data + 20);
                        parsed.module = read_string(blob, section + parsed.module_offset, read_u32(blob, data + 16));

                        if (search_structure == 0)
                        {
                            parsed.reachable_through_hash_table = true;
                        }
                        else
                        {
                            const size_t bucket = buckets + (parsed.pseudokey % bucket_count) * 8;
                            const size_t chain = section + read_u32(blob, bucket + 4);
                            for (uint32_t chain_index = 0; chain_index < read_u32(blob, bucket); ++chain_index)
                            {
                                parsed.reachable_through_hash_table |= read_u32(blob, chain + chain_index * 4) == entry - section;
                            }
                        }
                        result.entries.push_back(std::move(parsed));
                    }
                    return result;
                }
                return std::nullopt;
            }

            static std::vector<std::byte> synthetic_blob(const size_t size, const std::byte fill)
            {
                std::vector<std::byte> blob(size, fill);
                const uint32_t magic = 0x78746341;
                const auto total_size = static_cast<uint32_t>(size);
                std::memcpy(blob.data(), &magic, sizeof(magic));
                std::memcpy(blob.data() + 12, &total_size, sizeof(total_size));
                return blob;
            }

            void store_captured(const std::string_view file_name, const std::span<const std::byte> contents) const
            {
                std::filesystem::create_directories(this->root / "emulation" / "sxs");
                std::ofstream stream(this->root / "emulation" / "sxs" / file_name, std::ios::binary);
                stream.write(reinterpret_cast<const char*>(contents.data()), static_cast<std::streamsize>(contents.size()));
            }

            static std::string window_class_manifest(const uint32_t window_classes)
            {
                const auto count = std::to_string(window_classes);
                std::string manifest = R"(<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">)"
                                       R"(<assemblyIdentity type="win32" name="SogenProbe.SdacBuckets)" +
                                       count + R"(" version="1.0.0.0" processorArchitecture="*"/><file name="sdac_probe_wc)" + count +
                                       R"(.dll">)";
                for (uint32_t index = 1; index <= window_classes; ++index)
                {
                    manifest += "<windowClass>SogenSdacWc" + count + (index < 10 ? "_0" : "_") + std::to_string(index) + "</windowClass>";
                }
                return manifest + "</file></assembly>";
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

    TEST_F(ActivationContextTest, EmitsWindowClassRedirectionSectionLikeWindows)
    {
        constexpr std::string_view manifest =
            R"(<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">)"
            R"(<assemblyIdentity type="win32" name="SogenProbe.WndClass" version="1.0.0.0" processorArchitecture="amd64"/>)"
            R"(<file name="probe_wc_a.dll"><windowClass>ProbeDefault</windowClass>)"
            R"(<windowClass versioned="yes">ProbeVersionedYes</windowClass>)"
            R"(<windowClass versioned="no">ProbeVersionedNo</windowClass>)"
            R"(<windowClass versioned="yes">ProbeMixedCase</windowClass></file>)"
            R"(<file name="probe_wc_b.dll"><windowClass versioned="no">ProbeSecondDll</windowClass></file>)"
            R"(<file name="probe_wc_c.dll"><windowClass versioned="no">Button</windowClass></file></assembly>)";

        struct expected_entry
        {
            std::u16string_view key;
            uint32_t pseudokey;
            uint32_t key_offset;
            uint32_t key_length;
            uint32_t data_offset;
            uint32_t data_length;
            std::u16string_view name;
            std::u16string_view module;
            uint32_t module_offset;
        };

        constexpr std::array expected{
            expected_entry{u"ProbeDefault", 0x7a1f4911, 0xbc, 24, 0xd8, 96, u"1.0.0.0!ProbeDefault", u"probe_wc_a.dll", 0x11a},
            expected_entry{u"ProbeVersionedYes", 0x305ee6e0, 0x138, 34, 0x15c, 106, u"1.0.0.0!ProbeVersionedYes", u"probe_wc_a.dll", 0x1a8},
            expected_entry{u"ProbeVersionedNo", 0xc532a388, 0x1c8, 32, 0x1ec, 88, u"ProbeVersionedNo", u"probe_wc_a.dll", 0x226},
            expected_entry{u"ProbeMixedCase", 0x86d6d7fb, 0x244, 28, 0x264, 100, u"1.0.0.0!ProbeMixedCase", u"probe_wc_a.dll", 0x2aa},
            expected_entry{u"ProbeSecondDll", 0x10967480, 0x2c8, 28, 0x2e8, 84, u"ProbeSecondDll", u"probe_wc_b.dll", 0x31e},
            expected_entry{u"Button", 0xb2d54652, 0x33c, 12, 0x34c, 68, u"Button", u"probe_wc_c.dll", 0x372},
        };

        const auto section = read_window_class_section(this->build(file_system{this->root}, manifest));
        ASSERT_TRUE(section);
        EXPECT_EQ(section->header_size, 44u);
        EXPECT_EQ(section->flags, 1u);
        ASSERT_EQ(section->entries.size(), expected.size());
        for (size_t index = 0; index < expected.size(); ++index)
        {
            const auto& actual = section->entries[index];
            const auto& wanted = expected[index];
            EXPECT_EQ(actual.key, wanted.key);
            EXPECT_EQ(actual.pseudokey, wanted.pseudokey);
            EXPECT_EQ(actual.key_offset, wanted.key_offset);
            EXPECT_EQ(actual.key_length, wanted.key_length);
            EXPECT_EQ(actual.data_offset, wanted.data_offset);
            EXPECT_EQ(actual.data_length, wanted.data_length);
            EXPECT_EQ(actual.roster_index, 1u);
            EXPECT_EQ(actual.name_offset, 0x18u);
            EXPECT_EQ(actual.name, wanted.name);
            EXPECT_EQ(actual.module, wanted.module);
            EXPECT_EQ(actual.module_offset, wanted.module_offset);
            EXPECT_TRUE(actual.reachable_through_hash_table);
        }

        constexpr std::string_view dependent_manifest =
            R"(<assembly><assemblyIdentity name="Sogen.App" version="1.0.0.0" processorArchitecture="amd64"/>)"
            R"(<dependency><dependentAssembly><assemblyIdentity name="Microsoft.Windows.Common-Controls" )"
            R"(version="6.0.0.0" processorArchitecture="amd64" publicKeyToken="6595b64144ccf1df" language="*"/>)"
            R"(</dependentAssembly></dependency></assembly>)";
        this->install("amd64_microsoft.windows.common-controls_6595b64144ccf1df_6.0.26100.9818_none_aaa",
                      R"(<assembly><assemblyIdentity name="Microsoft.Windows.Common-Controls" version="6.0.26100.9818" )"
                      R"(processorArchitecture="amd64" publicKeyToken="6595b64144ccf1df"/>)"
                      R"(<file name="comctl32.dll"><windowClass>msctls_statusbar32</windowClass></file></assembly>)");

        const auto dependent_section = read_window_class_section(this->build(file_system{this->root}, dependent_manifest));
        ASSERT_TRUE(dependent_section);
        ASSERT_EQ(dependent_section->entries.size(), 1u);
        const auto& entry = dependent_section->entries.front();
        EXPECT_EQ(entry.key, u"msctls_statusbar32");
        EXPECT_EQ(entry.pseudokey, 0x2a615cffu);
        EXPECT_EQ(entry.roster_index, 2u);
        EXPECT_EQ(entry.name, u"6.0.26100.9818!msctls_statusbar32");
        EXPECT_EQ(entry.module, u"comctl32.dll");
        EXPECT_EQ(entry.data_length, 118u);
        EXPECT_TRUE(entry.reachable_through_hash_table);
    }

    TEST_F(ActivationContextTest, HashesWindowClassNamesWithTheNlsUpcaseTable)
    {
        const file_system files{get_emulator_root() / "filesys"};
        const auto pseudokeys = [&](const std::string& class_names) {
            const auto section = read_window_class_section(
                this->build(files, R"(<assembly><assemblyIdentity name="Sogen.Unicode" version="1.0.0.0" processorArchitecture="amd64"/>)"
                                   R"(<file name="unicode.dll">)" +
                                       class_names + "</file></assembly>"));
            std::vector<uint32_t> result{};
            for (const auto& entry : section->entries)
            {
                result.push_back(entry.pseudokey);
            }
            return result;
        };

        EXPECT_EQ(pseudokeys("<windowClass>Fen\xC3\xAAtre</windowClass><windowClass>\xC3\xBF</windowClass>"),
                  (std::vector<uint32_t>{0x178, 0x1bc8410c}));
        EXPECT_EQ(pseudokeys("<windowClass>\xC5\xB8</windowClass>"), (std::vector<uint32_t>{0x178}));
    }

    TEST_F(ActivationContextTest, CapturedSystemDefaultContextIsReturnedVerbatimPerArchitecture)
    {
        const auto amd64 = synthetic_blob(0x80, std::byte{0x64});
        const auto x86 = synthetic_blob(0x58, std::byte{0x86});
        this->store_captured("system-default-amd64.bin", amd64);
        this->store_captured("system-default-x86.bin", x86);

        const auto emulation_root = this->root / "emulation";
        EXPECT_TRUE(sxs::obtain_system_default_activation_context(emulation_root, sxs::system_default_architecture::amd64) == amd64);
        EXPECT_TRUE(sxs::obtain_system_default_activation_context(emulation_root, sxs::system_default_architecture::x86) == x86);
    }

    TEST_F(ActivationContextTest, CapturedSystemDefaultContextThatIsNotWholeIsIgnored)
    {
        const auto whole = synthetic_blob(0x80, std::byte{0x64});
        auto truncated = whole;
        truncated.pop_back();
        auto extended = whole;
        extended.push_back(std::byte{0});
        auto foreign = whole;
        foreign[0] = std::byte{0};

        const auto emulation_root = this->root / "emulation";
        for (const auto& contents : {truncated, extended, foreign, std::vector<std::byte>(15, std::byte{0x41})})
        {
            this->store_captured("system-default-amd64.bin", contents);
            EXPECT_TRUE(sxs::obtain_system_default_activation_context(emulation_root, sxs::system_default_architecture::amd64).empty());
        }
    }

    TEST_F(ActivationContextTest, RootWithoutTheCapturedSystemDefaultContextHasNone)
    {
        EXPECT_TRUE(sxs::obtain_system_default_activation_context({}, sxs::system_default_architecture::amd64).empty());
        EXPECT_TRUE(
            sxs::obtain_system_default_activation_context(this->root / "emulation", sxs::system_default_architecture::amd64).empty());

        this->store_captured("system-default-x86.bin", synthetic_blob(0x58, std::byte{0x86}));
        EXPECT_TRUE(
            sxs::obtain_system_default_activation_context(this->root / "emulation", sxs::system_default_architecture::amd64).empty());
    }

    TEST_F(ActivationContextTest, WindowClassBucketCountFollowsTheCapturedSweep)
    {
        for (const auto& row : sdac_capture::bucket_sweep)
        {
            const auto manifest = window_class_manifest(row.window_classes);

            const auto blob = this->build(file_system{this->root}, manifest);
            const auto toc = find_toc_entry(blob, 3);
            ASSERT_TRUE(toc) << row.window_classes << " window classes";
            EXPECT_EQ(toc->length, row.section_length) << row.window_classes << " window classes";
            EXPECT_EQ(read_u32(blob, toc->offset + 16), row.flags) << row.window_classes << " window classes";
            EXPECT_EQ(read_u32(blob, toc->offset + 20), row.window_classes);
            const auto search_structure = read_u32(blob, toc->offset + 32);
            EXPECT_EQ(search_structure, row.search_structure_offset) << row.window_classes << " window classes";
            if (search_structure != 0)
            {
                EXPECT_EQ(read_u32(blob, toc->offset + search_structure), row.bucket_count) << row.window_classes << " window classes";
            }
            else
            {
                EXPECT_EQ(row.bucket_count, 0u);
            }
        }
    }
}
