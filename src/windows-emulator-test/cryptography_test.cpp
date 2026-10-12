#include "emulation_test_utils.hpp"
#include <io_device.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace sogen::test
{
    namespace
    {
        class CryptographyTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                this->initialize(false);
            }

            void initialize(bool reproducible)
            {
                this->emu.reset();
                emulator_interfaces interfaces{};
                interfaces.ui = std::make_unique<null_ui_backend>();
                interfaces.audio = std::make_unique<null_audio_backend>();
                interfaces.console = std::make_unique<null_console_backend>();
                this->emu = std::make_unique<windows_emulator>(create_x86_64_emulator_from_environment(),
                                                               emulator_settings{.disable_logging = true,
                                                                                 .use_relative_time = reproducible,
                                                                                 .emulation_root = get_emulator_root(),
                                                                                 .load_registry = false},
                                                               emulator_callbacks{}, std::move(interfaces));
                this->storage = this->emu->memory.allocate_memory(0x10000, memory_permission::read_write);
            }

            io_device_context request(uint32_t size)
            {
                io_device_context context{this->emu->memory};
                context.io_control_code = 0x390008;
                context.io_status_block.set_address(this->storage);
                context.output_buffer = this->storage + 0x1000;
                context.output_buffer_length = size;
                return context;
            }

            std::unique_ptr<windows_emulator> emu{};
            uint64_t storage{};
        };
    }

    TEST_F(CryptographyTest, GeneratesFreshRandomBytesAndReportsTransferSize)
    {
        io_device_container device{u"CNG", *this->emu, {}};
        auto context = this->request(8192);
        std::array<uint8_t, 8192> first{};
        std::array<uint8_t, 8192> second{};
        const uint8_t guard = 0xa5;
        this->emu->memory.write_memory(context.output_buffer - 1, &guard, sizeof(guard));
        this->emu->memory.write_memory(context.output_buffer + first.size(), &guard, sizeof(guard));

        ASSERT_EQ(STATUS_SUCCESS, device.execute_ioctl(*this->emu, context));
        EXPECT_EQ(STATUS_SUCCESS, context.io_status_block.read().Status);
        EXPECT_EQ(first.size(), context.io_status_block.read().Information);
        this->emu->memory.read_memory(context.output_buffer, first.data(), first.size());
        EXPECT_TRUE(std::ranges::any_of(first, [](uint8_t value) { return value != 0; }));
        ASSERT_EQ(STATUS_SUCCESS, device.execute_ioctl(*this->emu, context));
        this->emu->memory.read_memory(context.output_buffer, second.data(), second.size());
        EXPECT_NE(first, second);
        EXPECT_EQ(guard, (emulator_object<uint8_t>{this->emu->memory, context.output_buffer - 1}.read()));
        EXPECT_EQ(guard, (emulator_object<uint8_t>{this->emu->memory, context.output_buffer + first.size()}.read()));
    }

    TEST_F(CryptographyTest, NormalModeUsesFreshEntropyAcrossDeviceInstances)
    {
        ASSERT_FALSE(this->emu->uses_relative_time());
        io_device_container first_device{u"CNG", *this->emu, {}};
        io_device_container second_device{u"CNG", *this->emu, {}};
        const auto context = this->request(8192);
        std::array<uint8_t, 8192> first{};
        std::array<uint8_t, 8192> second{};
        ASSERT_EQ(STATUS_SUCCESS, first_device.execute_ioctl(*this->emu, context));
        this->emu->memory.read_memory(context.output_buffer, first.data(), first.size());
        ASSERT_EQ(STATUS_SUCCESS, second_device.execute_ioctl(*this->emu, context));
        this->emu->memory.read_memory(context.output_buffer, second.data(), second.size());
        EXPECT_NE(first, second);
    }

    TEST_F(CryptographyTest, ReproducibleRandomSequenceSurvivesStateRestore)
    {
        this->initialize(true);
        io_device_container device{u"CNG", *this->emu, {}};
        const auto context = this->request(8192);
        std::array<uint8_t, 8192> first{};
        std::array<uint8_t, 8192> second{};
        std::array<uint8_t, 8192> replay{};
        auto* generator = device.get_internal_device();
        utils::buffer_serializer initial{};
        generator->serialize(initial);

        ASSERT_EQ(STATUS_SUCCESS, device.execute_ioctl(*this->emu, context));
        this->emu->memory.read_memory(context.output_buffer, first.data(), first.size());
        utils::buffer_serializer after_first{};
        generator->serialize(after_first);
        ASSERT_EQ(STATUS_SUCCESS, device.execute_ioctl(*this->emu, context));
        this->emu->memory.read_memory(context.output_buffer, second.data(), second.size());
        EXPECT_NE(first, second);

        utils::buffer_deserializer restore_first{initial};
        generator->deserialize(restore_first);
        ASSERT_EQ(STATUS_SUCCESS, device.execute_ioctl(*this->emu, context));
        this->emu->memory.read_memory(context.output_buffer, replay.data(), replay.size());
        EXPECT_EQ(first, replay);

        utils::buffer_deserializer restore_second{after_first};
        generator->deserialize(restore_second);
        ASSERT_EQ(STATUS_SUCCESS, device.execute_ioctl(*this->emu, context));
        this->emu->memory.read_memory(context.output_buffer, replay.data(), replay.size());
        EXPECT_EQ(second, replay);
    }

    TEST_F(CryptographyTest, RejectsUnsupportedControlCodes)
    {
        io_device_container device{u"CNG", *this->emu, {}};
        auto context = this->request(48);
        context.io_control_code = 0xffffffff;
        EXPECT_EQ(STATUS_NOT_SUPPORTED, device.execute_ioctl(*this->emu, context));
        EXPECT_EQ(0, context.io_status_block.read().Information);
    }

    TEST_F(CryptographyTest, AcceptsEmptyOutput)
    {
        io_device_container device{u"CNG", *this->emu, {}};
        auto context = this->request(0);
        context.output_buffer = 0;
        EXPECT_EQ(STATUS_SUCCESS, device.execute_ioctl(*this->emu, context));
        EXPECT_EQ(0, context.io_status_block.read().Information);
    }

    TEST_F(CryptographyTest, RejectsMissingOutputAndUnexpectedInput)
    {
        io_device_container device{u"CNG", *this->emu, {}};
        auto context = this->request(48);
        context.output_buffer = 0;
        EXPECT_EQ(STATUS_INVALID_PARAMETER, device.execute_ioctl(*this->emu, context));
        context = this->request(48);
        context.input_buffer_length = 1;
        EXPECT_EQ(STATUS_INVALID_PARAMETER, device.execute_ioctl(*this->emu, context));
    }

    TEST_F(CryptographyTest, ReturnsAccessViolationForUnmappedOutput)
    {
        io_device_container device{u"CNG", *this->emu, {}};
        auto context = this->request(48);
        context.output_buffer = 0xdead000000;
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, device.execute_ioctl(*this->emu, context));
        EXPECT_EQ(0, context.io_status_block.read().Information);
    }

    TEST_F(CryptographyTest, RejectsOutputAddressOverflow)
    {
        io_device_container device{u"CNG", *this->emu, {}};
        auto context = this->request(48);
        context.output_buffer = std::numeric_limits<uint64_t>::max() - 16;
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, device.execute_ioctl(*this->emu, context));
        EXPECT_EQ(0, context.io_status_block.read().Information);
    }

    TEST_F(CryptographyTest, EnumeratesRandomProviderInsteadOfTlsProviders)
    {
        io_device_container device{u"KsecDD", *this->emu, {}};
        auto context = this->request(384);
        context.io_control_code = 0x390400;
        context.input_buffer = this->storage + 0x100;
        context.input_buffer_length = 48;
        std::array<uint8_t, 48> request{};
        const uint32_t signature = 0x1a2b3c4d;
        std::memcpy(request.data(), &signature, sizeof(signature));
        request[6] = 2;
        request[16] = 6;
        request[20] = 1;
        std::fill_n(request.begin() + 24, 16, uint8_t{0xff});
        request[40] = 1;
        this->emu->memory.write_memory(context.input_buffer, request.data(), request.size());

        ASSERT_EQ(STATUS_SUCCESS, device.execute_ioctl(*this->emu, context));
        EXPECT_EQ(216, context.io_status_block.read().Information);
        EXPECT_EQ(1, (emulator_object<uint32_t>{this->emu->memory, context.output_buffer}.read()));
        EXPECT_EQ(6, (emulator_object<uint32_t>{this->emu->memory, context.output_buffer + 24}.read()));
    }
}
