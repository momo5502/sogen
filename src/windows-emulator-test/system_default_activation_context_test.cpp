#include "std_include.hpp"

namespace sogen::test
{
    namespace
    {
        constexpr uint32_t activation_context_magic = 0x78746341;

        std::vector<std::byte> synthetic_blob(const size_t size, const std::byte fill)
        {
            std::vector<std::byte> blob(size, fill);
            const auto total_size = static_cast<uint32_t>(size);
            std::memcpy(blob.data(), &activation_context_magic, sizeof(activation_context_magic));
            std::memcpy(blob.data() + 12, &total_size, sizeof(total_size));
            return blob;
        }

        class SystemDefaultActivationContextTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                this->root = std::filesystem::temp_directory_path() /
                             ("sogen-sdac-test-" + std::to_string(std::random_device{}()) + std::to_string(std::random_device{}()));
                std::filesystem::create_directories(this->root);

                std::error_code ec{};
                for (const auto* const directory : {"filesys", "registry"})
                {
                    std::filesystem::create_directory_symlink(get_emulator_root() / directory, this->root / directory, ec);
                    if (ec)
                    {
                        GTEST_SKIP() << "Cannot link the emulation root: " << ec.message();
                    }
                }
            }

            void TearDown() override
            {
                std::error_code ec{};
                std::filesystem::remove_all(this->root, ec);
            }

            void store(const std::string_view file_name, const std::vector<std::byte>& contents) const
            {
                std::filesystem::create_directories(this->root / "sxs");
                std::ofstream stream(this->root / "sxs" / file_name, std::ios::binary);
                stream.write(reinterpret_cast<const char*>(contents.data()), static_cast<std::streamsize>(contents.size()));
            }

            std::unique_ptr<windows_emulator> start(const windows_path& application) const
            {
                emulator_interfaces interfaces{};
                interfaces.ui = std::make_unique<null_ui_backend>();
                interfaces.audio = std::make_unique<null_audio_backend>();
                interfaces.console = std::make_unique<null_console_backend>();
                emulator_settings settings{.disable_logging = true};
                settings.emulation_root = this->root;
                auto emu = std::make_unique<windows_emulator>(create_x86_64_emulator_from_environment(),
                                                              application_settings{.application = application}, settings,
                                                              emulator_callbacks{}, std::move(interfaces));
                emu->setup_process_if_necessary();
                return emu;
            }

            static std::vector<std::byte> system_default_context(const windows_emulator& emu, const size_t size)
            {
                const auto address = emu.process.peb64.read().SystemDefaultActivationContextData;
                return address ? emu.memory.read_memory(address, size) : std::vector<std::byte>{};
            }

            std::filesystem::path root{};
        };
    }

    TEST_F(SystemDefaultActivationContextTest, NativeProcessGetsTheAmd64ContextInItsPeb)
    {
        const auto amd64 = synthetic_blob(0x80, std::byte{0x64});
        this->store("system-default-amd64.bin", amd64);
        this->store("system-default-x86.bin", synthetic_blob(0x58, std::byte{0x86}));

        const auto emu = this->start(windows_path{u"C:\\test-sample.exe"});

        ASSERT_FALSE(emu->process.is_wow64_process);
        EXPECT_TRUE(system_default_context(*emu, amd64.size()) == amd64);
    }

    TEST_F(SystemDefaultActivationContextTest, Wow64ProcessGetsTheX86ContextInPeb64)
    {
        if (!std::filesystem::exists(get_emulator_root() / "filesys" / "c" / "windows" / "syswow64" / "cmd.exe"))
        {
            GTEST_SKIP() << "The emulation root has no 32-bit executable";
        }

        const auto x86 = synthetic_blob(0x58, std::byte{0x86});
        this->store("system-default-amd64.bin", synthetic_blob(0x80, std::byte{0x64}));
        this->store("system-default-x86.bin", x86);

        const auto emu = this->start(windows_path{u"C:\\Windows\\SysWOW64\\cmd.exe"});

        ASSERT_TRUE(emu->process.is_wow64_process);
        EXPECT_TRUE(system_default_context(*emu, x86.size()) == x86);
        EXPECT_LT(emu->process.peb64.read().SystemDefaultActivationContextData, uint64_t{1} << 32);
    }

    TEST_F(SystemDefaultActivationContextTest, RootWithoutTheCapturedContextLeavesThePebFieldUnset)
    {
        const auto emu = this->start(windows_path{u"C:\\test-sample.exe"});

        EXPECT_EQ(emu->process.peb64.read().SystemDefaultActivationContextData, 0u);
    }
}
