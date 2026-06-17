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
        // Windows Audio Service (Audiosrv) LRPC interface {923F85B3-BBEE-4EDF-8059-F569FA64A027} v1.6,
        // as called by the real mmdevapi.dll. Opnums reverse-engineered from mmdevapi's MIDL client stubs.
        constexpr uint32_t k_audio_opnum_get_default_endpoint = 25;

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
                switch (procedure_id)
                {
                case k_audio_opnum_get_default_endpoint:
                    return handle_get_default_endpoint(win_emu, c, writer);
                default:
                    win_emu.log.error("[audiosrv] UNHANDLED opnum=%u send_len=%u recv_len=%u req: %s\n", procedure_id,
                                      c.send_buffer_length, c.recv_buffer_length,
                                      dump_hex(win_emu, c.send_buffer, c.send_buffer_length).c_str());
                    return STATUS_NOT_SUPPORTED;
                }
            }

          private:
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
