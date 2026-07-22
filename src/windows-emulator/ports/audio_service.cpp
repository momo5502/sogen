#include "../std_include.hpp"
#include "audio_service.hpp"

#include "binary_writer.hpp"
#include "../windows_emulator.hpp"
#include "../registry/registry_utils.hpp"

#include <platform/unicode.hpp>
#include <utils/string.hpp>

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

        constexpr uint32_t k_audio_opnum_mmdev_get_blob = 0;        // {923F85B3} [out] serialized blob, no [in]
        constexpr uint32_t k_audio_opnum_get_default_endpoint = 25; // {923F85B3}
        constexpr uint32_t k_audio_opnum_get_mix_format = 0;        // {D574D111}
        constexpr uint32_t k_audio_opnum_is_format_supported = 1;   // {D574D111} AudioServerIsFormatSupported
        constexpr uint32_t k_audio_opnum_get_device_period = 2;     // {D574D111} AudioServerGetDevicePeriod
        constexpr uint32_t k_audio_opnum_open_stream = 4;           // {D574D111} (Initialize prep)
        constexpr uint32_t k_audio_opnum_get_audio_session = 6;     // {D574D111} AudioServerGetAudioSession
        constexpr uint32_t k_audio_opnum_create_stream = 7;         // {D574D111} CreateRemoteStream
        constexpr uint32_t k_audio_opnum_get_session_state = 27;    // {D574D111} AudioSessionGetState
        constexpr uint32_t k_audio_opnum_destroy_session = 55;      // {D574D111} AudioSessionDestroy
        constexpr uint32_t k_audio_opnum_destroy_stream = 13;       // {D574D111} AudioServerDestroyStream
        constexpr uint32_t k_audio_opnum_start_stream = 8;          // {D574D111} StartStream (IAudioClient::Start)
        constexpr uint32_t k_audio_opnum_stop_stream = 9;           // {D574D111} StopStream (IAudioClient::Stop)

        constexpr NTSTATUS k_hr_ok = 0;

        // An NDR [out] context handle: a 4-byte attributes field + a 16-byte UUID identifying the stream. The
        // client stores this and passes it back as the binding for the follow-on stream RPCs.
        constexpr std::array<uint8_t, 16> k_stream_context_uuid = {0x53, 0x6f, 0x67, 0x65, 0x6e, 0x41, 0x75, 0x64,
                                                                   0x69, 0x6f, 0x53, 0x74, 0x72, 0x6d, 0x00, 0x01};

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

            if (!first_active && !first_any)
            {
                win_emu.log.error("[audiosrv] no audio endpoint found for flow=%u\n", data_flow);
            }
            return first_active ? first_active : first_any;
        }

        std::string dump_hex(windows_emulator& win_emu, const emulator_pointer address, const ULONG length, const ULONG cap = 128)
        {
            const auto count = std::min<ULONG>(length, cap);
            if (!address || count == 0)
            {
                return {};
            }

            std::vector<std::byte> bytes(count, std::byte{});
            win_emu.emu().read_memory(address, bytes.data(), bytes.size());

            return utils::string::to_hex_string(bytes);
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

        // The shared-mode mix format reported by handle_get_mix_format: 44.1 kHz, 2 channels, 32-bit float.
        constexpr uint32_t k_sample_rate = 44100;
        constexpr uint32_t k_block_align = 8; // channels * bytes-per-sample
        constexpr uint64_t k_hns_per_second = 10000000;
        constexpr uint64_t k_default_buffer_duration = k_hns_per_second; // 1 s

        // How far the guest may run ahead of what the host device has actually played. It keeps the sink supplied
        // (the guest's own buffer is often only ~20 ms, far too thin a cushion on its own) while bounding total
        // latency to roughly this plus that buffer.
        constexpr uint64_t k_host_queue_cushion_ms = 60;
        constexpr uint64_t k_host_queue_cushion = k_sample_rate * k_block_align * k_host_queue_cushion_ms / 1000;

        // The engine period reported by handle_get_device_period, and the same span expressed in bytes. An
        // event-driven client is woken once per period consumed, so this also paces how often we signal it.
        constexpr int64_t k_device_period_hns = 100000; // 10 ms
        constexpr int64_t k_device_minimum_period_hns = 30000;
        constexpr auto k_device_period = std::chrono::microseconds{k_device_period_hns / 10};
        constexpr audio_format k_stream_format{.sample_rate = k_sample_rate, .channels = 2, .bits_per_sample = 32, .is_float = true};

        // Emulates the audio endpoint's hardware DMA. The render section handed to the guest is backed by a
        // host-owned buffer aliased into the guest address space (allocate_host_memory), so the guest and this
        // object share the exact same memory. A host thread drains that buffer at the device rate: it forwards
        // newly committed PCM to the host sink and advances the play cursor so the guest's render loop sees the
        // buffer emptying and keeps producing. Because the buffer is plain host memory, the thread touches it
        // directly -- no memory-access hooks (which the WHP backend does not honor) and no calls into the CPU
        // backend from a foreign thread (which is not thread-safe).
        class render_stream
        {
          public:
            render_stream(windows_emulator& win_emu, const uint32_t buffer_bytes, const uint64_t section_size,
                          const uint8_t* control_header, const size_t control_header_size)
                : win_emu_(win_emu),
                  buffer_bytes_(buffer_bytes),
                  section_size_(section_size),
                  host_storage_(static_cast<size_t>(section_size) + k_audio_page_size)
            {
                this->host_ptr_ = reinterpret_cast<uint8_t*>(
                    (reinterpret_cast<uintptr_t>(this->host_storage_.data()) + (k_audio_page_size - 1)) & ~(k_audio_page_size - 1));
                std::memcpy(this->host_ptr_, control_header, control_header_size);

                this->guest_address_ = win_emu.memory.find_free_allocation_base(static_cast<size_t>(section_size));
                if (this->guest_address_ == 0 ||
                    !win_emu.memory.allocate_host_memory(this->guest_address_, static_cast<size_t>(section_size), this->host_ptr_,
                                                         memory_permission::read_write))
                {
                    this->guest_address_ = 0;
                    return;
                }

                this->thread_ = std::thread(&render_stream::run, this);
            }

            ~render_stream()
            {
                this->stop_ = true;
                if (this->thread_.joinable())
                {
                    this->thread_.join();
                }

                if (this->guest_address_)
                {
                    this->win_emu_.audio().stop();
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

          private:
            static constexpr size_t k_audio_page_size = 0x1000;

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
                uint64_t submitted = 0;
                bool tried_start = false;
                bool has_device = false;
                std::chrono::steady_clock::time_point anchor{};
                std::chrono::steady_clock::time_point last_signal{};

                while (!this->stop_)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));

                    const auto write = this->read_cursor(k_write_cursor_offset);
                    if (write == 0)
                    {
                        continue;
                    }

                    if (!tried_start)
                    {
                        has_device = this->win_emu_.audio().start(k_stream_format);
                        tried_start = true;
                        anchor = std::chrono::steady_clock::now();
                    }

                    uint64_t play{};

                    if (has_device)
                    {
                        for (uint64_t pos = submitted; pos < write;)
                        {
                            const uint64_t ring_offset = pos % this->buffer_bytes_;
                            const uint64_t chunk = std::min<uint64_t>(write - pos, this->buffer_bytes_ - ring_offset);
                            this->win_emu_.audio().submit(this->host_ptr_ + k_render_data_offset + ring_offset, static_cast<size_t>(chunk));
                            pos += chunk;
                        }

                        // Advance the play cursor so the guest sees genuine backpressure. Prefer deriving it from
                        // what the device actually played (everything submitted minus what is still queued) plus a
                        // fixed cushion: a wall-clock estimate drifts ahead of the device clock, so the host queue
                        // grows without bound and latency creeps up to seconds the longer playback runs. The
                        // cushion keeps the sink from starving, since the guest's own buffer is far too small to
                        // serve as one. Fall back to the clock when the sink cannot report its queue.
                        uint64_t played{};
                        if (const auto queued = this->win_emu_.audio().queued_bytes())
                        {
                            const auto consumed = write > *queued ? write - *queued : 0;
                            played = consumed + k_host_queue_cushion;
                        }
                        else
                        {
                            const auto elapsed_ns =
                                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - anchor).count();
                            played =
                                static_cast<uint64_t>(k_sample_rate) * k_block_align * static_cast<uint64_t>(elapsed_ns) / k_ns_per_second;
                        }
                        play = std::min(played, write);
                    }
                    else
                    {
                        // No host audio device (e.g. headless / CI): keep the play cursor level with the write
                        // cursor so the guest's render loop never blocks waiting for a buffer that nothing drains.
                        play = write;
                    }

                    this->write_play_cursor(play);

                    // Wake an EVENTCALLBACK client: it fills one buffer, then waits on its render event for the
                    // engine to report an elapsed period before writing more. Pace that to one wake per period,
                    // like a real engine -- firing on every poll tick only wakes the client to find no new space,
                    // and each wake costs a guest thread dispatch. Drive it from the clock rather than from how far
                    // the play cursor moved: the cursor never passes the write cursor, so a client that wakes
                    // without writing would stall it and never be woken again.
                    const auto now = std::chrono::steady_clock::now();
                    const auto render_event = this->win_emu_.process.audio_render_event.load(std::memory_order_relaxed);
                    if (render_event && now - last_signal >= k_device_period &&
                        this->win_emu_.try_signal_guest_event(make_handle(render_event)))
                    {
                        last_signal = now;
                    }

                    submitted = write;
                }
            }

            static constexpr uint64_t k_ns_per_second = 1'000'000'000ULL;

            windows_emulator& win_emu_;
            uint32_t buffer_bytes_{};
            uint64_t section_size_{};
            uint64_t guest_address_{0};
            std::vector<uint8_t> host_storage_;
            uint8_t* host_ptr_{nullptr};
            std::atomic<bool> stop_{false};
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
                        return handle_is_format_supported(writer);
                    case k_audio_opnum_get_device_period:
                        return handle_get_device_period(writer);
                    case k_audio_opnum_destroy_stream:
                        this->render_stream_.reset();
                        return handle_post_create(writer);
                    case k_audio_opnum_open_stream:
                        return handle_open_stream(writer);
                    case k_audio_opnum_get_audio_session:
                        return handle_get_audio_session(writer);
                    case k_audio_opnum_get_session_state:
                        return handle_get_session_state(writer);
                    case k_audio_opnum_destroy_session:
                        return handle_destroy_session(writer);
                    case k_audio_opnum_create_stream:
                        return handle_create_stream(win_emu, c, writer, reply_handles);
                    case k_audio_opnum_start_stream:
                        return handle_post_create(writer);
                    case k_audio_opnum_stop_stream:
                        return handle_post_create(writer);
                    case 5:
                        return handle_post_create(writer);
                    default:
                        return log_unhandled(win_emu, "AudioClient", procedure_id, c);
                    }
                }

                switch (procedure_id)
                {
                case k_audio_opnum_mmdev_get_blob:
                    return handle_mmdev_get_blob(writer);
                case k_audio_opnum_get_default_endpoint:
                    return handle_get_default_endpoint(win_emu, c, writer);
                default:
                    return log_unhandled(win_emu, "MMDevEnum", procedure_id, c);
                }
            }

          private:
            std::unique_ptr<render_stream> render_stream_{};

            static NTSTATUS log_unhandled(windows_emulator& win_emu, const char* iface, const uint32_t opnum, const lpc_request_context& c)
            {
                win_emu.log.error("[audiosrv] UNHANDLED %s opnum=%u send_len=%u recv_len=%u req: %s\n", iface, opnum, c.send_buffer_length,
                                  c.recv_buffer_length, dump_hex(win_emu, c.send_buffer, c.send_buffer_length).c_str());
                return STATUS_NOT_SUPPORTED;
            }

            // {D574D111} opnum 0: AudioServerGetMixFormat(endpointId, VadServerSettings*, [out] WAVEFORMATEX**).
            // The [out] format is an FC_CSTRUCT (18-byte WAVEFORMATEX base + cbSize-conformant tail) behind a
            // unique pointer. The WASAPI shared-mode mix format is a WAVEFORMATEXTENSIBLE IEEE-float format;
            // report 48 kHz / 2-channel / 32-bit float.
            static NTSTATUS handle_get_mix_format(utils::aligned_binary_writer& writer)
            {
                // 44100 Hz matches the real captured device mix format; CreateRemoteStream reports the same
                // nAvgBytesPerSec, so the stream format the client negotiates stays self-consistent.
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

                writer.align_to(sizeof(uint32_t));
                writer.write(k_hr_ok); // return HRESULT
                return STATUS_SUCCESS;
            }

            // {D574D111} opnum 1: AudioServerIsFormatSupported(endpointId, shareMode, mixParams, format,
            //   [in,out,unique] closestMatch). DirectSound probes its buffer format here before activating the
            //   render client; failing (or not answering) the call makes audioses map the RPC error to
            //   AUDCLNT_E_DEVICE_INVALIDATED. The emulated shared-mode engine accepts the requested format, so
            //   report S_OK with no suggested closest match.
            static NTSTATUS handle_is_format_supported(utils::aligned_binary_writer& writer)
            {
                writer.write_ndr_pointer(false); // [out] closest-match format: null (format accepted as-is)
                writer.align_to(sizeof(uint32_t));
                writer.write(k_hr_ok); // return S_OK
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
            static NTSTATUS handle_open_stream(utils::aligned_binary_writer& writer)
            {
                writer.write_ndr_pointer(true); // [out, string] p6: stream identifier
                writer.write_ndr_u16string(u"SogenAudioStream", true);
                writer.align_to(sizeof(uint32_t));

                writer.write<uint32_t>(0);                                                   // context handle: attributes
                writer.write(k_stream_context_uuid.data(), k_stream_context_uuid.size(), 1); // context handle: uuid

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

                const uint64_t buffer_frames = (k_sample_rate * duration_hns + k_hns_per_second - 1) / k_hns_per_second;
                const auto buffer_bytes = static_cast<uint32_t>(buffer_frames * k_block_align);
                const uint32_t buffer_extent = k_render_data_offset + buffer_bytes; // control header + sample area
                const uint64_t render_section_size = (buffer_extent + 0xFFF) & ~uint64_t{0xFFF};

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
                std::memcpy(&render_control_header[0x154], &duration_hns, sizeof(duration_hns));

                // Back the render section with a host-owned buffer aliased into the guest and drained by a host
                // thread (see render_stream). The guest maps this same memory as the shared audio buffer, so the
                // drain thread reads the committed PCM and advances the play cursor without any CPU-backend hooks.
                this->render_stream_ = std::make_unique<render_stream>(win_emu, buffer_bytes, render_section_size,
                                                                       render_control_header.data(), render_control_header.size());
                const auto backing = this->render_stream_->guest_address();
                if (backing == 0)
                {
                    this->render_stream_.reset();
                    return STATUS_NO_MEMORY;
                }
                render_section.backing_address = backing;

                const auto section_handle = win_emu.process.sections.store(std::move(render_section));

                // Deliver the render section. The op7 NDR references it as an sh_section handle (the _Struct_9
                // union arm). rpcrt4!LRPC_SYSTEM_HANDLE_DATA::GetSystemHandle requires the handle's ObjectType
                // to be a single-bit mask whose table lookup (rpcrt4 rva 0xECA50: bit->NdrSystemHandleResource)
                // matches the expected resource; bit 7 maps to Section (=6), so ObjectType must be 1<<7 = 0x80.
                // A zero/wrong ObjectType makes rpcrt4 __fastfail(FAST_FAIL_INVALID_ARG). Report SECTION access.
                constexpr uint32_t alpc_objtype_section = 0x80;
                constexpr uint32_t section_access = 0xF001F; // SECTION_ALL_ACCESS
                reply_handles.push_back(alpc_reply_handle{
                    .handle = section_handle.bits, .object_type = alpc_objtype_section, .desired_access = section_access});

                // The 120-byte op7 [out] _Struct_4 wire, replayed BYTE-FOR-BYTE from a live capture
                // (build/.../capture/alpc_reply_27_op7_len144.bin, NDR payload @ +0x18). Decompiled IDL
                // (docs/audio-capture/audioclient_rpc_idl.txt, AudioServerCreateStream): GUID + nAvgBytesPerSec
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
            // an unimplemented reply here aborts Initialize (DestroyStream + E_FAIL). Per the decompiled IDL
            // (docs/audio-capture/audioclient_rpc_idl.txt) the only [out] is the session context handle (a 4-byte
            // attributes field + 16-byte UUID); the NDR64 return HRESULT follows immediately. No reserved tail.
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
            //   [in]  DWORD data_flow (0 = eRender, 1 = eCapture)  [+ role]
            //   [out] LPWSTR* device_id   (unique pointer + conformant-varying wstring)
            //   [out] DWORD*  state
            //   returns HRESULT
            static NTSTATUS handle_get_default_endpoint(windows_emulator& win_emu, const lpc_request_context& c,
                                                        utils::aligned_binary_writer& writer)
            {
                uint32_t data_flow = 0;
                if (c.send_buffer && c.send_buffer_length >= sizeof(uint32_t))
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

    std::unique_ptr<port> create_audio_service_port()
    {
        return std::make_unique<audio_service_port>();
    }

} // namespace sogen
