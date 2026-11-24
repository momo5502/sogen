#include "std_include.hpp"

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

    TEST_F(CryptographyTest, KsecDdAnswersProviderResolutionLikeWindows)
    {
        const auto ksec_request = [](const uint32_t interface_id, const uint32_t mode = 1, const uint32_t flags = 0,
                                     const std::optional<std::u16string_view> context = std::nullopt,
                                     const std::optional<std::u16string_view> function = std::nullopt,
                                     const std::optional<std::u16string_view> provider = std::nullopt) {
            std::vector<uint8_t> bytes(0x30);
            const auto put = [&](const size_t offset, const auto value) { std::memcpy(bytes.data() + offset, &value, sizeof(value)); };
            put(0, uint32_t{0x1a2b3c4d});
            put(4, uint32_t{0x20000});
            put(0x10, interface_id);
            put(0x28, mode);
            put(0x2c, flags);
            const auto append_string = [&](const size_t field, const std::optional<std::u16string_view> text) {
                if (!text)
                {
                    put(field, ~uint64_t{});
                    return;
                }

                put(field, static_cast<uint64_t>(bytes.size()));
                const auto offset = bytes.size();
                bytes.resize(offset + ((2 * text->size() + 9) & ~size_t{7}));
                std::memcpy(bytes.data() + offset, text->data(), text->size() * sizeof(char16_t));
            };
            append_string(0x8, context);
            append_string(0x18, function);
            append_string(0x20, provider);
            return bytes;
        };
        const auto with = [](std::vector<uint8_t> bytes, const size_t offset, const uint64_t value, const size_t size) {
            std::memcpy(bytes.data() + offset, &value, size);
            return bytes;
        };

        struct ksec_row
        {
            std::string evidence;
            std::vector<uint8_t> request;
            uint32_t input_length;
            uint32_t output_length;
            NTSTATUS status;
            std::optional<size_t> information;
            std::vector<uint32_t> output_words;
            bool null_output{};
            std::vector<uint32_t> markers{};
        };

        struct ksec_exact_row
        {
            std::string evidence;
            std::vector<uint8_t> request;
            uint32_t input_length;
            uint32_t output_length;
            std::string reply_hex;
        };

        const auto execute = [&](const std::vector<uint8_t>& request_bytes, const uint32_t input_length, const uint32_t output_length,
                                 const bool null_output) {
            io_device_container device{u"KsecDD", *this->emu, {}};
            auto context = this->request(output_length);
            context.io_control_code = 0x390400;
            context.input_buffer = this->storage + 0x100;
            context.input_buffer_length = input_length;
            if (null_output)
            {
                context.output_buffer = 0;
            }

            std::vector<uint8_t> input(input_length);
            std::memcpy(input.data(), request_bytes.data(), std::min(input.size(), request_bytes.size()));
            this->emu->memory.write_memory(context.input_buffer, input.data(), input.size());
            const auto status = device.execute_ioctl(*this->emu, context);
            return std::tuple{status, context.io_status_block.read().Information, context.output_buffer};
        };

        const auto read_words = [&](const uint64_t address, const std::vector<uint32_t>& expected) {
            for (size_t i = 0; i < expected.size(); ++i)
            {
                EXPECT_EQ(expected[i], (emulator_object<uint32_t>{this->emu->memory, address + (i * sizeof(uint32_t))}.read()));
            }
        };

        const auto read_markers = [&](const uint64_t address, const std::vector<uint32_t>& expected) {
            if (expected.empty())
            {
                return;
            }

            ASSERT_EQ(expected.size(), (emulator_object<uint32_t>{this->emu->memory, address}.read()));
            for (size_t i = 0; i < expected.size(); ++i)
            {
                const auto reference = emulator_object<uint64_t>{this->emu->memory, address + 0x10 + (i * sizeof(uint64_t))}.read();
                EXPECT_EQ(expected[i], (emulator_object<uint32_t>{this->emu->memory, address + reference + sizeof(uint32_t)}.read()));
            }
        };

        const std::vector<ksec_row> rejected{
            {"Q22-empty-function-if0 in=56 out=4096",
             ksec_request(0, 1, 0, std::nullopt, u""),
             56,
             4096,
             STATUS_BUFFER_OVERFLOW,
             4,
             {0xC000000D}},
            {"Q22-empty-function-if0-min in=50 out=4096",
             ksec_request(0, 1, 0, std::nullopt, u""),
             50,
             4096,
             STATUS_BUFFER_OVERFLOW,
             4,
             {0xC000000D}},
            {"Q22-empty-context-if0 in=56 out=4096", ksec_request(0, 1, 0, u""), 56, 4096, STATUS_BUFFER_OVERFLOW, 4, {0xC000000D}},
            {"Q22-empty-provider-if0 in=56 out=4096",
             ksec_request(0, 1, 0, std::nullopt, std::nullopt, u""),
             56,
             4096,
             STATUS_BUFFER_OVERFLOW,
             4,
             {0xC000000D}},
            {"Q3-outlen-resolve-rng in=48 out=7", ksec_request(6), 48, 7, STATUS_BUFFER_TOO_SMALL, std::nullopt, {}, false},
            {"Q3-nullout in=48 out=216", ksec_request(6), 48, 216, STATUS_BUFFER_TOO_SMALL, std::nullopt, {}, true},
            {"Q12-in8-out7 in=8 out=7", ksec_request(6), 8, 7, STATUS_BUFFER_TOO_SMALL, std::nullopt, {}, false},
            {"Q12-in8-out8 in=8 out=8", ksec_request(6), 8, 8, STATUS_BUFFER_OVERFLOW, 4, {0xC000000D}, false},
            {"Q3-outlen-resolve-rng in=48 out=215", ksec_request(6), 48, 215, STATUS_BUFFER_OVERFLOW, 8, {0xC0000023, 0xD8}, false},
            {"Q2-inlen in=7 out=384", ksec_request(6), 7, 384, STATUS_ILLEGAL_FUNCTION, std::nullopt, {}, false},
            {"Q13-inlen in=9 out=384", ksec_request(6), 9, 384, STATUS_BUFFER_OVERFLOW, 4, {0xC00000E5}, false},
            {"Q2-inlen in=47 out=384", ksec_request(6), 47, 384, STATUS_BUFFER_OVERFLOW, 4, {0xC00000E5}, false},
            {"Q8-sig00000000 in=48 out=4096", with(ksec_request(6), 0x0, 0, 4), 48, 4096, STATUS_ILLEGAL_FUNCTION, std::nullopt, {}, false},
            {"Q1-func00000004-body in=48 out=4096",
             with(ksec_request(6), 0x4, 4, 4),
             48,
             4096,
             STATUS_ILLEGAL_FUNCTION,
             std::nullopt,
             {},
             false},
            {"Q1-func00020001-body in=48 out=4096",
             with(ksec_request(6), 0x4, 0x20001, 4),
             48,
             4096,
             STATUS_ILLEGAL_FUNCTION,
             std::nullopt,
             {},
             false},
            {"Q1-func00030000-body in=48 out=4096",
             with(ksec_request(6), 0x4, 0x30000, 4),
             48,
             4096,
             STATUS_ILLEGAL_FUNCTION,
             std::nullopt,
             {},
             false},
            {"Q8-ctxoff0 in=48 out=4096", with(ksec_request(6), 0x8, 0, 8), 48, 4096, STATUS_BUFFER_OVERFLOW, 4, {0xC00000E5}, false},
            {"Q8-ctxoff30 in=48 out=4096", with(ksec_request(6), 0x8, 0x30, 8), 48, 4096, STATUS_BUFFER_OVERFLOW, 4, {0xC00000E5}, false},
            {"Q13-fnoff31 in=56 out=4096",
             with(ksec_request(0, 1, 0, std::nullopt, u"RNG"), 0x18, 0x31, 8),
             56,
             4096,
             STATUS_BUFFER_OVERFLOW,
             4,
             {0xC0000225},
             false},
            {"Q13-fnoff38 in=56 out=4096",
             with(ksec_request(0, 1, 0, std::nullopt, u"RNG"), 0x18, 0x38, 8),
             56,
             4096,
             STATUS_BUFFER_OVERFLOW,
             4,
             {0xC00000E5},
             false},
            {"Q13-name-unterminated-inlen-minus2 in=54 out=4096",
             ksec_request(0, 1, 0, std::nullopt, u"RNG"),
             54,
             4096,
             STATUS_BUFFER_OVERFLOW,
             4,
             {0xC00000E5},
             false},
            {"Q13-order-badoffset-badmode in=48 out=4096",
             with(ksec_request(6, 0), 0x8, 0, 8),
             48,
             4096,
             STATUS_BUFFER_OVERFLOW,
             4,
             {0xC00000E5},
             false},
            {"Q8-mode0 in=48 out=4096", ksec_request(6, 0), 48, 4096, STATUS_BUFFER_OVERFLOW, 4, {0xC000000D}, false},
            {"Q8-mode4 in=48 out=4096", ksec_request(6, 4), 48, 4096, STATUS_BUFFER_OVERFLOW, 4, {0xC000000D}, false},
            {"Q8-flags4 in=48 out=4096", ksec_request(6, 1, 4), 48, 4096, STATUS_BUFFER_OVERFLOW, 4, {0xC000000D}, false},
            {"Q8-flags80000000 in=48 out=4096", ksec_request(6, 1, 0x80000000), 48, 4096, STATUS_BUFFER_OVERFLOW, 4, {0xC000000D}, false},
            {"Q7-if0 in=48 out=4096", ksec_request(0), 48, 4096, STATUS_BUFFER_OVERFLOW, 4, {0xC000000D}, false},
            {"Q7-if9 in=48 out=4096", ksec_request(9), 48, 4096, STATUS_BUFFER_OVERFLOW, 4, {0xC000000D}, false},
            {"Q14-if10000 in=48 out=4096", ksec_request(0x10000), 48, 4096, STATUS_BUFFER_OVERFLOW, 4, {0xC000000D}, false},
            {"Q14-if10003 in=48 out=4096", ksec_request(0x10003), 48, 4096, STATUS_BUFFER_OVERFLOW, 4, {0xC0000225}, false},
            {"Q14-if20000 in=48 out=4096", ksec_request(0x20000), 48, 4096, STATUS_BUFFER_OVERFLOW, 4, {0xC000000D}, false},
            {"Q15-alg46 in=64 out=4096",
             ksec_request(0, 1, 0, std::nullopt, u"EDDSA"),
             64,
             4096,
             STATUS_BUFFER_OVERFLOW,
             4,
             {0xC0000225},
             false},
            {"Q15-alg47 in=72 out=4096",
             ksec_request(0, 1, 0, std::nullopt, u"ECDSA_P256 "),
             72,
             4096,
             STATUS_BUFFER_OVERFLOW,
             4,
             {0xC0000225},
             false},
            {"Q15-alg51 in=48 out=4096", ksec_request(0), 48, 4096, STATUS_BUFFER_OVERFLOW, 4, {0xC000000D}, false},
            {"Q21-casefold-dotless-i in=64 out=4096",
             ksec_request(0, 1, 0, std::nullopt, u"s\u0131ha256"),
             64,
             4096,
             STATUS_BUFFER_OVERFLOW,
             4,
             {0xC0000225},
             false},
            {"Q21-casefold-long-s in=64 out=4096",
             ksec_request(0, 1, 0, std::nullopt, u"\u017Fha256"),
             64,
             4096,
             STATUS_BUFFER_OVERFLOW,
             4,
             {0xC0000225},
             false},
            {"Q6-prov0-function-slot in=112 out=4096",
             ksec_request(0, 1, 0, std::nullopt, u"Microsoft Primitive Provider"),
             112,
             4096,
             STATUS_BUFFER_OVERFLOW,
             4,
             {0xC0000225},
             false},
            {"Q6-prov1-RNG-plus-provider in=80 out=4096",
             ksec_request(0, 1, 0, std::nullopt, u"RNG", u"KEY_STORAGE"),
             80,
             4096,
             STATUS_BUFFER_OVERFLOW,
             4,
             {0xC0000225},
             false},
            {"Q6-prov0-provider-slot in=112 out=4096",
             ksec_request(0, 1, 0, std::nullopt, std::nullopt, u"Microsoft Primitive Provider"),
             112,
             4096,
             STATUS_BUFFER_OVERFLOW,
             4,
             {0xC000000D},
             false},
            {"Q6-prov2-provider-slot-if6 in=112 out=4096",
             ksec_request(6, 1, 0, std::nullopt, std::nullopt, u"Microsoft SSL Protocol Provider"),
             112,
             4096,
             STATUS_BUFFER_OVERFLOW,
             4,
             {0xC0000225},
             false},
            {"Q16-ctx0-if1 in=56 out=4096", ksec_request(1, 1, 0, u"SSL"), 56, 4096, STATUS_BUFFER_OVERFLOW, 4, {0xC0000225}, false},
            {"Q16-ctx0-if0 in=56 out=4096", ksec_request(0, 1, 0, u"SSL"), 56, 4096, STATUS_BUFFER_OVERFLOW, 4, {0xC000000D}, false},
            {"Q16-ctx1-if10003 in=64 out=4096",
             ksec_request(0x10003, 1, 0, u"Default"),
             64,
             4096,
             STATUS_BUFFER_OVERFLOW,
             4,
             {0xC0000225},
             false},
            {"Q16-ctx2-if10002 in=80 out=4096",
             ksec_request(0x10002, 1, 0, u"Cryptography"),
             80,
             4096,
             STATUS_BUFFER_OVERFLOW,
             4,
             {0xC0000225},
             false},
            {"Q16-ctxprov1-if1 in=128 out=4096",
             ksec_request(1, 1, 0, u"Default", std::nullopt, u"Microsoft SSL Protocol Provider"),
             128,
             4096,
             STATUS_BUFFER_OVERFLOW,
             4,
             {0xC0000225},
             false},
            {"Q20-if10001-mode2-flags0 in=48 out=8192", ksec_request(0x10001, 2), 48, 8192, STATUS_BUFFER_OVERFLOW, 4, {0xC0000225}, false},
            {"Q20-if10001-mode3-flags1 in=48 out=8192",
             ksec_request(0x10001, 3, 1),
             48,
             8192,
             STATUS_BUFFER_OVERFLOW,
             4,
             {0xC0000225},
             false},
            {"Q20-if10004-mode2-flags0 in=48 out=8192", ksec_request(0x10004, 2), 48, 8192, STATUS_BUFFER_OVERFLOW, 4, {0xC0000225}, false},
            {"Q21-fn2-mode2 in=56 out=4096",
             ksec_request(0, 2, 0, std::nullopt, u"SID"),
             56,
             4096,
             STATUS_BUFFER_OVERFLOW,
             4,
             {0xC0000225},
             false},
            {"Q21-fn3-mode3 in=72 out=4096",
             ksec_request(0, 3, 0, std::nullopt, u"KEY_STORAGE"),
             72,
             4096,
             STATUS_BUFFER_OVERFLOW,
             4,
             {0xC0000225},
             false},
            {"Q21-fn8-mode2 in=56 out=4096",
             ksec_request(0, 2, 0, std::nullopt, u"DSA"),
             56,
             4096,
             STATUS_BUFFER_OVERFLOW,
             4,
             {0xC0000225},
             false},
            {"Q21-fn-sha256-if1 in=64 out=4096",
             ksec_request(1, 1, 0, std::nullopt, u"SHA256"),
             64,
             4096,
             STATUS_BUFFER_OVERFLOW,
             4,
             {0xC0000225},
             false},
            {"Q21-fn-sha256-if10002 in=64 out=4096",
             ksec_request(0x10002, 1, 0, std::nullopt, u"SHA256"),
             64,
             4096,
             STATUS_BUFFER_OVERFLOW,
             4,
             {0xC0000225},
             false},
            {"Q21-fn-sha256-ifffff in=64 out=4096",
             ksec_request(0xFFFF, 1, 0, std::nullopt, u"SHA256"),
             64,
             4096,
             STATUS_BUFFER_OVERFLOW,
             4,
             {0xC000000D},
             false},
            {"Q21-fn-sha256-ctx1 in=72 out=4096",
             ksec_request(0, 1, 0, u"SSL", u"SHA256"),
             72,
             4096,
             STATUS_BUFFER_OVERFLOW,
             4,
             {0xC0000225},
             false},
            {"Q21-fn-tls-ctx2 in=112 out=4096",
             ksec_request(0, 1, 0, u"Bogus", u"TLS_AES_256_GCM_SHA384"),
             112,
             4096,
             STATUS_BUFFER_OVERFLOW,
             4,
             {0xC0000225},
             false},
        };

        const std::vector<ksec_row> sized{
            {"Q22-empty-m4-if6-flags0 in=56 out=4096",
             ksec_request(6, 1, 0, std::nullopt, std::nullopt, u""),
             56,
             4096,
             STATUS_SUCCESS,
             216,
             {0x1},
             false,
             {0xFFFFFFFF}},
            {"Q22-empty-m4-if6-flags0-min in=50 out=4096",
             ksec_request(6, 1, 0, std::nullopt, std::nullopt, u""),
             50,
             4096,
             STATUS_SUCCESS,
             216,
             {0x1},
             false,
             {0xFFFFFFFF}},
            {"Q22-empty-m1-if6-flags0 in=56 out=4096",
             ksec_request(6, 1, 0, u""),
             56,
             4096,
             STATUS_SUCCESS,
             216,
             {0x1},
             false,
             {0xFFFFFFFF}},
            {"Q22-empty-m7-if6-flags0 in=72 out=4096",
             ksec_request(6, 1, 0, u"", u"", u""),
             72,
             4096,
             STATUS_SUCCESS,
             216,
             {0x1},
             false,
             {0x0}},
            {"Q22-empty-m2-if6-flags1 in=56 out=4096",
             ksec_request(6, 1, 1, std::nullopt, u""),
             56,
             4096,
             STATUS_SUCCESS,
             656,
             {0x3},
             false,
             {0x1, 0x0, 0x0}},
            {"Q22-empty-m5-rng in=72 out=4096",
             ksec_request(0, 1, 0, u"", u"RNG", u""),
             72,
             4096,
             STATUS_SUCCESS,
             216,
             {0x1},
             false,
             {0x0}},
            {"Q22-empty-provider-if10002 in=56 out=4096",
             ksec_request(0x10002, 1, 0, std::nullopt, std::nullopt, u""),
             56,
             4096,
             STATUS_SUCCESS,
             240,
             {0x1},
             false,
             {0xFFFFFFFF}},
            {"Q22-empty-context-if10002-flags1 in=56 out=16384", ksec_request(0x10002, 1, 1, u""), 56, 16384, STATUS_SUCCESS, 11704, {}},
            {"Q2-inlen in=4096 out=384", ksec_request(6), 4096, 384, STATUS_SUCCESS, 216, {0x1}, false, {0xFFFFFFFF}},
            {"Q6-alg0-open in=64 out=4096",
             ksec_request(0, 1, 0, std::nullopt, u"SHA1"),
             64,
             4096,
             STATUS_SUCCESS,
             224,
             {0x1},
             false,
             {0x0}},
            {"Q7-if5 in=48 out=8", ksec_request(5), 48, 8, STATUS_BUFFER_OVERFLOW, 8, {0xC0000023, 0x128}, false, {}},
            {"Q7-if7 in=48 out=4096", ksec_request(7), 48, 4096, STATUS_SUCCESS, 248, {0x1}, false, {0xFFFFFFFF}},
            {"Q6-prov0-RNG-plus-provider in=120 out=4096",
             ksec_request(0, 1, 0, std::nullopt, u"RNG", u"Microsoft Primitive Provider"),
             120,
             4096,
             STATUS_SUCCESS,
             216,
             {0x1},
             false,
             {0x0}},
            {"Q19-if1-mode1-flags1 in=48 out=8192",
             ksec_request(1, 1, 1),
             48,
             8192,
             STATUS_SUCCESS,
             2336,
             {0x9},
             false,
             std::vector<uint32_t>(9, 0x0)},
            {"Q19-if1-mode2-flags1 in=48 out=8192",
             ksec_request(1, 2, 1),
             48,
             8192,
             STATUS_SUCCESS,
             2048,
             {0x9},
             false,
             std::vector<uint32_t>(9, 0x0)},
            {"Q19-if1-mode3-flags1 in=48 out=8192",
             ksec_request(1, 3, 1),
             48,
             8192,
             STATUS_SUCCESS,
             2624,
             {0x9},
             false,
             std::vector<uint32_t>(9, 0x0)},
            {"Q19-if2-mode1-flags1 in=48 out=8192",
             ksec_request(2, 1, 1),
             48,
             8192,
             STATUS_SUCCESS,
             3808,
             {0x12},
             false,
             std::vector<uint32_t>(18, 0x0)},
            {"Q19-if2-mode2-flags1 in=48 out=8192",
             ksec_request(2, 2, 1),
             48,
             8192,
             STATUS_SUCCESS,
             3232,
             {0x12},
             false,
             std::vector<uint32_t>(18, 0x0)},
            {"Q19-if2-mode3-flags1 in=48 out=8192",
             ksec_request(2, 3, 1),
             48,
             8192,
             STATUS_SUCCESS,
             4384,
             {0x12},
             false,
             std::vector<uint32_t>(18, 0x0)},
            {"Q17-if5-mode3-flags1 in=48 out=4096",
             ksec_request(5, 3, 1),
             48,
             4096,
             STATUS_SUCCESS,
             2008,
             {0x7},
             false,
             std::vector<uint32_t>(7, 0x0)},
            {"Q17-if4-mode2-flags1 in=48 out=4096",
             ksec_request(4, 2, 1),
             48,
             4096,
             STATUS_SUCCESS,
             1168,
             {0x5},
             false,
             std::vector<uint32_t>(5, 0x0)},
            {"Q8-flags3 in=48 out=4096", ksec_request(6, 1, 3), 48, 4096, STATUS_SUCCESS, 656, {0x3}, false, {0x3, 0x0, 0x0}},
            {"Q20-if10001-mode1-flags1 in=48 out=8192",
             ksec_request(0x10001, 1, 1),
             48,
             8192,
             STATUS_SUCCESS,
             232,
             {0x1},
             false,
             {0xFFFFFFFF}},
            {"Q20-if10002-mode2-flags0 in=48 out=8192",
             ksec_request(0x10002, 2),
             48,
             8192,
             STATUS_SUCCESS,
             224,
             {0x1},
             false,
             {0xFFFFFFFF}},
            {"Q20-if10002-mode1-flags1 in=48 out=8192",
             ksec_request(0x10002, 1, 1),
             48,
             8192,
             STATUS_BUFFER_OVERFLOW,
             8,
             {0xC0000023, 0x2DB8},
             false,
             {}},
            {"Q20-if10002-mode2-flags1 in=48 out=8192",
             ksec_request(0x10002, 2, 1),
             48,
             8192,
             STATUS_BUFFER_OVERFLOW,
             8,
             {0xC0000023, 0x2AB8},
             false,
             {}},
            {"Q20-if10002-mode3-flags1 in=48 out=8192",
             ksec_request(0x10002, 3, 1),
             48,
             8192,
             STATUS_BUFFER_OVERFLOW,
             8,
             {0xC0000023, 0x33B8},
             false,
             {}},
            {"Q20-if10004-mode1-flags0 in=48 out=8192", ksec_request(0x10004), 48, 8192, STATUS_SUCCESS, 216, {0x1}, false, {0xFFFFFFFF}},
            {"Q20-if10004-mode1-flags1 in=48 out=8192",
             ksec_request(0x10004, 1, 1),
             48,
             8192,
             STATUS_SUCCESS,
             2192,
             {0xA},
             false,
             std::vector<uint32_t>(10, 0x0)},
            {"Q21-fn0-mode1 in=96 out=4096",
             ksec_request(0, 1, 0, std::nullopt, u"TLS_AES_256_GCM_SHA384"),
             96,
             4096,
             STATUS_SUCCESS,
             240,
             {0x1},
             false,
             {0x0}},
            {"Q21-fn0-mode2 in=96 out=4096",
             ksec_request(0, 2, 0, std::nullopt, u"TLS_AES_256_GCM_SHA384"),
             96,
             4096,
             STATUS_SUCCESS,
             224,
             {0x1},
             false,
             {0x0}},
            {"Q21-fn0-mode3 in=96 out=4096",
             ksec_request(0, 3, 0, std::nullopt, u"TLS_AES_256_GCM_SHA384"),
             96,
             4096,
             STATUS_SUCCESS,
             272,
             {0x1},
             false,
             {0x0}},
            {"Q21-fn1-mode1 in=96 out=4096",
             ksec_request(0, 1, 0, std::nullopt, u"TLS_RSA_WITH_NULL_MD5"),
             96,
             4096,
             STATUS_SUCCESS,
             240,
             {0x1},
             false,
             {0x0}},
            {"Q21-fn2-mode1 in=56 out=4096",
             ksec_request(0, 1, 0, std::nullopt, u"SID"),
             56,
             4096,
             STATUS_SUCCESS,
             216,
             {0x1},
             false,
             {0x0}},
            {"Q21-fn4-mode3 in=88 out=4096",
             ksec_request(0, 3, 0, std::nullopt, u"Composite-ML-DSA"),
             88,
             4096,
             STATUS_SUCCESS,
             280,
             {0x1},
             false,
             {0x0}},
            {"Q21-fn5-mode2 in=88 out=4096",
             ksec_request(0, 2, 0, std::nullopt, u"Composite-ML-KEM"),
             88,
             4096,
             STATUS_SUCCESS,
             216,
             {0x1},
             false,
             {0x0}},
            {"Q21-fn6-mode2 in=72 out=4096",
             ksec_request(0, 2, 0, std::nullopt, u"CSHAKE256"),
             72,
             4096,
             STATUS_SUCCESS,
             200,
             {0x1},
             false,
             {0x0}},
            {"Q21-fn7-mode3 in=64 out=4096",
             ksec_request(0, 3, 0, std::nullopt, u"KMAC256"),
             64,
             4096,
             STATUS_SUCCESS,
             256,
             {0x1},
             false,
             {0x0}},
            {"Q21-fn9-mode3 in=64 out=4096",
             ksec_request(0, 3, 0, std::nullopt, u"ML-DSA"),
             64,
             4096,
             STATUS_SUCCESS,
             256,
             {0x1},
             false,
             {0x0}},
            {"Q21-fn-sha256-if2 in=64 out=4096",
             ksec_request(2, 1, 0, std::nullopt, u"SHA256"),
             64,
             4096,
             STATUS_SUCCESS,
             224,
             {0x1},
             false,
             {0x0}},
            {"Q21-fn-sha256-ctx0 in=80 out=4096",
             ksec_request(0, 1, 0, u"Default", u"SHA256"),
             80,
             4096,
             STATUS_SUCCESS,
             224,
             {0x1},
             false,
             {0x0}},
            {"Q21-fn-tls-ctx1 in=104 out=4096",
             ksec_request(0, 1, 0, u"SSL", u"TLS_AES_256_GCM_SHA384"),
             104,
             4096,
             STATUS_SUCCESS,
             240,
             {0x1},
             false,
             {0x0}},
            {"Q21-ctxdefault-if1 in=64 out=4096",
             ksec_request(1, 1, 0, u"Default"),
             64,
             4096,
             STATUS_SUCCESS,
             280,
             {0x1},
             false,
             {0xFFFFFFFF}},
            {"Q21-ctxdefault-if2 in=64 out=4096",
             ksec_request(2, 1, 0, u"Default"),
             64,
             4096,
             STATUS_SUCCESS,
             224,
             {0x1},
             false,
             {0xFFFFFFFF}},
            {"Q21-ctxdefault-if3 in=64 out=4096",
             ksec_request(3, 1, 0, u"Default"),
             64,
             4096,
             STATUS_SUCCESS,
             280,
             {0x1},
             false,
             {0xFFFFFFFF}},
            {"Q21-ctxdefault-if4 in=64 out=4096",
             ksec_request(4, 1, 0, u"Default"),
             64,
             4096,
             STATUS_SUCCESS,
             280,
             {0x1},
             false,
             {0xFFFFFFFF}},
            {"Q21-ctxdefault-if5 in=64 out=4096",
             ksec_request(5, 1, 0, u"Default"),
             64,
             4096,
             STATUS_SUCCESS,
             296,
             {0x1},
             false,
             {0xFFFFFFFF}},
            {"Q21-ctxdefault-if6 in=64 out=4096",
             ksec_request(6, 1, 0, u"Default"),
             64,
             4096,
             STATUS_SUCCESS,
             216,
             {0x1},
             false,
             {0xFFFFFFFF}},
            {"Q21-ctxdefault-if7 in=64 out=4096",
             ksec_request(7, 1, 0, u"Default"),
             64,
             4096,
             STATUS_SUCCESS,
             248,
             {0x1},
             false,
             {0xFFFFFFFF}},
            {"Q21-ctxdefault-if8 in=64 out=4096",
             ksec_request(8, 1, 0, u"Default"),
             64,
             4096,
             STATUS_SUCCESS,
             224,
             {0x1},
             false,
             {0xFFFFFFFF}},
            {"Q21-prov-flags1-if6 in=112 out=4096",
             ksec_request(6, 1, 1, std::nullopt, std::nullopt, u"Microsoft Primitive Provider"),
             112,
             4096,
             STATUS_SUCCESS,
             208,
             {0x1},
             false,
             {0xFFFFFFFF}},
            {"Q17-name1-mode3-flags3 in=56 out=4096",
             ksec_request(0, 3, 3, std::nullopt, u"AES"),
             56,
             4096,
             STATUS_SUCCESS,
             312,
             {0x1},
             false,
             {0x0}},
        };

        const std::vector<ksec_exact_row> exact{
            {"Q22-empty-m2-if6-flags0 in=56 out=4096", ksec_request(6, 1, 0, std::nullopt, u""), 56, 4096,
             "010000000000020010000000000000001800000000000000060000000000000050000000000000005800000000000000"
             "0000000000000000ffffffffffffffff9800000000000000ffffffffffffffff52004e00470000004d00690063007200"
             "6f0073006f006600740020005000720069006d00690074006900760065002000500072006f0076006900640065007200"
             "0000000000000000a80000000000000001000000000000006200630072007900700074007000720069006d0069007400"
             "69007600650073002e0064006c006c000000000000000000"},
            {"B1-resolve-rng-if6 in=48 out=384", ksec_request(6), 48, 384,
             "01000000000002001000000000000000180000000000000006000000ffffffff50000000000000005800000000000000"
             "0000000000000000ffffffffffffffff9800000000000000ffffffffffffffff52004e00470000004d00690063007200"
             "6f0073006f006600740020005000720069006d00690074006900760065002000500072006f0076006900640065007200"
             "0000000000000000a80000000000000001000000000000006200630072007900700074007000720069006d0069007400"
             "69007600650073002e0064006c006c000000000000000000"},
            {"B2-open-RNG in=56 out=384", ksec_request(0, 1, 0, std::nullopt, u"RNG"), 56, 384,
             "010000000000020010000000000000001800000000000000060000000000000050000000000000005800000000000000"
             "0000000047000000ffffffffffffffff9800000000000000ffffffffffffffff52004e00470000004d00690063007200"
             "6f0073006f006600740020005000720069006d00690074006900760065002000500072006f0076006900640065007200"
             "0000000000000000a80000000000000001000000000000006200630072007900700074007000720069006d0069007400"
             "69007600650073002e0064006c006c000000000000000000"},
            {"Q15-alg0 in=64 out=4096", ksec_request(0, 1, 0, std::nullopt, u"3DES"), 64, 4096,
             "010000000000020010000000000000001800000000000000010000000000000050000000000000006000000000000000"
             "0100000045005300a000000000000000e000000000000000ffffffffffffffff33004400450053000000000000000000"
             "4d006900630072006f0073006f006600740020005000720069006d00690074006900760065002000500072006f007600"
             "69006400650072000000000000000000a800000000000000c0000000000000000400000000000000d800000000000000"
             "4b00650079004c0065006e006700740068000000000000008000000000000000f0000000000000000100000000000000"
             "6200630072007900700074007000720069006d006900740069007600650073002e0064006c006c000000000000000000"},
            {"Q15-alg48 in=64 out=4096", ksec_request(0, 1, 0, std::nullopt, u"sha256"), 64, 4096,
             "010000000000020010000000000000001800000000000000020000000000000050000000000000006000000000000000"
             "0000000061003200ffffffffffffffffa000000000000000ffffffffffffffff53004800410032003500360000000000"
             "4d006900630072006f0073006f006600740020005000720069006d00690074006900760065002000500072006f007600"
             "69006400650072000000000000000000b000000000000000010000000000000062006300720079007000740070007200"
             "69006d006900740069007600650073002e0064006c006c000000000000000000"},
            {"Q6-prov1-function-slot in=72 out=4096", ksec_request(0, 1, 0, std::nullopt, u"KEY_STORAGE"), 72, 4096,
             "010000000000020010000000000000001800000000000000010001000000000050000000000000006800000000000000"
             "0000000059005f00ffffffffffffffffb800000000000000ffffffffffffffff4b00450059005f00530054004f005200"
             "41004700450000004d006900630072006f0073006f0066007400200053006f0066007400770061007200650020004b00"
             "650079002000530074006f0072006100670065002000500072006f00760069006400650072000000c800000000000000"
             "00000100000000006e0063007200790070007400700072006f0076002e0064006c006c0000000000"},
            {"Q6-prov0-provider-slot-if6 in=112 out=4096",
             ksec_request(6, 1, 0, std::nullopt, std::nullopt, u"Microsoft Primitive Provider"), 112, 4096,
             "01000000000002001000000000000000180000000000000006000000ffffffffffffffffffffffff5000000000000000"
             "0000000063007200ffffffffffffffff9000000000000000ffffffffffffffff4d006900630072006f0073006f006600"
             "740020005000720069006d00690074006900760065002000500072006f00760069006400650072000000000000000000"
             "a00000000000000001000000000000006200630072007900700074007000720069006d00690074006900760065007300"
             "2e0064006c006c000000000000000000"},
            {"Q7-if1 in=48 out=4096", ksec_request(1), 48, 4096,
             "01000000000002001000000000000000180000000000000001000000ffffffff50000000000000005800000000000000"
             "01000000000000009800000000000000d800000000000000ffffffffffffffff41004500530000004d00690063007200"
             "6f0073006f006600740020005000720069006d00690074006900760065002000500072006f0076006900640065007200"
             "0000000000000000a000000000000000b8000000000000000400000000000000d0000000000000004b00650079004c00"
             "65006e006700740068000000000000008000000000000000e80000000000000001000000000000006200630072007900"
             "700074007000720069006d006900740069007600650073002e0064006c006c000000000000000000"},
            {"Q14-if10002 in=48 out=4096", ksec_request(0x10002), 48, 4096,
             "01000000000002001000000000000000180000000000000002000100ffffffff50000000000000008000000000000000"
             "0000000000000000ffffffffffffffffc000000000000000ffffffffffffffff54004c0053005f004100450053005f00"
             "3200350036005f00470043004d005f00530048004100330038003400000000004d006900630072006f0073006f006600"
             "74002000530053004c002000500072006f0074006f0063006f006c002000500072006f00760069006400650072000000"
             "d00000000000000000000000000000006e0063007200790070007400730073006c0070002e0064006c006c0000000000"},
            {"Q14-if10004 in=48 out=4096", ksec_request(0x10004), 48, 4096,
             "01000000000002001000000000000000180000000000000004000100ffffffff50000000000000005800000000000000"
             "0000000000000000ffffffffffffffffa000000000000000ffffffffffffffff53004900440000004d00690063007200"
             "6f0073006f006600740020004b00650079002000500072006f00740065006300740069006f006e002000500072006f00"
             "76006900640065007200000000000000b00000000000000000000100000000006d0073006b0065007900700072006f00"
             "74006500630074002e0064006c006c000000000000000000"},
            {"Q16-ctxprov0-if10002 in=120 out=4096", ksec_request(0x10002, 1, 0, u"SSL", std::nullopt, u"Microsoft SSL Protocol Provider"),
             120, 4096,
             "01000000000002001000000000000000180000000000000002000100ffffffffffffffffffffffff5000000000000000"
             "000000004c000000ffffffffffffffff9000000000000000ffffffffffffffff4d006900630072006f0073006f006600"
             "74002000530053004c002000500072006f0074006f0063006f006c002000500072006f00760069006400650072000000"
             "a00000000000000000000000000000006e0063007200790070007400730073006c0070002e0064006c006c0000000000"},
            {"Q17-if6-mode3-flags0 in=48 out=4096", ksec_request(6, 3), 48, 4096,
             "01000000000002001000000000000000180000000000000006000000ffffffff50000000000000005800000000000000"
             "0000000000000000ffffffffffffffff9800000000000000d80000000000000052004e00470000004d00690063007200"
             "6f0073006f006600740020005000720069006d00690074006900760065002000500072006f0076006900640065007200"
             "0000000000000000a80000000000000001000000000000006200630072007900700074007000720069006d0069007400"
             "69007600650073002e0064006c006c000000000000000000e800000000000000010000000000000063006e0067002e00"
             "7300790073000000"},
            {"Q17-if6-mode1-flags1 in=48 out=4096", ksec_request(6, 1, 1), 48, 4096,
             "030000000000020010000000000000002800000000000000e800000000000000c0010000000000000600000001000000"
             "600000000000000068000000000000000000000000000000ffffffffffffffffa800000000000000ffffffffffffffff"
             "52004e00470000004d006900630072006f0073006f006600740020005000720069006d00690074006900760065002000"
             "500072006f00760069006400650072000000000000000000b80000000000000001000000000000006200630072007900"
             "700074007000720069006d006900740069007600650073002e0064006c006c0000000000000000000600000000000000"
             "200100000000000040010000000000000000000000000000ffffffffffffffff8001000000000000ffffffffffffffff"
             "460049005000530031003800360044005300410052004e0047000000000000004d006900630072006f0073006f006600"
             "740020005000720069006d00690074006900760065002000500072006f00760069006400650072000000000000000000"
             "900100000000000001000000000000006200630072007900700074007000720069006d00690074006900760065007300"
             "2e0064006c006c0000000000000000000600000000000000f80100000000000010020000000000000000000000000000"
             "ffffffffffffffff5002000000000000ffffffffffffffff4400550041004c004500430052004e004700000000000000"
             "4d006900630072006f0073006f006600740020005000720069006d00690074006900760065002000500072006f007600"
             "690064006500720000000000000000006002000000000000010000000000000062006300720079007000740070007200"
             "69006d006900740069007600650073002e0064006c006c000000000000000000"},
            {"Q17-if4-mode1-flags1 in=48 out=4096", ksec_request(4, 1, 1), 48, 4096,
             "050000000000020010000000000000003800000000000000380100000000000048020000000000005803000000000000"
             "68040000000000000400000000000000700000000000000078000000000000000100000000000000b800000000000000"
             "f800000000000000ffffffffffffffff44004800000000004d006900630072006f0073006f0066007400200050007200"
             "69006d00690074006900760065002000500072006f00760069006400650072000000000000000000c000000000000000"
             "d8000000000000000400000000000000f0000000000000004b00650079004c0065006e00670074006800000000000000"
             "0004000000000000080100000000000001000000000000006200630072007900700074007000720069006d0069007400"
             "69007600650073002e0064006c006c000000000000000000040000000000000070010000000000008801000000000000"
             "0100000000000000c8010000000000000802000000000000ffffffffffffffff45004300440048005f00500032003500"
             "36000000000000004d006900630072006f0073006f006600740020005000720069006d00690074006900760065002000"
             "500072006f00760069006400650072000000000000000000d001000000000000e8010000000000000400000000000000"
             "00020000000000004b00650079004c0065006e0067007400680000000000000000010000000000001802000000000000"
             "01000000000000006200630072007900700074007000720069006d006900740069007600650073002e0064006c006c00"
             "00000000000000000400000000000000800200000000000098020000000000000100000000000000d802000000000000"
             "1803000000000000ffffffffffffffff45004300440048005f0050003300380034000000000000004d00690063007200"
             "6f0073006f006600740020005000720069006d00690074006900760065002000500072006f0076006900640065007200"
             "0000000000000000e002000000000000f802000000000000040000000000000010030000000000004b00650079004c00"
             "65006e006700740068000000000000008001000000000000280300000000000001000000000000006200630072007900"
             "700074007000720069006d006900740069007600650073002e0064006c006c0000000000000000000400000000000000"
             "9003000000000000a8030000000000000100000000000000e8030000000000002804000000000000ffffffffffffffff"
             "45004300440048005f0050003500320031000000000000004d006900630072006f0073006f0066007400200050007200"
             "69006d00690074006900760065002000500072006f00760069006400650072000000000000000000f003000000000000"
             "0804000000000000040000000000000020040000000000004b00650079004c0065006e00670074006800000000000000"
             "0902000000000000380400000000000001000000000000006200630072007900700074007000720069006d0069007400"
             "69007600650073002e0064006c006c0000000000000000000400000000000000a004000000000000b004000000000000"
             "0000000000000000fffffffffffffffff004000000000000ffffffffffffffff45004300440048000000000000000000"
             "4d006900630072006f0073006f006600740020005000720069006d00690074006900760065002000500072006f007600"
             "690064006500720000000000000000000005000000000000010000000000000062006300720079007000740070007200"
             "69006d006900740069007600650073002e0064006c006c000000000000000000"},
            {"Q17-name2-mode2-flags0 in=56 out=4096", ksec_request(0, 2, 0, std::nullopt, u"RNG"), 56, 4096,
             "010000000000020010000000000000001800000000000000060000000000000050000000000000005800000000000000"
             "0000000047000000ffffffffffffffffffffffffffffffff980000000000000052004e00470000004d00690063007200"
             "6f0073006f006600740020005000720069006d00690074006900760065002000500072006f0076006900640065007200"
             "0000000000000000a800000000000000010000000000000063006e0067002e007300790073000000"},
            {"Q20-if10002-mode3-flags0 in=48 out=8192", ksec_request(0x10002, 3), 48, 8192,
             "01000000000002001000000000000000180000000000000002000100ffffffff50000000000000008000000000000000"
             "0000000000000000ffffffffffffffffc000000000000000f00000000000000054004c0053005f004100450053005f00"
             "3200350036005f00470043004d005f00530048004100330038003400000000004d006900630072006f0073006f006600"
             "74002000530053004c002000500072006f0074006f0063006f006c002000500072006f00760069006400650072000000"
             "d00000000000000000000000000000006e0063007200790070007400730073006c0070002e0064006c006c0000000000"
             "0001000000000000000000000000000063006e0067002e007300790073000000"},
        };

        for (const auto& row : rejected)
        {
            SCOPED_TRACE(row.evidence);
            const auto [status, information, output] = execute(row.request, row.input_length, row.output_length, row.null_output);
            ASSERT_EQ(row.status, status);
            if (row.information)
            {
                EXPECT_EQ(*row.information, information);
            }

            read_words(output, row.output_words);
        }

        for (const auto& row : sized)
        {
            SCOPED_TRACE(row.evidence);
            const auto [status, information, output] = execute(row.request, row.input_length, row.output_length, false);
            ASSERT_EQ(row.status, status);
            EXPECT_EQ(*row.information, information);
            read_words(output, row.output_words);
            read_markers(output, row.markers);
        }

        for (const auto& row : exact)
        {
            SCOPED_TRACE(row.evidence);
            const auto [status, information, output] = execute(row.request, row.input_length, row.output_length, false);
            ASSERT_EQ(STATUS_SUCCESS, status);
            std::vector<uint8_t> expected(row.reply_hex.size() / 2);
            for (size_t i = 0; i < expected.size(); ++i)
            {
                expected[i] = static_cast<uint8_t>(std::stoul(row.reply_hex.substr(i * 2, 2), nullptr, 16));
            }

            std::vector<uint8_t> reply(expected.size());
            this->emu->memory.read_memory(output, reply.data(), reply.size());
            EXPECT_EQ(expected.size(), information);
            EXPECT_EQ(expected, reply);
        }
    }
}
