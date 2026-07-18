#include "../std_include.hpp"
#include "audio_service.hpp"

#include "binary_writer.hpp"
#include "../windows_emulator.hpp"
#include "../registry/registry_utils.hpp"

#include <platform/unicode.hpp>

namespace sogen
{

    namespace
    {
        // The audio RPC is split across two ALPC ports with overlapping opnums: \RPC Control\Audiosrv hosts the
        // AudioEndpointBuilder / MMDevice-enumeration interface (GetDefaultAudioEndpoint), while
        // \RPC Control\AudioClientRpc hosts the IAudioClient streaming interface (audioses). The concrete RPC
        // interface GUIDs differ across Windows builds (e.g. {A3BE171F} v1.6 and {98B2C141} v2.8 on build 20348),
        // so we dispatch by the bound port name rather than by a hardcoded interface GUID.
        constexpr uint32_t k_audio_opnum_get_default_endpoint = 25; // Audiosrv
        constexpr uint32_t k_audio_opnum_get_mix_format = 0;        // AudioClientRpc
        constexpr uint32_t k_audio_opnum_is_format_supported = 1;   // AudioClientRpc AudioServerIsFormatSupported
        constexpr uint32_t k_audio_opnum_get_device_period = 2;     // AudioClientRpc AudioServerGetDevicePeriod
        constexpr uint32_t k_audio_opnum_open_stream = 4;           // AudioClientRpc (Initialize prep)
        constexpr uint32_t k_audio_opnum_create_stream = 7;         // AudioClientRpc CreateRemoteStream
        constexpr uint32_t k_audio_opnum_destroy_stream = 13;       // AudioClientRpc AudioServerDestroyStream
        constexpr uint32_t k_audio_opnum_post_create_a = 8;         // AudioClientRpc post-CreateRemoteStream (returns S_OK)
        constexpr uint32_t k_audio_opnum_post_create_b = 9;         // AudioClientRpc post-CreateRemoteStream (returns S_OK)

        constexpr NTSTATUS k_hr_ok = 0;

        // An NDR [out] context handle: a 4-byte attributes field + a 16-byte UUID identifying the stream. The
        // client stores this and passes it back as the binding for the follow-on stream RPCs.
        constexpr std::array<uint8_t, 16> k_stream_context_uuid = {0x53, 0x6f, 0x67, 0x65, 0x6e, 0x41, 0x75, 0x64,
                                                                   0x69, 0x6f, 0x53, 0x74, 0x72, 0x6d, 0x00, 0x01};

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

            std::vector<uint8_t> bytes(count, 0);
            win_emu.emu().read_memory(address, bytes.data(), bytes.size());

            std::string hex;
            hex.reserve(static_cast<size_t>(count) * 3);
            std::array<char, 4> tmp{};
            for (const auto b : bytes)
            {
                (void)snprintf(tmp.data(), tmp.size(), "%02x ", b);
                hex += tmp.data();
            }
            return hex;
        }

        struct audio_service_port : rpc_port
        {
            explicit audio_service_port(const bool is_audio_client)
                : is_audio_client_(is_audio_client)
            {
            }

            NTSTATUS handle_rpc(windows_emulator& win_emu, const uint32_t procedure_id, const lpc_request_context& c,
                                utils::aligned_binary_writer& writer, std::vector<alpc_reply_handle>& reply_handles) override
            {
                const auto& iface = this->bound_interface();
                if (getenv("EMULATOR_LOG_RPC"))
                {
                    win_emu.log.error("[audiosrv] call iface=%02x%02x%02x%02x opnum=%u send=%u\n", iface[0], iface[1], iface[2], iface[3],
                                      procedure_id, static_cast<uint32_t>(c.send_buffer_length));
                }
                // DEBUG-AUDIO-NDR: dump full request bytes for the AudioClient streaming opnums under scrutiny.
                if (getenv("EMULATOR_LOG_RPC") && this->is_audio_client_ &&
                    (procedure_id == 0 || procedure_id == 2 || procedure_id == 4 || procedure_id == 5 || procedure_id == 7))
                {
                    win_emu.log.error("[audiosrv-dbg] REQ opnum=%u send_len=%u bytes: %s\n", procedure_id,
                                      static_cast<uint32_t>(c.send_buffer_length),
                                      dump_hex(win_emu, c.send_buffer, c.send_buffer_length, 256).c_str());
                }

                if (this->is_audio_client_)
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
                        return handle_post_create(writer);
                    case k_audio_opnum_open_stream:
                        return handle_open_stream(writer);
                    case k_audio_opnum_create_stream:
                        return handle_create_stream(win_emu, writer, reply_handles);
                    case 5:
                    case k_audio_opnum_post_create_a:
                    case k_audio_opnum_post_create_b:
                        return handle_post_create(writer);
                    default:
                        return log_unhandled(win_emu, "AudioClient", procedure_id, c);
                    }
                }

                if (procedure_id == k_audio_opnum_get_default_endpoint)
                {
                    return handle_get_default_endpoint(win_emu, c, writer);
                }

                return log_unhandled(win_emu, "MMDevEnum", procedure_id, c);
            }

          private:
            // True when this port instance serves \RPC Control\AudioClientRpc (the IAudioClient streaming
            // interface); false for the Audiosrv / AudioSrvServiceRpc endpoint-enumeration interface.
            bool is_audio_client_;

            static NTSTATUS log_unhandled(windows_emulator& win_emu, const char* iface, const uint32_t opnum, const lpc_request_context& c)
            {
                win_emu.log.error("[audiosrv] UNHANDLED %s opnum=%u send_len=%u recv_len=%u req: %s\n", iface, opnum,
                                  static_cast<uint32_t>(c.send_buffer_length), static_cast<uint32_t>(c.recv_buffer_length),
                                  dump_hex(win_emu, c.send_buffer, c.send_buffer_length).c_str());
                return STATUS_NOT_SUPPORTED;
            }

            // {D574D111} opnum 0: AudioServerGetMixFormat(endpointId, VadServerSettings*, [out] WAVEFORMATEX**).
            // The [out] format is an FC_CSTRUCT (18-byte WAVEFORMATEX base + cbSize-conformant tail) behind a
            // unique pointer. The WASAPI shared-mode mix format is a WAVEFORMATEXTENSIBLE IEEE-float format;
            // report 44.1 kHz / 2-channel / 32-bit float.
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
                constexpr int64_t default_period = 100000; // 10 ms
                constexpr int64_t minimum_period = 30000;  // 3 ms
                writer.write_ndr_pointer(true);            // defaultPeriod referent
                writer.write_ndr_pointer(true);            // minimumPeriod referent
                writer.write<int64_t>(default_period);
                writer.write<int64_t>(minimum_period);
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
                writer.write_ndr_pointer(false); // [out] string: null

                writer.write<uint32_t>(0);                                                   // context handle: attributes
                writer.write(k_stream_context_uuid.data(), k_stream_context_uuid.size(), 1); // context handle: uuid

                writer.align_to(sizeof(uint32_t));
                writer.write(k_hr_ok); // return HRESULT
                return STATUS_SUCCESS;
            }

            // {D574D111} opnum 7: CreateRemoteStream. The [out] SYSTEM_AUDIO_STREAM wire is only 120 bytes (the
            // 1232-byte form seen in memory is bloated by host pointers): a session GUID, nAvgBytesPerSec, an
            // opaque server cookie (the client just hands it back in opnums 8/9, which we ignore), and a few
            // counts. The shared render buffer is NOT in the payload — it rides in as an ALPC HANDLE message
            // attribute. We back it with a pagefile section the guest can map and attach its handle. The wire
            // below was captured from a live Windows audio service (tools/alpc_capture.py).
            static NTSTATUS handle_create_stream(windows_emulator& win_emu, utils::aligned_binary_writer& writer,
                                                 std::vector<alpc_reply_handle>& reply_handles)
            {
                // The shared render buffer the client maps: 0x57000 bytes (one second of 44100 Hz / 2ch /
                // 32-bit float, page-aligned), prefixed by a WASAPI shared-buffer control header that audioses
                // validates during Initialize. The header (buffer size, "DCPE" magic, period 0x989680, and the
                // WAVEFORMATEXTENSIBLE) was captured from a live audio service (tools/alpc_capture.py); the rest
                // of the buffer stays silent. We pre-allocate the section backing and write the header so the
                // client's mapping reads it.
                constexpr uint64_t render_section_size = 0x57000;
                section render_section{};
                render_section.maximum_size = render_section_size;
                render_section.section_page_protection = PAGE_READWRITE;
                render_section.allocation_attributes = SEC_COMMIT;

                static constexpr std::array<uint8_t, 0x1c0> render_control_header = {
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

                const auto backing = win_emu.memory.allocate_memory(static_cast<size_t>(render_section_size), memory_permission::read_write,
                                                                    false, 0, memory_region_kind::pagefile_section_view);
                if (backing)
                {
                    win_emu.emu().write_memory(backing, render_control_header.data(), render_control_header.size());
                    render_section.backing_address = backing;
                }

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
                static constexpr std::array<uint8_t, 120> system_audio_stream = {
                    0x40, 0x37, 0x77, 0xcd, 0x87, 0xb1, 0x74, 0x49, 0xa1, 0xd5, 0xe0, 0xff, // session GUID
                    0x91, 0x37, 0x22, 0x77, 0x20, 0x62, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, // nAvgBytesPerSec=0x56220
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0xd2, 0x14, 0x55, // +0x20: server cookie
                    0xd2, 0x01, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // +0x28: 0x18
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, // +0x50: 1
                    0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // +0x54 union sel=1, +0x58 handle idx=1
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                };
                writer.write(system_audio_stream.data(), system_audio_stream.size(), 1);
                return STATUS_SUCCESS;
            }

            // {D574D111} opnums 8 and 9: the post-CreateRemoteStream finalize calls. Each returns just an
            // S_OK HRESULT (the captured replies are 8 zero bytes of NDR).
            static NTSTATUS handle_post_create(utils::aligned_binary_writer& writer)
            {
                writer.write<uint32_t>(0);
                writer.write(k_hr_ok); // return HRESULT
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

                writer.write<uint32_t>(1); // [out] state -- DEVICE_STATE_ACTIVE, matching the endpoint
                                           // find_default_endpoint_id just selected on (state == 1)

                writer.write(k_hr_ok); // return HRESULT
                return STATUS_SUCCESS;
            }
        };
    }

    std::unique_ptr<port> create_audio_service_port(const std::u16string_view port_name)
    {
        return std::make_unique<audio_service_port>(port_name == u"\\RPC Control\\AudioClientRpc");
    }

} // namespace sogen
