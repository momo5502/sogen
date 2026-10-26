#include "../std_include.hpp"
#include "audio_service.hpp"

#include "binary_writer.hpp"
#include "../windows_emulator.hpp"
#include "../registry/registry_utils.hpp"

#include <platform/unicode.hpp>
#include <utils/finally.hpp>
#include <utils/shared_host_buffer.hpp>

namespace sogen
{

    namespace
    {
        // The Audiosrv ALPC port hosts several RPC interfaces whose opnums overlap, so we dispatch by the
        // interface the client bound to.
        //   {923F85B3-BBEE-4EDF-8059-F569FA64A027} v1.6 = MMDevice enumeration (mmdevapi).
        //   {D574D111-6126-49D7-9B86-4DE6B650D4FC} v2.8 = "AudioClientRpc" / IAudioClient streaming (audioses).
        constexpr std::array<uint8_t, 16> k_iface_mmdevice_enum = {0xb3, 0x85, 0x3f, 0x92, 0xee, 0xbb, 0xdf, 0x4e,
                                                                   0x80, 0x59, 0xf5, 0x69, 0xfa, 0x64, 0xa0, 0x27};
        constexpr std::array<uint8_t, 16> k_iface_audio_client = {0x11, 0xd1, 0x74, 0xd5, 0x26, 0x61, 0xd7, 0x49,
                                                                  0x9b, 0x86, 0x4d, 0xe6, 0xb6, 0x50, 0xd4, 0xfc};

        constexpr uint32_t k_audio_opnum_mmdev_get_blob = 0;           // {923F85B3} [out] serialized blob, no [in]
        constexpr uint32_t k_audio_opnum_get_default_endpoint = 25;    // {923F85B3}
        constexpr uint32_t k_audio_opnum_get_mix_format = 0;           // {D574D111}
        constexpr uint32_t k_audio_opnum_is_format_supported = 1;      // {D574D111} AudioServerIsFormatSupported
        constexpr uint32_t k_audio_opnum_get_device_period = 2;        // {D574D111} AudioServerGetDevicePeriod
        constexpr uint32_t k_audio_opnum_open_stream = 4;              // {D574D111} (Initialize prep)
        constexpr uint32_t k_audio_opnum_get_audio_session = 6;        // {D574D111} AudioServerGetAudioSession
        constexpr uint32_t k_audio_opnum_create_stream = 7;            // {D574D111} CreateRemoteStream
        constexpr uint32_t k_audio_opnum_get_session_state = 27;       // {D574D111} AudioSessionGetState
        constexpr uint32_t k_audio_opnum_destroy_session = 55;         // {D574D111} AudioSessionDestroy
        constexpr uint32_t k_audio_opnum_destroy_stream = 13;          // {D574D111} AudioServerDestroyStream
        constexpr uint32_t k_audio_opnum_start_stream = 8;             // {D574D111} StartStream (IAudioClient::Start)
        constexpr uint32_t k_audio_opnum_stop_stream = 9;              // {D574D111} StopStream (IAudioClient::Stop)
        constexpr uint32_t k_audio_opnum_derive_stream_category = 122; // {D574D111} AudioServerDeriveStreamCategory

        constexpr NTSTATUS k_hr_ok = 0;
        constexpr NTSTATUS k_hr_false = 1;

        // An NDR [out] context handle: a 4-byte attributes field + a 16-byte UUID identifying the stream. The
        // client stores this and passes it back as the binding for the follow-on stream RPCs, at the start of their
        // request. Its last four bytes carry the stream id, since one client can hold several streams at once.
        constexpr std::array<uint8_t, 12> k_stream_context_uuid_prefix = {0x53, 0x6f, 0x67, 0x65, 0x6e, 0x41,
                                                                          0x75, 0x64, 0x69, 0x6f, 0x53, 0x74};

        std::array<uint8_t, 16> make_stream_context_uuid(const uint32_t stream_id)
        {
            std::array<uint8_t, 16> uuid{};
            std::memcpy(uuid.data(), k_stream_context_uuid_prefix.data(), k_stream_context_uuid_prefix.size());
            std::memcpy(uuid.data() + k_stream_context_uuid_prefix.size(), &stream_id, sizeof(stream_id));
            return uuid;
        }

        std::optional<uint32_t> read_stream_id(windows_emulator& win_emu, const lpc_request_context& c)
        {
            constexpr size_t context_attributes_size = 4;
            std::array<uint8_t, 16> uuid{};
            if (!c.send_buffer || c.send_buffer_length < context_attributes_size + uuid.size())
            {
                return std::nullopt;
            }

            win_emu.emu().read_memory(c.send_buffer + context_attributes_size, uuid.data(), uuid.size());
            if (!std::equal(k_stream_context_uuid_prefix.begin(), k_stream_context_uuid_prefix.end(), uuid.begin()))
            {
                return std::nullopt;
            }

            uint32_t stream_id{};
            std::memcpy(&stream_id, uuid.data() + k_stream_context_uuid_prefix.size(), sizeof(stream_id));
            return stream_id;
        }

        // The audio-session context handle handed back by AudioServerGetAudioSession (opnum 6) and round-tripped
        // by the AudioSession* opnums (e.g. AudioSessionGetState, opnum 27). Opaque to the client, which only
        // binds follow-on session RPCs to it.
        constexpr std::array<uint8_t, 16> k_session_context_uuid = {0x53, 0x6f, 0x67, 0x65, 0x6e, 0x41, 0x75, 0x64,
                                                                    0x69, 0x6f, 0x53, 0x65, 0x73, 0x73, 0x00, 0x01};

        // The audio endpoint database lives in the registry under
        // HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\{Render,Capture}. Each endpoint is a
        // sub-key named by its endpoint id ("{0.0.<flow>.00000000}.{<guid>}"). mmdevapi resolves a device by
        // opening that registry key, so GetDefaultEndpoint must return an id that actually exists there.
        std::optional<std::u16string> find_default_endpoint_id(windows_emulator& win_emu, const uint32_t data_flow)
        {
            const std::string base = R"(\Registry\Machine\Software\Microsoft\Windows\CurrentVersion\MMDevices\Audio\)";

            // A headless/server host has no physical audio device, so the local Render/Capture folders are
            // empty; an RDP session instead populates RemoteRender/RemoteCapture with the redirected endpoint.
            // Scan both, preferring a local endpoint when one exists.
            const std::array<const char*, 2> folders = data_flow == 0 ? std::array<const char*, 2>{"Render", "RemoteRender"}
                                                                      : std::array<const char*, 2>{"Capture", "RemoteCapture"};

            std::optional<std::u16string> first_active{};
            std::optional<std::u16string> first_any{};

            for (const auto* folder : folders)
            {
                const std::filesystem::path root = base + folder;
                const auto key = win_emu.registry.get_key(utils::path_key{root});
                if (!key)
                {
                    continue;
                }

                for (size_t i = 0;; ++i)
                {
                    const auto name = win_emu.registry.get_sub_key_name(*key, i);
                    if (!name)
                    {
                        break;
                    }

                    const std::string name_str(*name);
                    std::u16string id(name_str.begin(), name_str.end());
                    const auto sub_key = win_emu.registry.get_key(utils::path_key{root / name_str});
                    uint32_t state = 0;
                    if (sub_key)
                    {
                        if (const auto v = win_emu.registry.get_value(*sub_key, "DeviceState"))
                        {
                            state = v->as_dword().value_or(0);
                        }
                    }

                    if (!first_any)
                    {
                        first_any = id;
                    }
                    if (state == 1 /* DEVICE_STATE_ACTIVE */ && !first_active)
                    {
                        first_active = id;
                    }
                }
            }

            return first_active ? first_active : first_any;
        }

        // Layout of the WASAPI shared-buffer control header the guest maps. The client writes interleaved PCM
        // into the sample area at k_render_data_offset (past the DCPE control header) and advances the write
        // cursor (bytes queued) at +0x18; the audio engine (this emulator) advances the play cursor (bytes
        // consumed) at +0x20. CCrossProcessBaseClientEndpoint::GetCurrentPadding reports (write - play) /
        // block_align frames, so a streaming client blocks until the engine drains the buffer by advancing the
        // play cursor. Offsets confirmed against a live capture and audioses!GetCurrentPadding.
        constexpr uint32_t k_render_data_offset = 0x400;
        constexpr uint32_t k_write_cursor_offset = 0x18;
        constexpr uint32_t k_play_cursor_offset = 0x20;

        constexpr uint64_t k_hns_per_second = 10000000;
        constexpr uint64_t k_default_buffer_duration = k_hns_per_second; // 1 s

        // How far the guest may run ahead of what the host device has actually played. It keeps the sink supplied
        // (the guest's own buffer is often only ~20 ms, far too thin a cushion on its own) while bounding total
        // latency to roughly this plus that buffer.
        constexpr uint64_t k_host_queue_cushion_ms = 60;

        // A stream stops feeding the host while the device still holds a second of it. The byte rate comes from the
        // guest's format, so the limit also has an absolute ceiling (a second of 8-channel float at 192 kHz is 6 MiB).
        constexpr uint64_t k_maximum_host_queue_bytes = 16ull * 1024 * 1024;

        // The engine period reported by handle_get_device_period, and the same span expressed in bytes. An
        // event-driven client is woken once per period consumed, so this also paces how often we signal it.
        constexpr int64_t k_device_period_hns = 100000; // 10 ms
        constexpr int64_t k_device_minimum_period_hns = 30000;
        constexpr auto k_device_period = std::chrono::microseconds{k_device_period_hns / 10};
        constexpr uint32_t k_maximum_pending_render_wakes = 4;
        constexpr auto k_maximum_render_event_lag = k_device_period * k_maximum_pending_render_wakes;

        struct wave_format_extensible
        {
            uint16_t tag{};
            uint16_t channels{};
            uint32_t sample_rate{};
            uint32_t average_bytes_per_second{};
            uint16_t block_align{};
            uint16_t bits_per_sample{};
            uint16_t extra_size{};
            uint16_t valid_bits_per_sample{};
            uint32_t channel_mask{};
            uint32_t subtype{};
            std::array<uint8_t, 12> subtype_tail{};
        };

        static_assert(sizeof(wave_format_extensible) == 40);

        struct stream_audio_format
        {
            wave_format_extensible wave_format{};
            audio_format host_format{};
            uint32_t average_bytes_per_second{};
            uint16_t block_align{};
        };

        // Where AudioClientRpc v2.8 places the deferred WAVEFORMATEXTENSIBLE of a request, in NDR32 and NDR64.
        struct request_format_offsets
        {
            size_t ndr32{};
            size_t ndr64{};
        };

        // Opnum 4 offsets were confirmed against native audiosrv traffic, opnum 1 offsets against the requests of 32- and
        // 64-bit IAudioClient::IsFormatSupported callers.
        constexpr request_format_offsets k_open_stream_format_offsets{.ndr32 = 136, .ndr64 = 152};
        constexpr request_format_offsets k_is_format_supported_format_offsets{.ndr32 = 308, .ndr64 = 336};

        std::optional<stream_audio_format> read_stream_audio_format(windows_emulator& win_emu, const lpc_request_context& c,
                                                                    const size_t pointer_size, const request_format_offsets& offsets)
        {
            const auto format_offset = pointer_size == utils::aligned_binary_writer::pointer_size_32 ? offsets.ndr32 : offsets.ndr64;
            constexpr uint16_t wave_format_extensible_tag = 0xFFFE;
            constexpr uint16_t extensible_size = 22;
            constexpr uint32_t subtype_pcm = 1;
            constexpr uint32_t subtype_ieee_float = 3;
            constexpr std::array<uint8_t, 12> ksdataformat_subtype_tail = {0x00, 0x00, 0x10, 0x00, 0x80, 0x00,
                                                                           0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};

            stream_audio_format result{};
            if (!c.send_buffer || c.send_buffer_length < format_offset + sizeof(result.wave_format))
            {
                return std::nullopt;
            }
            win_emu.emu().read_memory(c.send_buffer + format_offset, &result.wave_format, sizeof(result.wave_format));

            const auto& format = result.wave_format;

            if (format.tag != wave_format_extensible_tag || format.extra_size != extensible_size)
            {
                return std::nullopt;
            }

            if (format.channels == 0 || format.channels > 32 || format.sample_rate == 0 || format.bits_per_sample % 8 != 0 ||
                format.valid_bits_per_sample != format.bits_per_sample)
            {
                return std::nullopt;
            }

            if (format.subtype_tail != ksdataformat_subtype_tail)
            {
                return std::nullopt;
            }

            const bool supported_pcm = format.subtype == subtype_pcm &&
                                       (format.bits_per_sample == 8 || format.bits_per_sample == 16 || format.bits_per_sample == 32);
            const bool supported_float = format.subtype == subtype_ieee_float && format.bits_per_sample == 32;
            if (!supported_pcm && !supported_float)
            {
                return std::nullopt;
            }

            const auto bytes_per_sample = format.bits_per_sample / 8;
            const auto expected_block_align = static_cast<uint32_t>(format.channels) * bytes_per_sample;
            const auto expected_average = static_cast<uint64_t>(format.sample_rate) * format.block_align;
            if (expected_block_align != format.block_align || expected_average != format.average_bytes_per_second)
            {
                return std::nullopt;
            }

            result.host_format = {.sample_rate = format.sample_rate,
                                  .channels = format.channels,
                                  .bits_per_sample = format.bits_per_sample,
                                  .is_float = supported_float};
            result.average_bytes_per_second = format.average_bytes_per_second;
            result.block_align = format.block_align;
            return result;
        }

        // The host device behind every stream of an audio-service port. The host plays a single format, so the most
        // recently started stream owns it and the others are drained without sound; once the owner goes away, the next
        // stream that still plays takes the device over. XAudio2 briefly opens a second stream next to its first and
        // then destroys it, so neither stream may feed or stop the device while the other one owns it.
        class audio_sink
        {
          public:
            void take_over(audio_backend& audio, const uint32_t stream_id, const audio_format& format)
            {
                const std::lock_guard lock{this->mutex_};
                if (this->owner_ != 0 && this->owner_ != stream_id)
                {
                    audio.stop();
                }

                this->owner_ = stream_id;
                this->reopen_pending_ = false;
                this->has_device_ = audio.start(format);
            }

            struct feed_result
            {
                bool submitted{};
                std::optional<uint64_t> queued_bytes{};
            };

            // Hands the PCM to the device if the stream still owns it, along with how much the device has not played
            // yet when it can report that.
            template <typename Submit>
            feed_result feed(audio_backend& audio, const uint32_t stream_id, const audio_format& format, const Submit& submit)
            {
                const std::lock_guard lock{this->mutex_};
                if (this->owner_ == 0)
                {
                    this->owner_ = stream_id;
                    this->reopen_pending_ = true;
                }

                if (this->owner_ != stream_id)
                {
                    return {};
                }

                if (this->reopen_pending_)
                {
                    this->reopen_pending_ = false;
                    this->has_device_ = audio.start(format);
                }

                if (!this->has_device_)
                {
                    return {};
                }

                submit(audio);
                return {.submitted = true, .queued_bytes = audio.queued_bytes()};
            }

            void release(audio_backend& audio, const uint32_t stream_id)
            {
                const std::lock_guard lock{this->mutex_};
                if (this->owner_ != stream_id)
                {
                    return;
                }

                audio.stop();
                this->owner_ = 0;
                this->has_device_ = false;
                this->reopen_pending_ = false;
            }

            uint32_t owner() const
            {
                const std::lock_guard lock{this->mutex_};
                return this->owner_;
            }

            // The device was not carried over from the snapshot, so a restored owner reopens it on its next feed.
            void restore_owner(const uint32_t stream_id)
            {
                const std::lock_guard lock{this->mutex_};
                this->owner_ = stream_id;
                this->has_device_ = false;
                this->reopen_pending_ = stream_id != 0;
            }

          private:
            mutable std::mutex mutex_{};
            uint32_t owner_{};
            bool has_device_{};
            bool reopen_pending_{};
        };

        // Emulates the audio endpoint's hardware DMA. The render section handed to the guest is backed by a
        // host-owned buffer aliased into the guest address space (allocate_host_memory), so the guest and this
        // object share the exact same memory. A host thread drains that buffer at the device rate: it forwards
        // newly committed PCM to the host sink and advances the play cursor so the guest's render loop sees the
        // buffer emptying and keeps producing. Because the buffer is plain host memory, the thread touches it
        // directly -- no memory-access hooks (which the WHP backend does not honor) and no calls into the CPU
        // backend from a foreign thread (which is not thread-safe).
        class render_stream
        {
            enum class worker_status
            {
                running,
                pause_requested,
                paused,
                stopping,
            };

          public:
            struct state
            {
                uint32_t id{};
                bool claimed_sink{};
                uint64_t render_event{};
                stream_audio_format format{};
                uint32_t buffer_bytes{};
                uint64_t section_size{};
                uint64_t guest_address{};
                uint64_t submitted{};
                std::vector<uint8_t> contents{};

                void serialize(utils::buffer_serializer& buffer) const
                {
                    buffer.write(this->id);
                    buffer.write(this->claimed_sink);
                    buffer.write(this->render_event);
                    buffer.write(this->format);
                    buffer.write(this->buffer_bytes);
                    buffer.write(this->section_size);
                    buffer.write(this->guest_address);
                    buffer.write(this->submitted);
                    buffer.write_vector(this->contents);
                }

                void deserialize(utils::buffer_deserializer& buffer)
                {
                    buffer.read(this->id);
                    buffer.read(this->claimed_sink);
                    buffer.read(this->render_event);
                    buffer.read(this->format);
                    buffer.read(this->buffer_bytes);
                    buffer.read(this->section_size);
                    buffer.read(this->guest_address);
                    buffer.read(this->submitted);
                    buffer.read_vector(this->contents);
                }
            };

            render_stream(windows_emulator& win_emu, audio_sink& sink, const uint32_t id, const stream_audio_format& format,
                          const uint32_t buffer_bytes, const uint64_t section_size, const uint8_t* control_header,
                          const size_t control_header_size)
                : win_emu_(win_emu),
                  sink_(sink),
                  id_(id),
                  stream_format_(format),
                  buffer_bytes_(buffer_bytes),
                  section_size_(section_size),
                  host_storage_(static_cast<size_t>(section_size))
            {
                this->initialize_host_storage();
                std::memcpy(this->host_ptr_, control_header, control_header_size);

                this->guest_address_ =
                    win_emu.memory.allocate_host_memory(static_cast<size_t>(section_size), this->host_ptr_, memory_permission::read_write);
                if (this->guest_address_ == 0)
                {
                    return;
                }

                this->mapped_ = true;
                this->start();
            }

            render_stream(windows_emulator& win_emu, audio_sink& sink, state restored)
                : win_emu_(win_emu),
                  sink_(sink),
                  id_(restored.id),
                  claimed_sink_(restored.claimed_sink),
                  render_event_(restored.render_event),
                  stream_format_(restored.format),
                  buffer_bytes_(restored.buffer_bytes),
                  section_size_(restored.section_size),
                  guest_address_(restored.guest_address),
                  submitted_(restored.submitted),
                  host_storage_(validated_storage_size(restored))
            {
                this->initialize_host_storage();
                std::memcpy(this->host_ptr_, restored.contents.data(), restored.contents.size());

                if (!win_emu.memory.host_window_is_free(this->guest_address_, static_cast<size_t>(this->section_size_)))
                {
                    throw std::runtime_error("[audio-service] Cannot restore render stream: saved guest buffer address is unavailable");
                }

                if (!win_emu.memory.allocate_host_memory_at(this->guest_address_, static_cast<size_t>(this->section_size_), this->host_ptr_,
                                                            memory_permission::read_write))
                {
                    throw std::runtime_error("[audio-service] Cannot restore render stream: saved guest buffer address is unavailable");
                }

                this->mapped_ = true;
                // The prior backend queue is opaque and no longer exists, so only samples not submitted before the snapshot remain pending.
                this->write_play_cursor(std::max(this->read_cursor(k_play_cursor_offset), this->submitted_));
            }

            ~render_stream()
            {
                this->worker_status_.store(worker_status::stopping, std::memory_order_release);
                this->worker_status_.notify_all();
                if (this->thread_.joinable())
                {
                    this->thread_.join();
                }

                this->sink_.release(this->win_emu_.audio(), this->id_);
                if (this->mapped_)
                {
                    this->win_emu_.memory.release_memory(this->guest_address_, static_cast<size_t>(this->section_size_));
                }
            }

            render_stream(const render_stream&) = delete;
            render_stream& operator=(const render_stream&) = delete;
            render_stream(render_stream&&) = delete;
            render_stream& operator=(render_stream&&) = delete;

            uint64_t guest_address() const
            {
                return this->guest_address_;
            }

            void start()
            {
                this->thread_ = std::thread(&render_stream::run, this);
            }

            void set_render_event(const uint64_t event)
            {
                this->render_event_.store(event, std::memory_order_relaxed);
            }

            // Holds the render thread between two polls, so its state stays put until resume_worker().
            void pause_worker() const
            {
                auto status = worker_status::running;
                while (!this->worker_status_.compare_exchange_strong(status, worker_status::pause_requested, std::memory_order_acq_rel,
                                                                     std::memory_order_acquire))
                {
                    if (status == worker_status::stopping)
                    {
                        throw std::runtime_error("[audio-service] Cannot snapshot a stopping render stream");
                    }

                    this->worker_status_.wait(status, std::memory_order_acquire);
                    status = worker_status::running;
                }

                status = this->worker_status_.load(std::memory_order_acquire);
                while (status == worker_status::pause_requested)
                {
                    this->worker_status_.wait(status, std::memory_order_acquire);
                    status = this->worker_status_.load(std::memory_order_acquire);
                }

                if (status != worker_status::paused)
                {
                    throw std::runtime_error("[audio-service] Render stream stopped while taking a snapshot");
                }
            }

            void resume_worker() const
            {
                auto expected = worker_status::paused;
                this->worker_status_.compare_exchange_strong(expected, worker_status::running, std::memory_order_release,
                                                             std::memory_order_relaxed);
                this->worker_status_.notify_all();
            }

            state paused_state() const
            {
                state result{};
                result.id = this->id_;
                result.claimed_sink = this->claimed_sink_;
                result.render_event = this->render_event_.load(std::memory_order_relaxed);
                result.format = this->stream_format_;
                result.buffer_bytes = this->buffer_bytes_;
                result.section_size = this->section_size_;
                result.guest_address = this->guest_address_;
                result.submitted = this->submitted_;
                result.contents.assign(this->host_ptr_, this->host_ptr_ + this->section_size_);
                return result;
            }

          private:
            static constexpr size_t k_audio_page_size = 0x1000;

            static size_t validated_storage_size(const state& restored)
            {
                if (restored.section_size > std::numeric_limits<size_t>::max() - k_audio_page_size ||
                    restored.contents.size() != restored.section_size || restored.section_size < k_render_data_offset ||
                    restored.buffer_bytes == 0 || restored.buffer_bytes > restored.section_size - k_render_data_offset)
                {
                    throw std::runtime_error("[audio-service] Cannot restore invalid render stream storage");
                }

                return static_cast<size_t>(restored.section_size);
            }

            void initialize_host_storage()
            {
                this->host_ptr_ = this->host_storage_.data();
            }

            uint64_t read_cursor(const uint32_t offset) const
            {
                uint64_t value = 0;
                std::memcpy(&value, this->host_ptr_ + offset, sizeof(value));
                return value;
            }

            void write_play_cursor(const uint64_t value)
            {
                std::memcpy(this->host_ptr_ + k_play_cursor_offset, &value, sizeof(value));
            }

            void run()
            {
                bool started = false;
                uint64_t play_base{};
                std::chrono::steady_clock::time_point anchor{};
                std::chrono::steady_clock::time_point next_signal{};

                while (true)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));

                    auto status = this->worker_status_.load(std::memory_order_acquire);
                    if (status == worker_status::stopping)
                    {
                        break;
                    }
                    if (status == worker_status::pause_requested)
                    {
                        if (this->worker_status_.compare_exchange_strong(status, worker_status::paused, std::memory_order_acq_rel,
                                                                         std::memory_order_acquire))
                        {
                            this->worker_status_.notify_all();
                            this->worker_status_.wait(worker_status::paused, std::memory_order_acquire);
                        }

                        if (this->worker_status_.load(std::memory_order_acquire) == worker_status::stopping)
                        {
                            break;
                        }
                        continue;
                    }

                    const auto write = this->read_cursor(k_write_cursor_offset);
                    if (write == 0)
                    {
                        continue;
                    }

                    if (!started)
                    {
                        started = true;
                        play_base = this->read_cursor(k_play_cursor_offset);
                        anchor = std::chrono::steady_clock::now();
                    }

                    if (!this->claimed_sink_)
                    {
                        this->claimed_sink_ = true;
                        this->sink_.take_over(this->win_emu_.audio(), this->id_, this->stream_format_.host_format);
                    }

                    uint64_t play{};

                    // The cursors are guest memory, so a corrupt write cursor must not make the host replay the ring
                    // without end: at most one ring is new since the last poll, a cursor that went backwards starts
                    // over, and a device that reports its queue never holds more than a second of this stream. Whatever
                    // does not fit is dropped, keeping the newest whole frames.
                    const uint64_t ring_bytes = this->buffer_bytes_;
                    const uint64_t pending = write >= this->submitted_ ? write - this->submitted_ : write;
                    const auto maximum_queued =
                        std::min<uint64_t>(this->stream_format_.average_bytes_per_second, k_maximum_host_queue_bytes);

                    const auto fed =
                        this->sink_.feed(this->win_emu_.audio(), this->id_, this->stream_format_.host_format, [&](audio_backend& audio) {
                            const auto queued = audio.queued_bytes().value_or(0);
                            auto budget = queued < maximum_queued ? maximum_queued - queued : 0;
                            budget -= budget % this->stream_format_.block_align;
                            const uint64_t start = write - std::min({pending, ring_bytes, budget});

                            for (uint64_t pos = start; pos < write;)
                            {
                                const uint64_t ring_offset = pos % this->buffer_bytes_;
                                const uint64_t chunk = std::min<uint64_t>(write - pos, this->buffer_bytes_ - ring_offset);
                                audio.submit(this->host_ptr_ + k_render_data_offset + ring_offset, static_cast<size_t>(chunk));
                                pos += chunk;
                            }
                        });

                    if (fed.submitted)
                    {
                        // Advance the play cursor so the guest sees genuine backpressure. Prefer deriving it from
                        // what the device actually played (everything submitted minus what is still queued) plus a
                        // fixed cushion: a wall-clock estimate drifts ahead of the device clock, so the host queue
                        // grows without bound and latency creeps up to seconds the longer playback runs. The
                        // cushion keeps the sink from starving, since the guest's own buffer is far too small to
                        // serve as one. Fall back to the clock when the sink cannot report its queue.
                        uint64_t played{};
                        if (const auto queued = fed.queued_bytes)
                        {
                            const auto consumed = write > *queued ? write - *queued : 0;
                            const auto cushion = this->stream_format_.average_bytes_per_second * k_host_queue_cushion_ms / 1000;
                            played = consumed + cushion;
                        }
                        else
                        {
                            const auto elapsed_ns =
                                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - anchor).count();
                            played = play_base +
                                     this->stream_format_.average_bytes_per_second * static_cast<uint64_t>(elapsed_ns) / k_ns_per_second;
                        }
                        play = std::min(played, write);
                    }
                    else
                    {
                        // No host audio device (e.g. headless / CI), or another stream owns it: keep the play cursor
                        // level with the write cursor so the guest's render loop never blocks waiting for a buffer that
                        // nothing drains.
                        play = write;
                    }

                    this->write_play_cursor(play);

                    // Wake an EVENTCALLBACK client: it fills one buffer, then waits on its render event for the
                    // engine to report an elapsed period before writing more. Pace that to one wake per period,
                    // like a real engine -- firing on every poll tick only wakes the client to find no new space,
                    // and each wake costs a guest thread dispatch. Drive it from the clock rather than from how far
                    // the play cursor moved: the cursor never passes the write cursor, so a client that wakes
                    // without writing would stall it and never be woken again. Keep the wakes on a fixed period
                    // grid: poll ticks don't line up with periods, and clients such as XAudio2 render exactly one
                    // period per wake, so signalling a period after the previous wake falls behind the device
                    // rate and starves the sink. A bounded lag lets late wakes catch up without bursting after a
                    // long stall.
                    const auto now = std::chrono::steady_clock::now();
                    const auto render_event = this->render_event_.load(std::memory_order_relaxed);
                    if (render_event && now >= next_signal)
                    {
                        this->win_emu_.request_guest_event_signal(make_handle(render_event), k_maximum_pending_render_wakes);
                        next_signal = std::max(next_signal + k_device_period, now - k_maximum_render_event_lag);
                    }

                    this->submitted_ = write;
                }
            }

            static constexpr uint64_t k_ns_per_second = 1'000'000'000ULL;

            windows_emulator& win_emu_;
            audio_sink& sink_;
            uint32_t id_{};
            bool claimed_sink_{};
            std::atomic<uint64_t> render_event_{};
            stream_audio_format stream_format_{};
            uint32_t buffer_bytes_{};
            uint64_t section_size_{};
            uint64_t guest_address_{0};
            uint64_t submitted_{};
            utils::shared_host_buffer host_storage_;
            uint8_t* host_ptr_{nullptr};
            bool mapped_{};
            mutable std::atomic<worker_status> worker_status_{worker_status::running};
            std::thread thread_;
        };

        struct audio_service_port : rpc_port
        {
            NTSTATUS handle_rpc(windows_emulator& win_emu, const uint32_t procedure_id, const lpc_request_context& c,
                                utils::aligned_binary_writer& writer, std::vector<alpc_reply_handle>& reply_handles) override
            {
                const auto& iface = this->bound_interface();
                if (iface == k_iface_audio_client)
                {
                    switch (procedure_id)
                    {
                    case k_audio_opnum_get_mix_format:
                        return handle_get_mix_format(writer);
                    case k_audio_opnum_is_format_supported:
                        return handle_is_format_supported(win_emu, c, writer);
                    case k_audio_opnum_get_device_period:
                        return handle_get_device_period(writer);
                    case k_audio_opnum_destroy_stream:
                        this->destroy_stream(win_emu, c);
                        return handle_post_create(writer);
                    case k_audio_opnum_open_stream:
                        return handle_open_stream(win_emu, c, writer);
                    case k_audio_opnum_get_audio_session:
                        return handle_get_audio_session(writer);
                    case k_audio_opnum_get_session_state:
                        return handle_get_session_state(writer);
                    case k_audio_opnum_destroy_session:
                        return handle_destroy_session(writer);
                    case k_audio_opnum_create_stream:
                        return handle_create_stream(win_emu, c, writer, reply_handles);
                    case k_audio_opnum_start_stream:
                        this->bind_render_event(win_emu, c);
                        return handle_post_create(writer);
                    case k_audio_opnum_stop_stream:
                        return handle_post_create(writer);
                    case k_audio_opnum_derive_stream_category:
                        return handle_derive_stream_category(win_emu, c, writer);
                    case 5:
                        return handle_post_create(writer);
                    default:
                        return STATUS_NOT_SUPPORTED;
                    }
                }

                if (iface != k_iface_mmdevice_enum)
                {
                    return STATUS_NOT_SUPPORTED;
                }

                switch (procedure_id)
                {
                case k_audio_opnum_mmdev_get_blob:
                    return handle_mmdev_get_blob(writer);
                case k_audio_opnum_get_default_endpoint:
                    return handle_get_default_endpoint(win_emu, c, writer);
                default:
                    return STATUS_NOT_SUPPORTED;
                }
            }

            // Every render thread is held before anything is recorded: a stream taking the device over between two
            // streams' states would otherwise save an owner that does not match them.
            void serialize_object(utils::buffer_serializer& buffer) const override
            {
                std::vector<const render_stream*> paused{};
                paused.reserve(this->streams_.size());
                const auto resume = utils::finally([&] {
                    for (const auto* stream : paused)
                    {
                        stream->resume_worker();
                    }
                });

                for (const auto& stream : this->streams_ | std::views::values)
                {
                    if (stream.render)
                    {
                        stream.render->pause_worker();
                        paused.push_back(stream.render.get());
                    }
                }

                rpc_port::serialize_object(buffer);
                buffer.write(this->sink_.owner());
                buffer.write(static_cast<uint64_t>(this->streams_.size()));
                for (const auto& [id, stream] : this->streams_)
                {
                    buffer.write(id);
                    buffer.write_optional(stream.format);
                    buffer.write(stream.render != nullptr);
                    if (stream.render)
                    {
                        buffer.write(stream.render->paused_state());
                    }
                    buffer.write(stream.section);
                }
            }

            void deserialize_object(utils::buffer_deserializer& buffer) override
            {
                rpc_port::deserialize_object(buffer);
                buffer.read(this->restored_sink_owner_);
                this->streams_.clear();
                const auto stream_count = buffer.read<uint64_t>();
                for (uint64_t i = 0; i < stream_count; ++i)
                {
                    auto& stream = this->streams_[buffer.read<uint32_t>()];
                    buffer.read_optional(stream.format);
                    if (buffer.read<bool>())
                    {
                        stream.restored.emplace(buffer.read<render_stream::state>());
                    }
                    buffer.read(stream.section);
                }
            }

            void prepare_for_state_restore(windows_emulator& win_emu) override
            {
                for (auto& stream : this->streams_ | std::views::values)
                {
                    detach_render_stream(win_emu, stream);
                }
                this->streams_.clear();
            }

            void restore_after_state_restore(windows_emulator& win_emu) override
            {
                for (auto& stream : this->streams_ | std::views::values)
                {
                    if (!stream.restored)
                    {
                        if (stream.section.bits != 0)
                        {
                            throw std::runtime_error("[audio-service] Cannot restore render stream: section exists without stream state");
                        }
                        continue;
                    }

                    if (!stream.format)
                    {
                        throw std::runtime_error("[audio-service] Cannot restore render stream: negotiated format is missing");
                    }

                    auto* section = win_emu.process.sections.get(stream.section);
                    if (!section || section->backing_address != stream.restored->guest_address)
                    {
                        throw std::runtime_error("[audio-service] Cannot restore render stream: section metadata is inconsistent");
                    }

                    stream.render = std::make_unique<render_stream>(win_emu, this->sink_, std::move(*stream.restored));
                    section->backing_address = stream.render->guest_address();
                    stream.restored.reset();
                }

                this->sink_.restore_owner(this->restored_sink_owner_);
                for (auto& stream : this->streams_ | std::views::values)
                {
                    if (stream.render)
                    {
                        stream.render->start();
                    }
                }
            }

          private:
            struct audio_stream
            {
                std::optional<stream_audio_format> format{};
                std::unique_ptr<render_stream> render{};
                std::optional<render_stream::state> restored{};
                handle section{};
            };

            audio_sink sink_{};
            std::map<uint32_t, audio_stream> streams_{};
            uint32_t restored_sink_owner_{};

            static void detach_render_stream(windows_emulator& win_emu, audio_stream& stream)
            {
                stream.render.reset();
                stream.restored.reset();
                if (auto* section = win_emu.process.sections.get(stream.section))
                {
                    section->backing_address = 0;
                }
                stream.section = {};
            }

            // The endpoint control message carrying the render event does not name its stream. IAudioClient::Start fails
            // unless an EVENTCALLBACK client set its event first, and audioses forwards SetEventHandle on the calling
            // thread, so the event this thread registered belongs to the stream it now starts (XAudio2 does both on one
            // thread per stream). A lone pending event is unambiguous even when Start comes from another thread; with
            // several, binding a guess could wake the wrong client.
            void bind_render_event(windows_emulator& win_emu, const lpc_request_context& c)
            {
                const auto id = read_stream_id(win_emu, c);
                const auto stream = id ? this->streams_.find(*id) : this->streams_.end();
                if (stream == this->streams_.end() || !stream->second.render)
                {
                    win_emu.log.warn("[audio-service] StartStream for an unknown or unopened stream\n");
                    return;
                }

                auto& by_stream = win_emu.process.audio_render_events_by_stream;
                if (const auto registered = by_stream.find(*id); registered != by_stream.end())
                {
                    stream->second.render->set_render_event(registered->second);
                    by_stream.erase(registered);
                    return;
                }

                auto& pending = win_emu.process.pending_audio_render_events;
                auto event = pending.find(win_emu.current_thread().id);
                if (event == pending.end() && pending.size() == 1)
                {
                    event = pending.begin();
                }

                if (event == pending.end())
                {
                    if (!pending.empty())
                    {
                        win_emu.log.error("[audio-service] Cannot tell which of %zu pending render events belongs to stream %u\n",
                                          pending.size(), *id);
                    }
                    return;
                }

                stream->second.render->set_render_event(event->second);
                pending.erase(event);
            }

            void destroy_stream(windows_emulator& win_emu, const lpc_request_context& c)
            {
                const auto id = read_stream_id(win_emu, c);
                const auto stream = id ? this->streams_.find(*id) : this->streams_.end();
                if (stream == this->streams_.end())
                {
                    win_emu.log.warn("[audio-service] DestroyStream for an unknown stream\n");
                    return;
                }

                // An event the client registered for this stream but never started it with must not outlive it.
                win_emu.process.audio_render_events_by_stream.erase(stream->first);
                detach_render_stream(win_emu, stream->second);
                this->streams_.erase(stream);
            }

            // The WASAPI shared-mode mix format: a 44.1 kHz / 2-channel / 32-bit IEEE-float WAVEFORMATEXTENSIBLE, written as
            // an FC_CSTRUCT (18-byte WAVEFORMATEX base + cbSize-conformant tail) behind a unique pointer. 44100 Hz matches
            // the real captured device mix format; CreateRemoteStream reports the same nAvgBytesPerSec, so the stream
            // format the client negotiates stays self-consistent.
            static void write_mix_format(utils::aligned_binary_writer& writer)
            {
                constexpr uint32_t sample_rate = 44100;
                constexpr uint16_t channels = 2;
                constexpr uint16_t bits = 32;
                constexpr uint16_t block_align = channels * (bits / 8);
                constexpr uint16_t cb_size = 22; // WAVEFORMATEXTENSIBLE tail

                std::array<uint8_t, 18 + cb_size> fmt{};
                const auto put16 = [&](const size_t off, const uint16_t v) { std::memcpy(&fmt[off], &v, sizeof(v)); };
                const auto put32 = [&](const size_t off, const uint32_t v) { std::memcpy(&fmt[off], &v, sizeof(v)); };

                put16(0, 0xFFFE);                    // wFormatTag = WAVE_FORMAT_EXTENSIBLE
                put16(2, channels);                  // nChannels
                put32(4, sample_rate);               // nSamplesPerSec
                put32(8, sample_rate * block_align); // nAvgBytesPerSec
                put16(12, block_align);              // nBlockAlign
                put16(14, bits);                     // wBitsPerSample
                put16(16, cb_size);                  // cbSize
                put16(18, bits);                     // wValidBitsPerSample
                put32(20, 0x3);                      // dwChannelMask = FRONT_LEFT | FRONT_RIGHT
                // SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT {00000003-0000-0010-8000-00AA00389B71}
                static constexpr std::array<uint8_t, 16> ksdataformat_subtype_ieee_float = {0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
                                                                                            0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
                std::memcpy(&fmt[24], ksdataformat_subtype_ieee_float.data(), ksdataformat_subtype_ieee_float.size());

                writer.write_ndr_pointer(true);      // unique pointer referent
                writer.write_pointer_sized(cb_size); // conformance: length of the conformant tail
                writer.write(fmt.data(), fmt.size(), 1);
            }

            // {D574D111} opnum 0: AudioServerGetMixFormat(endpointId, VadServerSettings*, [out] WAVEFORMATEX**).
            static NTSTATUS handle_get_mix_format(utils::aligned_binary_writer& writer)
            {
                write_mix_format(writer);
                writer.align_to(sizeof(uint32_t));
                writer.write(k_hr_ok); // return HRESULT
                return STATUS_SUCCESS;
            }

            // {D574D111} opnum 1: AudioServerIsFormatSupported(endpointId, shareMode, mixParams, format,
            //   [in,out,unique] closestMatch). DirectSound probes its buffer format here before activating the
            //   render client; failing (or not answering) the call makes audioses map the RPC error to
            //   AUDCLNT_E_DEVICE_INVALIDATED. Like the native engine, accept a format the stream can play and suggest
            //   the mix format for any other: XAudio2 probes an incomplete format first and only moves on to a
            //   playable one when the engine declines it.
            static NTSTATUS handle_is_format_supported(windows_emulator& win_emu, const lpc_request_context& c,
                                                       utils::aligned_binary_writer& writer)
            {
                if (read_stream_audio_format(win_emu, c, writer.pointer_size(), k_is_format_supported_format_offsets))
                {
                    writer.write_ndr_pointer(false); // [out] closest-match format: null (format accepted as-is)
                    writer.align_to(sizeof(uint32_t));
                    writer.write(k_hr_ok);
                    return STATUS_SUCCESS;
                }

                writer.write_ndr_pointer(true); // closestMatch is a WAVEFORMATEX**: its outer referent comes first
                write_mix_format(writer);
                writer.align_to(sizeof(uint32_t));
                writer.write(k_hr_false);
                return STATUS_SUCCESS;
            }

            // {D574D111} opnum 2: AudioServerGetDevicePeriod(endpointId, mixParams, flags,
            //   [in,out,unique] *defaultPeriod, [in,out,unique] *minimumPeriod), both in 100-ns units. Report the
            //   standard shared-mode engine periods (10 ms default, 3 ms minimum).
            static NTSTATUS handle_get_device_period(utils::aligned_binary_writer& writer)
            {
                constexpr int64_t default_period = k_device_period_hns;
                constexpr int64_t minimum_period = k_device_minimum_period_hns;

                // Two [in,out,unique] hyper* out-params. Classic NDR (32-bit) flushes each top-level pointer's
                // pointee inline, right after its referent id (referent, then 8-aligned hyper); NDR64 (64-bit)
                // marshals all referent ids first and defers the pointees. Emit whichever the guest's transfer
                // syntax expects, or the client rejects the reply with E_INVALIDARG.
                if (writer.pointer_size() == utils::aligned_binary_writer::pointer_size_32)
                {
                    writer.write_ndr_pointer(true); // defaultPeriod referent
                    writer.write<int64_t>(default_period);
                    writer.write_ndr_pointer(true); // minimumPeriod referent
                    writer.write<int64_t>(minimum_period);
                }
                else
                {
                    writer.write_ndr_pointer(true); // defaultPeriod referent
                    writer.write_ndr_pointer(true); // minimumPeriod referent
                    writer.write<int64_t>(default_period);
                    writer.write<int64_t>(minimum_period);
                }
                writer.align_to(sizeof(uint32_t));
                writer.write(k_hr_ok);
                return STATUS_SUCCESS;
            }

            // {D574D111} opnum 4: the IAudioClient::Initialize "open stream" prep call.
            //   [in]  endpointId, sharemode, flags, WAVEFORMATEX*, GUID*, request-struct
            //   [out] LPWSTR*       (unique pointer, optional)
            //   [out] context_handle (the stream handle, reused as the binding for follow-on RPCs)
            //   returns HRESULT
            NTSTATUS handle_open_stream(windows_emulator& win_emu, const lpc_request_context& c, utils::aligned_binary_writer& writer)
            {
                const auto format = read_stream_audio_format(win_emu, c, writer.pointer_size(), k_open_stream_format_offsets);
                if (!format)
                {
                    win_emu.log.warn("[audio-service] Failed to read the RPC stream format or the requested format is unsupported\n");
                    return STATUS_INVALID_PARAMETER;
                }

                writer.write_ndr_pointer(true); // [out, string] p6: stream identifier
                writer.write_ndr_u16string(u"SogenAudioStream", true);
                writer.align_to(sizeof(uint32_t));

                const auto id = win_emu.process.next_audio_stream_id++;
                this->streams_[id].format = format;

                const auto context_uuid = make_stream_context_uuid(id);
                writer.write<uint32_t>(0);                                 // context handle: attributes
                writer.write(context_uuid.data(), context_uuid.size(), 1); // context handle: uuid

                writer.align_to(sizeof(uint32_t));
                writer.write(k_hr_ok); // return HRESULT
                return STATUS_SUCCESS;
            }

            // {D574D111} opnum 7: CreateRemoteStream. The [out] SYSTEM_AUDIO_STREAM wire is only 120 bytes (the
            // 1232-byte form seen in memory is bloated by host pointers): a session GUID, nAvgBytesPerSec, an
            // opaque server cookie (the client just hands it back in StartStream/StopStream, which we ignore), and a few
            // counts. The shared render buffer is NOT in the payload — it rides in as an ALPC HANDLE message
            // attribute. We back it with a pagefile section the guest can map and attach its handle. The wire
            // below was captured from a live Windows audio service (tools/alpc_capture.py).
            NTSTATUS handle_create_stream(windows_emulator& win_emu, const lpc_request_context& c, utils::aligned_binary_writer& writer,
                                          std::vector<alpc_reply_handle>& reply_handles)
            {
                const auto id = read_stream_id(win_emu, c);
                const auto entry = id ? this->streams_.find(*id) : this->streams_.end();
                if (entry == this->streams_.end() || !entry->second.format)
                {
                    return STATUS_INVALID_PARAMETER;
                }
                auto& stream = entry->second;
                const auto& format = *stream.format;

                // The client negotiates the render-buffer duration; the reply must describe a buffer of exactly
                // that size. The op7 [in] carries the requested duration (100-ns units) as a hyper right after the
                // 20-byte context handle + the 4-byte share-mode enum, i.e. at request offset 24. A fixed
                // one-second reply is accepted by a client that asked for one second (the audio-sample), but
                // audioses rejects it (DestroyStream) for a client that asked for a shorter buffer -- which is
                // what DirectSound/Miles negotiate. Size the buffer, the section, and every buffer-size field in
                // the control header + reply from the requested duration instead.
                uint64_t duration_hns = k_default_buffer_duration;
                if (c.send_buffer && c.send_buffer_length >= 32)
                {
                    const auto requested = win_emu.emu().read_memory<uint64_t>(c.send_buffer + 24);
                    if (requested != 0)
                    {
                        duration_hns = requested;
                    }
                }

                const auto sample_rate = static_cast<uint64_t>(format.host_format.sample_rate);
                constexpr auto duration_rounding = k_hns_per_second - 1;
                const auto maximum_duration = (std::numeric_limits<uint64_t>::max() - duration_rounding) / sample_rate;
                if (duration_hns > maximum_duration)
                {
                    return STATUS_INVALID_PARAMETER;
                }

                const auto buffer_frames = (sample_rate * duration_hns + duration_rounding) / k_hns_per_second;

                constexpr auto section_alignment_mask = uint32_t{0xFFF};
                constexpr auto maximum_buffer_extent = std::numeric_limits<uint32_t>::max() - section_alignment_mask;
                constexpr auto maximum_buffer_bytes = maximum_buffer_extent - k_render_data_offset;
                const auto maximum_buffer_frames = maximum_buffer_bytes / format.block_align;
                if (buffer_frames == 0 || buffer_frames > maximum_buffer_frames)
                {
                    return STATUS_INVALID_PARAMETER;
                }

                const auto buffer_bytes = static_cast<uint32_t>(buffer_frames * format.block_align);
                const uint32_t buffer_extent = k_render_data_offset + buffer_bytes; // control header + sample area
                const uint64_t render_section_size = (buffer_extent + section_alignment_mask) & ~uint64_t{section_alignment_mask};

                section render_section{};
                render_section.maximum_size = render_section_size;
                render_section.section_page_protection = PAGE_READWRITE;
                render_section.allocation_attributes = SEC_COMMIT;

                // The WASAPI shared-buffer control header audioses validates during Initialize. Captured from a
                // live audio service (tools/alpc_capture.py) for a 1-second buffer; the buffer-size fields (+0x04,
                // +0x170, +0x174) and the buffer duration (+0x154) are patched below to match this stream.
                std::array<uint8_t, 0x1c0> render_control_header = {
                    0x01, 0x00, 0x00, 0x00, 0x20, 0x66, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, // version, buffer size
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x44, 0x43, 0x50, 0x45, // "DCPE"
                    0xdc, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x96, 0x98, 0x00,
                    0x00, 0x00, 0x00, 0x00, // period 0x989680
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x80, 0x01, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00,
                    0x20, 0x66, 0x05, 0x00, 0x20, 0x66, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfe, 0xff, 0x02, 0x00,
                    0x44, 0xac, 0x00, 0x00, 0x20, 0x62, 0x05, 0x00, // WAVEFORMATEXTENSIBLE
                    0x08, 0x00, 0x20, 0x00, 0x16, 0x00, 0x20, 0x00, 0x03, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
                    0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                };

                const auto patch32 = [&](const size_t offset, const uint32_t value) {
                    std::memcpy(&render_control_header[offset], &value, sizeof(value));
                };
                patch32(0x04, buffer_extent);
                patch32(0x170, buffer_extent);
                patch32(0x174, buffer_extent);

                // The static control data (length at +0xCC) carries the name of the port SetEventHandle connects to, as
                // up to 64 UTF-16 code units at +0xD0 (audioses CCrossProcessBaseClientEndpoint::SetEventHandle).
                // Naming it per stream is what tells the engine which stream an event belongs to.
                constexpr size_t endpoint_name_offset = 0xD0;
                constexpr size_t endpoint_name_capacity = 64;
                static_assert(endpoint_name_offset + endpoint_name_capacity * sizeof(char16_t) <= 0x154);
                const auto endpoint_name = audio_endpoint_port_name(*id);
                if (endpoint_name.size() >= endpoint_name_capacity)
                {
                    return STATUS_INTERNAL_ERROR;
                }
                std::memcpy(&render_control_header[endpoint_name_offset], endpoint_name.data(), endpoint_name.size() * sizeof(char16_t));
                std::memcpy(&render_control_header[0x154], &duration_hns, sizeof(duration_hns));
                std::memcpy(&render_control_header[0x180], &format.wave_format, sizeof(format.wave_format));

                // Back the render section with a host-owned buffer aliased into the guest and drained by a host
                // thread (see render_stream). The guest maps this same memory as the shared audio buffer, so the
                // drain thread reads the committed PCM and advances the play cursor without any CPU-backend hooks.
                detach_render_stream(win_emu, stream);
                stream.render = std::make_unique<render_stream>(win_emu, this->sink_, *id, format, buffer_bytes, render_section_size,
                                                                render_control_header.data(), render_control_header.size());
                const auto backing = stream.render->guest_address();
                if (backing == 0)
                {
                    stream.render.reset();
                    return STATUS_NO_MEMORY;
                }
                render_section.backing_address = backing;

                const auto section_handle = win_emu.process.sections.store(std::move(render_section));
                stream.section = section_handle;

                // Deliver the render section. The op7 NDR references it as an sh_section handle (the _Struct_9
                // union arm). rpcrt4!LRPC_SYSTEM_HANDLE_DATA::GetSystemHandle requires the handle's ObjectType
                // to be a single-bit mask whose table lookup (rpcrt4 rva 0xECA50: bit->NdrSystemHandleResource)
                // matches the expected resource; bit 7 maps to Section (=6), so ObjectType must be 1<<7 = 0x80.
                // A zero/wrong ObjectType makes rpcrt4 __fastfail(FAST_FAIL_INVALID_ARG). Report SECTION access.
                constexpr uint32_t alpc_objtype_section = 0x80;
                constexpr uint32_t section_access = 0xF001F; // SECTION_ALL_ACCESS
                reply_handles.push_back(alpc_reply_handle{
                    .handle = section_handle.bits, .object_type = alpc_objtype_section, .desired_access = section_access});

                // The 120-byte op7 [out] _Struct_4 wire, replayed BYTE-FOR-BYTE from a live capture (see
                // tools/alpc_capture.py). Per audiosrv's decompiled IDL, AudioServerCreateStream returns a GUID
                // + nAvgBytesPerSec
                // + [system_handle(sh_file)] HANDLE (+0x18, null here) + i64 cookie (+0x20) + i64 (+0x28) +
                // three default _Struct_5 unions + a _Struct_9 union whose selector (+0x54 = 1) picks the
                // sh_section arm, and whose handle index (+0x58 = 1) references the delivered render section.
                // rpcrt4!Ndr64SystemHandleUnmarshall consumes 8 wire bytes per system handle and treats a
                // 1-based index (0 = null); Ndr64UnionUnmarshall reads the selectors. The earlier hand-typed
                // copy had these three values shifted 4 bytes early, which misaligned the union/handle parse
                // and tripped RPC_X_BYTE_COUNT_TOO_SMALL.
                std::array<uint8_t, 120> system_audio_stream = {
                    0x40, 0x37, 0x77, 0xcd, 0x87, 0xb1, 0x74, 0x49, 0xa1, 0xd5, 0xe0, 0xff, // session GUID
                    0x91, 0x37, 0x22, 0x77, 0x20, 0x62, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, // +0x10: render-buffer byte size
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0xd2, 0x14, 0x55, // +0x20: server cookie
                    0xd2, 0x01, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // +0x28: 0x18
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, // +0x50: 1
                    0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // +0x54 union sel=1, +0x58 handle idx=1
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                };
                std::memcpy(&system_audio_stream[0x10], &buffer_bytes, sizeof(buffer_bytes));
                writer.write(system_audio_stream.data(), system_audio_stream.size(), 1);
                return STATUS_SUCCESS;
            }

            // {D574D111} StartStream/StopStream/disconnect/destroy all reply with just an S_OK HRESULT (the
            // captured replies are 8 zero bytes of NDR).
            static NTSTATUS handle_post_create(utils::aligned_binary_writer& writer)
            {
                writer.write<uint32_t>(0);
                writer.write(k_hr_ok); // return HRESULT
                return STATUS_SUCCESS;
            }

            // {D574D111} opnum 6: AudioServerGetAudioSession([in] stream ctx, [out] session ctx). Right after
            // CreateRemoteStream the DirectSound client (unlike the WASAPI one) fetches the audio-session handle;
            // an unimplemented reply here aborts Initialize (DestroyStream + E_FAIL). Per audiosrv's decompiled
            // IDL the only [out] is the session context handle (a 4-byte attributes field + 16-byte UUID); the
            // NDR64 return HRESULT follows immediately. No reserved tail.
            static NTSTATUS handle_get_audio_session(utils::aligned_binary_writer& writer)
            {
                writer.write<uint32_t>(0); // context handle attributes
                writer.write(k_session_context_uuid.data(), k_session_context_uuid.size(), 1);
                writer.align_to(sizeof(uint32_t));
                writer.write(k_hr_ok); // return HRESULT
                return STATUS_SUCCESS;
            }

            // {D574D111} opnum 27: AudioSessionGetState([in, out] session ctx, [out] short* state). Per the IDL
            // the [in,out] session context handle is marshalled back (4-byte attributes + 16-byte UUID) followed
            // by the [out] short state, then the NDR64 return HRESULT. Report AudioSessionStateInactive (0).
            static NTSTATUS handle_get_session_state(utils::aligned_binary_writer& writer)
            {
                writer.write<uint32_t>(0); // context handle attributes
                writer.write(k_session_context_uuid.data(), k_session_context_uuid.size(), 1);
                writer.write<uint16_t>(0); // [out] AudioSessionState = AudioSessionStateInactive
                writer.align_to(sizeof(uint32_t));
                writer.write(k_hr_ok); // return HRESULT
                return STATUS_SUCCESS;
            }

            // {D574D111} opnum 122: AudioServerDeriveStreamCategory([in] short category, [in] int flags,
            // [out] int* derived). The client asks the engine to map the category it requested onto the one the
            // endpoint will actually use, which a real engine may downgrade by policy. We apply no policy, so echo
            // the request back. The [in] short sits at offset 0, ahead of its two padding bytes.
            static NTSTATUS handle_derive_stream_category(windows_emulator& win_emu, const lpc_request_context& c,
                                                          utils::aligned_binary_writer& writer)
            {
                uint16_t category = 0;
                if (c.send_buffer_length >= sizeof(category))
                {
                    category = win_emu.emu().read_memory<uint16_t>(c.send_buffer);
                }

                writer.write<uint32_t>(category); // [out] derived AUDIO_STREAM_CATEGORY
                writer.write(k_hr_ok);            // return HRESULT
                return STATUS_SUCCESS;
            }

            // {D574D111} opnum 55: AudioSessionDestroy([in, out] session ctx). The 32-bit DirectSound session
            // setup releases the session control it fetched via GetAudioSession/GetSessionState; leaving this
            // opnum unimplemented returns STATUS_NOT_SUPPORTED (-> HRESULT_FROM_WIN32 NOT_SUPPORTED) and aborts
            // Initialize. Marshal the [in,out] context handle back and report success.
            static NTSTATUS handle_destroy_session(utils::aligned_binary_writer& writer)
            {
                writer.write<uint32_t>(0); // context handle attributes
                writer.write(k_session_context_uuid.data(), k_session_context_uuid.size(), 1);
                writer.align_to(sizeof(uint32_t));
                writer.write(k_hr_ok); // return HRESULT
                return STATUS_SUCCESS;
            }

            // {923F85B3} opnum 0: HRESULT Proc0([out] byte** ppBlob) - no [in] params. mmdevapi's proxy calls
            // it early (MW3 retries it) and expects a unique pointer to a conformant byte blob (a serialized
            // property/state store; a live host returned ~584 bytes). We have no faithful blob to hand back, so
            // report an empty result: a null unique pointer + S_OK. This replaces the RPC_X error the caller got
            // from an unhandled opnum with a well-formed "nothing here" reply.
            static NTSTATUS handle_mmdev_get_blob(utils::aligned_binary_writer& writer)
            {
                writer.write_ndr_pointer(false); // [out] blob: null unique pointer
                writer.align_to(sizeof(uint32_t));
                writer.write(k_hr_ok);
                return STATUS_SUCCESS;
            }

            // opnum 25: IMMDeviceEnumerator::GetDefaultAudioEndpoint backend.
            //   [in]  EDataFlow data_flow (0 = eRender, 1 = eCapture)
            //   [in]  ERole role
            //   [out] LPWSTR* device_id   (unique pointer + conformant-varying wstring)
            //   [out] DWORD*  state
            //   returns HRESULT
            // NDR32 encodes these enums as adjacent 16-bit values, while NDR64 encodes each as a 32-bit value.
            static NTSTATUS handle_get_default_endpoint(windows_emulator& win_emu, const lpc_request_context& c,
                                                        utils::aligned_binary_writer& writer)
            {
                uint32_t data_flow = 0;
                if (c.send_buffer && writer.pointer_size() == utils::aligned_binary_writer::pointer_size_32 &&
                    c.send_buffer_length >= sizeof(uint16_t))
                {
                    data_flow = win_emu.emu().read_memory<uint16_t>(c.send_buffer);
                }
                else if (c.send_buffer && c.send_buffer_length >= sizeof(uint32_t))
                {
                    data_flow = win_emu.emu().read_memory<uint32_t>(c.send_buffer);
                }

                const auto found = find_default_endpoint_id(win_emu, data_flow);
                if (!found)
                {
                    return static_cast<NTSTATUS>(0x80070490); // ERROR_NOT_FOUND -> no default endpoint
                }
                const auto& id = *found;

                writer.write_ndr_pointer(true);
                writer.write_ndr_u16string(id, true);
                writer.align_to(sizeof(uint32_t));

                writer.write<uint32_t>(0); // [out] state

                writer.write(k_hr_ok); // return HRESULT
                return STATUS_SUCCESS;
            }
        };
    }

    namespace
    {
        constexpr std::u16string_view k_endpoint_port_prefix = u"\\RPC Control\\SogenAudioEndpoint-";
    }

    std::u16string audio_endpoint_port_name(const uint32_t stream_id)
    {
        const auto digits = std::to_string(stream_id);
        return std::u16string(k_endpoint_port_prefix) + std::u16string(digits.begin(), digits.end());
    }

    std::optional<uint32_t> parse_audio_endpoint_port_name(const std::u16string_view name)
    {
        if (!name.starts_with(k_endpoint_port_prefix))
        {
            return std::nullopt;
        }

        const auto digits = name.substr(k_endpoint_port_prefix.size());
        if (digits.empty() || digits.size() > 10 || (digits.size() > 1 && digits.front() == u'0'))
        {
            return std::nullopt;
        }

        uint64_t value = 0;
        for (const auto digit : digits)
        {
            if (digit < u'0' || digit > u'9')
            {
                return std::nullopt;
            }
            value = value * 10 + static_cast<uint64_t>(digit - u'0');
        }

        if (value == 0 || value > std::numeric_limits<uint32_t>::max())
        {
            return std::nullopt;
        }

        return static_cast<uint32_t>(value);
    }

    std::unique_ptr<port> create_audio_service_port()
    {
        return std::make_unique<audio_service_port>();
    }

} // namespace sogen
