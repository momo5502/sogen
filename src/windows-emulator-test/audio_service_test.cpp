#include "std_include.hpp"

#include "rpc_test_utils.hpp"

namespace sogen::test
{
    namespace
    {
        constexpr std::array<uint8_t, 16> mmdevice_interface_26100{0xb3, 0x85, 0x3f, 0x92, 0xee, 0xbb, 0xdf, 0x4e,
                                                                   0x80, 0x59, 0xf5, 0x69, 0xfa, 0x64, 0xa0, 0x27};
        constexpr std::array<uint8_t, 16> mmdevice_interface_20348{0x1f, 0x17, 0xbe, 0xa3, 0x25, 0xa5, 0xa3, 0x47,
                                                                   0xb7, 0x5a, 0x93, 0xda, 0x9d, 0x64, 0x5d, 0x1c};
        constexpr std::array<uint8_t, 16> mmdevice_interface_19041{0x79, 0xbd, 0x71, 0x8b, 0xbb, 0xcc, 0xab, 0x47,
                                                                   0xba, 0x09, 0x97, 0xca, 0x52, 0xc8, 0x1d, 0xa9};
        constexpr std::array<uint8_t, 16> audio_client_interface_20348{0x41, 0xc1, 0xb2, 0x98, 0xa2, 0xec, 0x19, 0x49,
                                                                       0x98, 0xb4, 0xa1, 0x98, 0x0d, 0x00, 0xd2, 0x34};
        constexpr std::array<uint8_t, 16> audio_client_interface_26100{0x11, 0xd1, 0x74, 0xd5, 0x26, 0x61, 0xd7, 0x49,
                                                                       0x9b, 0x86, 0x4d, 0xe6, 0xb6, 0x50, 0xd4, 0xfc};
        constexpr std::array<uint8_t, 16> audio_client_interface_26100_9549{0x75, 0x2e, 0x68, 0xcd, 0x49, 0x37, 0x4d, 0x4a,
                                                                            0xa6, 0xb1, 0xc0, 0x27, 0xed, 0x8a, 0x4c, 0x13};
        constexpr std::array<uint8_t, 16> audio_client_interface_19041{0x76, 0x52, 0x93, 0x5f, 0x7b, 0x1c, 0xf5, 0x46,
                                                                       0xac, 0x77, 0x07, 0x77, 0x59, 0x00, 0x1d, 0x2b};

        constexpr size_t reply_header_size = 0x18;
        constexpr uint32_t sample_rate = 44100;
        constexpr uint16_t block_align = 8;
        constexpr uint64_t one_second_hns = 10000000;

        std::array<uint8_t, 40> float_format(const uint16_t channels)
        {
            const auto frame_bytes = static_cast<uint16_t>(channels * 4);
            std::array<uint8_t, 40> format{};
            const auto put16 = [&](const size_t offset, const uint16_t value) { std::memcpy(&format[offset], &value, sizeof(value)); };
            const auto put32 = [&](const size_t offset, const uint32_t value) { std::memcpy(&format[offset], &value, sizeof(value)); };
            put16(0, 0xFFFE);
            put16(2, channels);
            put32(4, sample_rate);
            put32(8, sample_rate * frame_bytes);
            put16(12, frame_bytes);
            put16(14, 32);
            put16(16, 22);
            put16(18, 32);
            put32(20, 3);
            constexpr std::array<uint8_t, 16> ieee_float{0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
                                                         0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
            std::memcpy(&format[24], ieee_float.data(), ieee_float.size());
            return format;
        }

        class AudioServiceTest : public RpcProtocolTest
        {
          protected:
            static std::vector<uint8_t> request_with_format(const size_t format_offset, const size_t size, const uint16_t channels = 2)
            {
                std::vector<uint8_t> data(size);
                const auto format = float_format(channels);
                std::memcpy(data.data() + format_offset, format.data(), format.size());
                return data;
            }

            static uint32_t trailing_hresult(const std::vector<uint8_t>& payload)
            {
                return read<uint32_t>(payload, payload.size() - sizeof(uint32_t));
            }

            std::array<uint8_t, 20> open_stream(const size_t request_size, const uint16_t channels = 2)
            {
                const auto reply = this->call(4, request_with_format(152, request_size, channels));
                EXPECT_EQ(STATUS_SUCCESS, reply.status);
                std::array<uint8_t, 20> context{};
                if (!reply.payload)
                {
                    ADD_FAILURE() << "Open stream returned no reply";
                    return context;
                }

                constexpr std::string_view uuid_prefix = "SogenAudioSt";
                const std::string_view bytes(reinterpret_cast<const char*>(reply.payload->data()), reply.payload->size());
                const auto uuid = bytes.find(uuid_prefix);
                if (uuid == std::string_view::npos || uuid + 16 > bytes.size())
                {
                    ADD_FAILURE() << "Open stream reply carries no stream context";
                    return context;
                }

                std::memcpy(context.data() + 4, reply.payload->data() + uuid, 16);
                return context;
            }

            uint64_t create_stream(const std::array<uint8_t, 20>& context)
            {
                std::vector<uint8_t> data(44);
                std::memcpy(data.data(), context.data(), context.size());
                std::memcpy(data.data() + 24, &one_second_hns, sizeof(one_second_hns));

                const auto reply = this->call(7, data);
                EXPECT_EQ(STATUS_SUCCESS, reply.status);
                if (reply.handles.size() != 1)
                {
                    ADD_FAILURE() << "Create stream delivered " << reply.handles.size() << " handles";
                    return 0;
                }

                const auto* render_section = this->emu->process.sections.get(make_handle(reply.handles.front().handle));
                if (!render_section)
                {
                    ADD_FAILURE() << "Create stream delivered no render section";
                    return 0;
                }

                return render_section->backing_address;
            }

            uint64_t create_render_buffer(const std::array<uint8_t, 16>& interface_id, const uint16_t channels = 2)
            {
                this->bind(u"\\RPC Control\\AudioClientRpc", interface_id, 8);
                return this->create_stream(this->open_stream(368, channels));
            }

            void expect_format_accepted(const std::array<uint8_t, 16>& interface_id, const size_t width, const size_t format_offset,
                                        const size_t request_size)
            {
                this->bind(u"\\RPC Control\\AudioClientRpc", interface_id, width);
                const auto reply = this->call(1, request_with_format(format_offset, request_size));
                ASSERT_EQ(STATUS_SUCCESS, reply.status);
                ASSERT_TRUE(reply.payload.has_value());
                EXPECT_EQ(reply_header_size + width + sizeof(uint32_t), reply.payload->size()) << width;
                EXPECT_EQ(0u, trailing_hresult(*reply.payload)) << width;
            }

            void expect_session_opnums(const std::array<uint8_t, 16>& interface_id, const uint32_t get_state, const uint32_t destroy,
                                       const uint32_t derive_category)
            {
                this->bind(u"\\RPC Control\\AudioClientRpc", interface_id, 8);
                const std::vector<uint8_t> session(20);

                const auto state = this->call(get_state, session);
                ASSERT_EQ(STATUS_SUCCESS, state.status);
                ASSERT_TRUE(state.payload.has_value());
                EXPECT_EQ(reply_header_size + 28, state.payload->size());
                EXPECT_EQ(0u, trailing_hresult(*state.payload));

                const auto destroyed = this->call(destroy, session);
                ASSERT_EQ(STATUS_SUCCESS, destroyed.status);
                ASSERT_TRUE(destroyed.payload.has_value());
                EXPECT_EQ(reply_header_size + 24, destroyed.payload->size());

                const std::vector<uint8_t> category{3, 0, 0, 0, 0, 0, 0, 0};
                const auto derived = this->call(derive_category, category);
                ASSERT_EQ(STATUS_SUCCESS, derived.status);
                ASSERT_TRUE(derived.payload.has_value());
                EXPECT_EQ(3u, read<uint32_t>(*derived.payload, reply_header_size));
                EXPECT_EQ(0u, trailing_hresult(*derived.payload));
            }

            template <typename T>
            T read_guest(const uint64_t address)
            {
                return this->emu->memory.read_memory<T>(address);
            }

            bool play_cursor_reaches(const uint64_t play_cursor, const uint64_t value)
            {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while (std::chrono::steady_clock::now() < deadline)
                {
                    if (this->read_guest<uint64_t>(play_cursor) == value)
                    {
                        return true;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                return false;
            }
        };

        // Records every output the audio service opens, so a test can see which stream feeds which output.
        struct recorded_output
        {
            audio_format format{};
            std::atomic<uint64_t> submitted{};
            std::atomic<bool> nonzero{};
            std::atomic<bool> closed{};
        };

        class recording_audio_output final : public audio_output
        {
          public:
            explicit recording_audio_output(std::shared_ptr<recorded_output> record)
                : record_(std::move(record))
            {
            }

            ~recording_audio_output() override
            {
                this->record_->closed = true;
            }

            recording_audio_output(const recording_audio_output&) = delete;
            recording_audio_output& operator=(const recording_audio_output&) = delete;
            recording_audio_output(recording_audio_output&&) = delete;
            recording_audio_output& operator=(recording_audio_output&&) = delete;

            void submit(const void* data, const size_t size) override
            {
                const auto* bytes = static_cast<const uint8_t*>(data);
                if (std::any_of(bytes, bytes + size, [](const uint8_t b) { return b != 0; }))
                {
                    this->record_->nonzero = true;
                }
                this->record_->submitted += size;
            }

            std::optional<uint64_t> queued_bytes() const override
            {
                return 0;
            }

          private:
            std::shared_ptr<recorded_output> record_{};
        };

        class recording_audio_backend final : public audio_backend
        {
          public:
            std::unique_ptr<audio_output> open_output(const audio_format& format) override
            {
                auto record = std::make_shared<recorded_output>();
                record->format = format;
                {
                    const std::lock_guard lock{this->mutex};
                    this->outputs.push_back(record);
                }
                return std::make_unique<recording_audio_output>(std::move(record));
            }

            std::vector<std::shared_ptr<recorded_output>> opened() const
            {
                const std::lock_guard lock{this->mutex};
                return this->outputs;
            }

          private:
            mutable std::mutex mutex{};
            std::vector<std::shared_ptr<recorded_output>> outputs{};
        };

        class AudioMixingTest : public AudioServiceTest
        {
          protected:
            std::unique_ptr<audio_backend> create_audio_backend() override
            {
                auto recording = std::make_unique<recording_audio_backend>();
                this->backend = recording.get();
                return recording;
            }

            // The output a render buffer's stream opened once it started playing.
            std::shared_ptr<recorded_output> wait_for_output(const size_t count) const
            {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while (std::chrono::steady_clock::now() < deadline)
                {
                    if (const auto outputs = this->backend->opened(); outputs.size() >= count)
                    {
                        return outputs[count - 1];
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                return nullptr;
            }

            static bool reaches(const recorded_output& output, const uint64_t bytes)
            {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while (std::chrono::steady_clock::now() < deadline)
                {
                    if (output.submitted >= bytes)
                    {
                        return true;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                return false;
            }

            // Server 2022 layout: the write cursor at 0x10, PCM from 0x400.
            void play(const uint64_t header, const uint64_t write_cursor)
            {
                std::vector<uint8_t> pcm(static_cast<size_t>(write_cursor), 0x3c);
                this->emu->memory.write_memory(header + 0x400, pcm.data(), pcm.size());
                this->emu->memory.write_memory(header + 0x10, write_cursor);
            }

            recording_audio_backend* backend{};
        };

        // The default endpoint comes from the registry: this fixture loads the root's hives, marks every render endpoint
        // already there as not present and adds one active endpoint, so that one is the default.
        class AudioEndpointTest : public AudioServiceTest
        {
          protected:
            static constexpr std::string_view endpoint_id = "{0.0.0.00000000}.{5c0f26a6-6c26-4cbb-9a8d-1a2b3c4d5e6f}";

            AudioEndpointTest()
            {
                this->load_registry = true;
            }

            void SetUp() override
            {
                AudioServiceTest::SetUp();

                constexpr uint32_t reg_dword = 4;
                constexpr uint32_t device_state_active = 1;
                constexpr uint32_t device_state_not_present = 4;
                auto& registry = this->emu->registry;
                const auto set_state = [&](const registry_key& key, const uint32_t state) {
                    registry.set_value(key, "DeviceState", reg_dword, std::as_bytes(std::span{&state, 1}));
                };

                const std::filesystem::path render =
                    R"(\Registry\Machine\Software\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render)";
                const auto render_key = registry.create_key(render);
                ASSERT_TRUE(render_key.has_value());
                for (size_t index = 0;; ++index)
                {
                    const auto name = registry.get_sub_key_name(*render_key, index);
                    if (!name)
                    {
                        break;
                    }
                    if (const auto existing = registry.get_key(utils::path_key{render / std::string{*name}}))
                    {
                        set_state(*existing, device_state_not_present);
                    }
                }

                const auto endpoint_key = registry.create_key(render / std::string{endpoint_id});
                ASSERT_TRUE(endpoint_key.has_value());
                set_state(*endpoint_key, device_state_active);
            }
        };
    }

    TEST_F(AudioServiceTest, ReportsInitialPnpChange)
    {
        for (const auto& interface_id : {mmdevice_interface_20348, mmdevice_interface_26100})
        {
            for (const size_t width : {size_t{4}, size_t{8}})
            {
                this->bind(u"\\RPC Control\\Audiosrv", interface_id, width);
                const auto reply = this->call(0, {});
                ASSERT_EQ(STATUS_SUCCESS, reply.status);
                ASSERT_TRUE(reply.payload.has_value());
                const auto& payload = *reply.payload;
                const auto pointer_offset = reply_header_size + width;
                const auto data_offset = pointer_offset + 2 * width;
                ASSERT_EQ(data_offset + 16, payload.size()) << width;
                EXPECT_EQ(12u, read<uint32_t>(payload, reply_header_size)) << width;
                if (width == 4)
                {
                    EXPECT_NE(0u, read<uint32_t>(payload, pointer_offset));
                    EXPECT_EQ(12u, read<uint32_t>(payload, pointer_offset + width));
                }
                else
                {
                    EXPECT_NE(0u, read<uint64_t>(payload, pointer_offset));
                    EXPECT_EQ(12u, read<uint64_t>(payload, pointer_offset + width));
                }
                EXPECT_EQ(12u, read<uint32_t>(payload, data_offset)) << width;
                EXPECT_NE(0u, read<uint32_t>(payload, data_offset + 4)) << width;
                EXPECT_EQ(0u, trailing_hresult(payload)) << width;
            }
        }
    }

    // AudiosrvGetDefaultAudioEndpoint (opnum 25) on the 19041 interface, decoded as 19041's mmdevapi unmarshals it (procedure
    // 25 of its NDR64 and NDR20 formats, PDB names). The request carries EDataFlow and ERole as 32-bit values in NDR64 and as
    // FC_ENUM16 in NDR20. The reply is a unique pointer to a conformant wide string, with 64-bit or 32-bit referent and
    // counts, then the [out] int and the HRESULT. audiosrv's s_rtgGetDefaultAudioEndpoint sets that int to
    // DynamicAudioEndpointManager's flag for a default routed to the calling process alone, false for an ordinary process.
    // The interface's other opnums stay refused.
    TEST_F(AudioEndpointTest, Windows10DefaultEndpointReplyHasItsLayout)
    {
        const std::u16string expected_id(endpoint_id.begin(), endpoint_id.end());
        for (const size_t width : {size_t{8}, size_t{4}})
        {
            const std::vector<uint8_t> render_console(width == 8 ? 8 : 4);
            this->bind(u"\\RPC Control\\Audiosrv", mmdevice_interface_19041, width);
            const auto reply = this->call(25, render_console);
            ASSERT_EQ(STATUS_SUCCESS, reply.status) << width;
            ASSERT_TRUE(reply.payload.has_value());
            const auto& payload = *reply.payload;

            size_t offset = reply_header_size;
            const auto next = [&] {
                const uint64_t value = width == 8 ? read<uint64_t>(payload, offset) : read<uint32_t>(payload, offset);
                offset += width;
                return value;
            };

            EXPECT_NE(0u, next()) << width; // unique pointer referent
            const auto max_count = next();
            const auto string_offset = next();
            const auto actual_count = next();
            EXPECT_EQ(expected_id.size() + 1, max_count) << width;
            EXPECT_EQ(0u, string_offset) << width;
            ASSERT_EQ(expected_id.size() + 1, actual_count) << width;
            ASSERT_LE(offset + (actual_count * sizeof(char16_t)), payload.size());

            std::u16string id(static_cast<size_t>(actual_count), u'\0');
            std::memcpy(id.data(), payload.data() + offset, id.size() * sizeof(char16_t));
            EXPECT_EQ(expected_id + u'\0', id) << width;
            offset = (offset + (id.size() * sizeof(char16_t)) + 3) & ~size_t{3};

            EXPECT_EQ(0u, read<uint32_t>(payload, offset)) << width;     // not routed to this process alone
            EXPECT_EQ(0u, read<uint32_t>(payload, offset + 4)) << width; // S_OK
            EXPECT_EQ(offset + 8, payload.size()) << width;

            EXPECT_EQ(STATUS_NOT_SUPPORTED, this->call(0, {}).status) << width;
        }
    }

    TEST_F(AudioServiceTest, ReadsServer2022IsFormatSupportedFormat)
    {
        this->expect_format_accepted(audio_client_interface_20348, 8, 312, 368);
        this->expect_format_accepted(audio_client_interface_20348, 4, 284, 332);
    }

    TEST_F(AudioServiceTest, ReadsServer2025IsFormatSupportedFormat)
    {
        this->expect_format_accepted(audio_client_interface_26100, 8, 336, 392);
        this->expect_format_accepted(audio_client_interface_26100, 4, 308, 356);
    }

    TEST_F(AudioServiceTest, ReadsUpdatedWindows11IsFormatSupportedFormat)
    {
        this->expect_format_accepted(audio_client_interface_26100_9549, 8, 336, 392);
        this->expect_format_accepted(audio_client_interface_26100_9549, 4, 308, 356);
    }

    TEST_F(AudioServiceTest, AnswersServer2022SessionOpnums)
    {
        this->expect_session_opnums(audio_client_interface_20348, 26, 54, 120);
    }

    TEST_F(AudioServiceTest, AnswersServer2025SessionOpnums)
    {
        this->expect_session_opnums(audio_client_interface_26100, 27, 55, 122);
    }

    TEST_F(AudioServiceTest, CreatesServer2022RenderControlData)
    {
#if defined(OS_EMSCRIPTEN) && !defined(__EMSCRIPTEN_PTHREADS__)
        GTEST_SKIP() << "Requires host threads";
#endif
        const auto header = this->create_render_buffer(audio_client_interface_20348);
        ASSERT_NE(0u, header);

        EXPECT_EQ(0x45504344u, this->read_guest<uint32_t>(header + 0xC0));
        EXPECT_EQ(22u + 200u, this->read_guest<uint32_t>(header + 0xC8));
        EXPECT_EQ(uint16_t{0xFFFE}, this->read_guest<uint16_t>(header + 0x17C));
        EXPECT_EQ(sample_rate, this->read_guest<uint32_t>(header + 0x180));
        const auto data_start = this->read_guest<uint32_t>(header + 0x168);
        const auto data_end = this->read_guest<uint32_t>(header + 0x16C);
        EXPECT_EQ(0x400u, data_start);
        EXPECT_EQ(uint32_t{sample_rate} * block_align, data_end - data_start);
        EXPECT_LE(data_end, this->read_guest<uint32_t>(header + 0x170));

        this->emu->memory.write_memory(header + 0x10, uint64_t{4096});
        EXPECT_TRUE(this->play_cursor_reaches(header + 0x18, 4096));
        EXPECT_TRUE(this->play_cursor_reaches(header + 0x98, 4096));
    }

    TEST_F(AudioServiceTest, ReadsWindows10IsFormatSupportedFormat)
    {
        this->expect_format_accepted(audio_client_interface_19041, 8, 312, 368);
        this->expect_format_accepted(audio_client_interface_19041, 4, 284, 332);
    }

    TEST_F(AudioServiceTest, AnswersWindows10SessionOpnums)
    {
        this->expect_session_opnums(audio_client_interface_19041, 26, 54, 120);
    }

    TEST_F(AudioServiceTest, CreatesWindows10RenderControlData)
    {
#if defined(OS_EMSCRIPTEN) && !defined(__EMSCRIPTEN_PTHREADS__)
        GTEST_SKIP() << "Requires host threads";
#endif
        const auto header = this->create_render_buffer(audio_client_interface_19041);
        ASSERT_NE(0u, header);

        EXPECT_EQ(0x45504344u, this->read_guest<uint32_t>(header + 0xC0));
        EXPECT_EQ(22u + 200u, this->read_guest<uint32_t>(header + 0xC8));
        EXPECT_EQ(uint16_t{0xFFFE}, this->read_guest<uint16_t>(header + 0x17C));

        this->emu->memory.write_memory(header + 0x10, uint64_t{4096});
        EXPECT_TRUE(this->play_cursor_reaches(header + 0x18, 4096));
        EXPECT_TRUE(this->play_cursor_reaches(header + 0x98, 4096));
    }

    TEST_F(AudioServiceTest, PlacesWindows10RenderSectionAfterItsThirdStreamI64)
    {
#if defined(OS_EMSCRIPTEN) && !defined(__EMSCRIPTEN_PTHREADS__)
        GTEST_SKIP() << "Requires host threads";
#endif
        this->bind(u"\\RPC Control\\AudioClientRpc", audio_client_interface_19041, 8);
        const auto context = this->open_stream(368);
        std::vector<uint8_t> data(44);
        std::memcpy(data.data(), context.data(), context.size());
        std::memcpy(data.data() + 24, &one_second_hns, sizeof(one_second_hns));

        const auto reply = this->call(7, data);
        ASSERT_EQ(STATUS_SUCCESS, reply.status);
        ASSERT_TRUE(reply.payload.has_value());
        const auto& payload = *reply.payload;
        EXPECT_EQ(reply_header_size + 0x7C, payload.size());
        EXPECT_EQ(0u, read<uint64_t>(payload, reply_header_size + 0x30));
        EXPECT_EQ(1u, read<uint32_t>(payload, reply_header_size + 0x58));
        EXPECT_EQ(1u, read<uint32_t>(payload, reply_header_size + 0x5C));
        EXPECT_EQ(1u, read<uint64_t>(payload, reply_header_size + 0x60));
        EXPECT_EQ(0u, trailing_hresult(payload));
    }

    TEST_F(AudioServiceTest, KeepsServer2025RenderControlData)
    {
#if defined(OS_EMSCRIPTEN) && !defined(__EMSCRIPTEN_PTHREADS__)
        GTEST_SKIP() << "Requires host threads";
#endif
        const auto header = this->create_render_buffer(audio_client_interface_26100);
        ASSERT_NE(0u, header);

        EXPECT_EQ(1u, this->read_guest<uint32_t>(header));
        EXPECT_EQ(0x45504344u, this->read_guest<uint32_t>(header + 0xC8));
        EXPECT_EQ(uint16_t{0xFFFE}, this->read_guest<uint16_t>(header + 0x180));

        this->emu->memory.write_memory(header + 0x18, uint64_t{4096});
        EXPECT_TRUE(this->play_cursor_reaches(header + 0x20, 4096));
        EXPECT_TRUE(this->play_cursor_reaches(header + 0xA0, 4096));
    }

    // The Windows audio engine mixes every stream of a process. Games such as Hogwarts Legacy keep a device watcher
    // that opens and destroys a silent stream every few hundred milliseconds next to the one that plays.
    TEST_F(AudioMixingTest, APlayingStreamKeepsItsOutputWhileOtherStreamsComeAndGo)
    {
#if defined(OS_EMSCRIPTEN) && !defined(__EMSCRIPTEN_PTHREADS__)
        GTEST_SKIP() << "Requires host threads";
#endif
        const auto playing = this->create_render_buffer(audio_client_interface_20348);
        ASSERT_NE(0u, playing);
        this->play(playing, 4096);
        const auto playing_output = this->wait_for_output(1);
        ASSERT_NE(nullptr, playing_output);
        ASSERT_TRUE(reaches(*playing_output, 4096));
        auto playing_port = std::move(this->endpoint);

        for (size_t watcher = 0; watcher < 3; ++watcher)
        {
            const auto silent = this->create_render_buffer(audio_client_interface_20348, 8);
            ASSERT_NE(0u, silent);
            this->emu->memory.write_memory(silent + 0x10, uint64_t{1024});
            const auto silent_output = this->wait_for_output(2 + watcher);
            ASSERT_NE(nullptr, silent_output);
            EXPECT_EQ(8u, silent_output->format.channels);
            this->endpoint.reset();
            EXPECT_TRUE(silent_output->closed);
        }

        this->endpoint = std::move(playing_port);
        this->play(playing, 8192);
        EXPECT_TRUE(reaches(*playing_output, 8192));
        EXPECT_TRUE(playing_output->nonzero);
        EXPECT_FALSE(playing_output->closed);
        EXPECT_EQ(2u, playing_output->format.channels);
    }
}
