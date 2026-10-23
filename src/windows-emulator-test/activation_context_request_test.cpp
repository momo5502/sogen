#include "emulation_test_utils.hpp"
#include <ports/api_port.hpp>

namespace sogen::test
{
    namespace
    {
        class ActivationContextRequestTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                emulator_interfaces interfaces{};
                interfaces.ui = std::make_unique<null_ui_backend>();
                interfaces.audio = std::make_unique<null_audio_backend>();
                interfaces.console = std::make_unique<null_console_backend>();
                this->emu = std::make_unique<windows_emulator>(
                    create_x86_64_emulator_from_environment(),
                    emulator_settings{.disable_logging = true, .emulation_root = get_emulator_root(), .load_registry = false},
                    emulator_callbacks{}, std::move(interfaces));
                this->storage = this->emu->memory.allocate_memory(0x4000, memory_permission::read_write);
                this->port = create_api_port();
                this->request = {
                    .send_buffer = this->storage, .send_buffer_length = 0x210, .recv_buffer = this->storage, .recv_buffer_length = 0x210};
                this->write<uint32_t>(0x08, 0x0001001e);
                this->write<uint32_t>(0x1c, PROCESSOR_ARCHITECTURE_AMD64);
                this->write_string(0x38, u"C:\\application\\program.exe", this->storage + 0x1000);
                this->write_string(0xa0, u"C:\\application", this->storage + 0x1200);
                constexpr std::string_view manifest =
                    "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">"
                    "<assemblyIdentity name=\"Sogen.Test\" version=\"1.0.0.0\" processorArchitecture=\"amd64\"/>"
                    "</assembly>";
                this->emu->memory.write_memory(this->storage + 0x2000, manifest.data(), manifest.size());
                this->write<uint64_t>(0x58, this->storage + 0x2000);
                this->write<uint64_t>(0x60, manifest.size());
                this->write<uint64_t>(0xd0, this->storage + 0x3000);
            }

            template <typename T>
            void write(uint64_t offset, T value)
            {
                emulator_object<T>{this->emu->memory, this->storage + offset}.write(value);
            }

            void write_string(uint64_t offset, std::u16string_view value, uint64_t buffer)
            {
                const auto size = static_cast<USHORT>(value.size() * sizeof(char16_t));
                this->emu->memory.write_memory(buffer, value.data(), size);
                this->write<UNICODE_STRING<EmulatorTraits<Emu64>>>(offset, {.Length = size, .MaximumLength = size, .Buffer = buffer});
            }

            std::unique_ptr<windows_emulator> emu{};
            std::unique_ptr<sogen::port> port{};
            lpc_request_context request{};
            uint64_t storage{};
        };
    }

    TEST_F(ActivationContextRequestTest, MapsManifestContextAndWritesOutputPointer)
    {
        ASSERT_EQ(STATUS_SUCCESS, this->port->handle_request(*this->emu, this->request).status);
        const auto address = emulator_object<uint64_t>{this->emu->memory, this->storage + 0x3000}.read();
        ASSERT_NE(0, address);
        EXPECT_EQ(0x78746341, (emulator_object<uint32_t>{this->emu->memory, address}.read()));
        EXPECT_EQ(memory_region_kind::file_section_view, this->emu->memory.get_region_kind(address));
        EXPECT_EQ(memory_permission::read, this->emu->memory.get_region_info(address).permissions.common);
        EXPECT_EQ(1, (emulator_object<uint32_t>{this->emu->memory, this->storage + 0xe4}.read()));
        EXPECT_TRUE(this->emu->memory.release_memory(address, 0));
    }

    TEST_F(ActivationContextRequestTest, RejectsTruncatedMessageAndInaccessibleManifest)
    {
        this->request.send_buffer_length = 0x40;
        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->port->handle_request(*this->emu, this->request).status);
        this->request.send_buffer_length = 0x210;
        this->write<uint64_t>(0x58, 0);
        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->port->handle_request(*this->emu, this->request).status);
        EXPECT_EQ(0, (emulator_object<uint64_t>{this->emu->memory, this->storage + 0x3000}.read()));
    }

    TEST_F(ActivationContextRequestTest, RejectsMisalignedUnicodeLength)
    {
        this->write<uint16_t>(0x38, 1);
        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->port->handle_request(*this->emu, this->request).status);
    }

    TEST_F(ActivationContextRequestTest, AcceptsWow64ProcessorArchitecture)
    {
        this->write<uint32_t>(0x1c, PROCESSOR_ARCHITECTURE_IA32_ON_WIN64);
        constexpr std::string_view manifest =
            "<assembly manifestVersion=\"1.0\"><assemblyIdentity name=\"Sogen.X86.Test\" version=\"1.0.0.0\" "
            "processorArchitecture=\"x86\"/></assembly>";
        this->emu->memory.write_memory(this->storage + 0x2000, manifest.data(), manifest.size());
        this->write<uint64_t>(0x60, manifest.size());
        ASSERT_EQ(STATUS_SUCCESS, this->port->handle_request(*this->emu, this->request).status);
        const auto address = emulator_object<uint64_t>{this->emu->memory, this->storage + 0x3000}.read();
        EXPECT_NE(0, address);
        EXPECT_TRUE(this->emu->memory.release_memory(address, 0));
    }

    TEST_F(ActivationContextRequestTest, AcceptsArm64ProcessorArchitecture)
    {
        this->write<uint32_t>(0x1c, PROCESSOR_ARCHITECTURE_ARM64);
        constexpr std::string_view manifest =
            "<assembly manifestVersion=\"1.0\"><assemblyIdentity name=\"Sogen.Arm64.Test\" version=\"1.0.0.0\" "
            "processorArchitecture=\"arm64\"/></assembly>";
        this->emu->memory.write_memory(this->storage + 0x2000, manifest.data(), manifest.size());
        this->write<uint64_t>(0x60, manifest.size());
        ASSERT_EQ(STATUS_SUCCESS, this->port->handle_request(*this->emu, this->request).status);
        const auto address = emulator_object<uint64_t>{this->emu->memory, this->storage + 0x3000}.read();
        EXPECT_NE(0, address);
        EXPECT_TRUE(this->emu->memory.release_memory(address, 0));
    }

    TEST_F(ActivationContextRequestTest, MissingRequiredDependencyReturnsSxsFailureWithoutAllocating)
    {
        constexpr std::string_view manifest =
            "<assembly manifestVersion=\"1.0\"><dependency><dependentAssembly>"
            "<assemblyIdentity name=\"Sogen.Missing.Assembly\" version=\"1.0.0.0\" processorArchitecture=\"amd64\"/>"
            "</dependentAssembly></dependency></assembly>";
        this->emu->memory.write_memory(this->storage + 0x2000, manifest.data(), manifest.size());
        this->write<uint64_t>(0x60, manifest.size());
        const auto region_count = this->emu->memory.get_reserved_regions().size();
        EXPECT_EQ(STATUS_SXS_CANT_GEN_ACTCTX, this->port->handle_request(*this->emu, this->request).status);
        EXPECT_EQ(region_count, this->emu->memory.get_reserved_regions().size());
        EXPECT_EQ(0, (emulator_object<uint64_t>{this->emu->memory, this->storage + 0x3000}.read()));
    }
}
