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
        // The Audiosrv ALPC port hosts several RPC interfaces whose opnums overlap, so we dispatch by the
        // interface the client bound to.
        //   {923F85B3-BBEE-4EDF-8059-F569FA64A027} v1.6 = MMDevice enumeration (mmdevapi).
        //   {D574D111-6126-49D7-9B86-4DE6B650D4FC} v2.8 = "AudioClientRpc" / IAudioClient streaming (audioses).
        constexpr std::array<uint8_t, 16> k_iface_mmdevice_enum = {0xb3, 0x85, 0x3f, 0x92, 0xee, 0xbb, 0xdf, 0x4e,
                                                                   0x80, 0x59, 0xf5, 0x69, 0xfa, 0x64, 0xa0, 0x27};
        constexpr std::array<uint8_t, 16> k_iface_audio_client = {0x11, 0xd1, 0x74, 0xd5, 0x26, 0x61, 0xd7, 0x49,
                                                                  0x9b, 0x86, 0x4d, 0xe6, 0xb6, 0x50, 0xd4, 0xfc};

        constexpr uint32_t k_audio_opnum_get_default_endpoint = 25; // {923F85B3}
        constexpr uint32_t k_audio_opnum_get_mix_format = 0;        // {D574D111}

        constexpr NTSTATUS k_hr_ok = 0;

        // The audio endpoint database lives in the registry under
        // HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\{Render,Capture}. Each endpoint is a
        // sub-key named by its endpoint id ("{0.0.<flow>.00000000}.{<guid>}"). mmdevapi resolves a device by
        // opening that registry key, so GetDefaultEndpoint must return an id that actually exists there.
        std::optional<std::u16string> find_default_endpoint_id(windows_emulator& win_emu, const uint32_t data_flow)
        {
            const std::filesystem::path render =
                R"(\Registry\Machine\Software\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render)";
            const std::filesystem::path capture =
                R"(\Registry\Machine\Software\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture)";

            const auto key = win_emu.registry.get_key(utils::path_key{data_flow == 0 ? render : capture});
            if (!key)
            {
                win_emu.log.error("[audiosrv] MMDevices key missing for flow=%u\n", data_flow);
                return std::nullopt;
            }

            std::optional<std::u16string> first_active{};
            std::optional<std::u16string> first_any{};
            for (size_t i = 0;; ++i)
            {
                const auto name = win_emu.registry.get_sub_key_name(*key, i);
                if (!name)
                {
                    break;
                }

                const std::string name_str(*name);
                std::u16string id(name_str.begin(), name_str.end());
                const auto sub_key = win_emu.registry.get_key(utils::path_key{(data_flow == 0 ? render : capture) / name_str});
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
            char tmp[4];
            for (const auto b : bytes)
            {
                (void)snprintf(tmp, sizeof(tmp), "%02x ", b);
                hex += tmp;
            }
            return hex;
        }

        struct audio_service_port : rpc_port
        {
            NTSTATUS handle_rpc(windows_emulator& win_emu, const uint32_t procedure_id, const lpc_request_context& c,
                                utils::aligned_binary_writer& writer) override
            {
                const auto& iface = this->bound_interface();
                if (getenv("EMULATOR_LOG_RPC"))
                {
                    win_emu.log.error("[audiosrv] call iface=%02x%02x%02x%02x opnum=%u send=%u\n", iface[0], iface[1], iface[2],
                                      iface[3], procedure_id, c.send_buffer_length);
                }

                if (iface == k_iface_audio_client)
                {
                    switch (procedure_id)
                    {
                    case k_audio_opnum_get_mix_format:
                        return handle_get_mix_format(writer);
                    default:
                        return log_unhandled(win_emu, "AudioClient", procedure_id, c);
                    }
                }

                switch (procedure_id)
                {
                case k_audio_opnum_get_default_endpoint:
                    return handle_get_default_endpoint(win_emu, c, writer);
                default:
                    return log_unhandled(win_emu, "MMDevEnum", procedure_id, c);
                }
            }

          private:
            static NTSTATUS log_unhandled(windows_emulator& win_emu, const char* iface, const uint32_t opnum,
                                          const lpc_request_context& c)
            {
                win_emu.log.error("[audiosrv] UNHANDLED %s opnum=%u send_len=%u recv_len=%u req: %s\n", iface, opnum,
                                  c.send_buffer_length, c.recv_buffer_length,
                                  dump_hex(win_emu, c.send_buffer, c.send_buffer_length).c_str());
                return STATUS_NOT_SUPPORTED;
            }

            // {D574D111} opnum 0: AudioServerGetMixFormat(endpointId, VadServerSettings*, [out] WAVEFORMATEX**).
            // The [out] format is an FC_CSTRUCT (18-byte WAVEFORMATEX base + cbSize-conformant tail) behind a
            // unique pointer. The WASAPI shared-mode mix format is a WAVEFORMATEXTENSIBLE IEEE-float format;
            // report 48 kHz / 2-channel / 32-bit float.
            static NTSTATUS handle_get_mix_format(utils::aligned_binary_writer& writer)
            {
                constexpr uint32_t sample_rate = 48000;
                constexpr uint16_t channels = 2;
                constexpr uint16_t bits = 32;
                constexpr uint16_t block_align = channels * (bits / 8);
                constexpr uint16_t cb_size = 22; // WAVEFORMATEXTENSIBLE tail

                std::array<uint8_t, 18 + cb_size> fmt{};
                const auto put16 = [&](const size_t off, const uint16_t v) { std::memcpy(&fmt[off], &v, sizeof(v)); };
                const auto put32 = [&](const size_t off, const uint32_t v) { std::memcpy(&fmt[off], &v, sizeof(v)); };

                put16(0, 0xFFFE);                       // wFormatTag = WAVE_FORMAT_EXTENSIBLE
                put16(2, channels);                     // nChannels
                put32(4, sample_rate);                  // nSamplesPerSec
                put32(8, sample_rate * block_align);    // nAvgBytesPerSec
                put16(12, block_align);                 // nBlockAlign
                put16(14, bits);                        // wBitsPerSample
                put16(16, cb_size);                     // cbSize
                put16(18, bits);                        // wValidBitsPerSample
                put32(20, 0x3);                         // dwChannelMask = FRONT_LEFT | FRONT_RIGHT
                // SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT {00000003-0000-0010-8000-00AA00389B71}
                static constexpr std::array<uint8_t, 16> ksdataformat_subtype_ieee_float = {
                    0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
                std::memcpy(&fmt[24], ksdataformat_subtype_ieee_float.data(), ksdataformat_subtype_ieee_float.size());

                writer.write_ndr_pointer(true);          // unique pointer referent
                writer.write_pointer_sized(cb_size);     // conformance: length of the conformant tail
                writer.write(fmt.data(), fmt.size(), 1);

                writer.align_to(sizeof(uint32_t));
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
