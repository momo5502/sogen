#include "../std_include.hpp"
#include "service_control.hpp"

#include "binary_writer.hpp"
#include "../windows_emulator.hpp"

namespace sogen
{

    namespace
    {
        // The \RPC Control\ntsvcs ALPC port hosts several RPC interfaces. The one that matters here is svcctl
        // (the Service Control Manager), {367ABB81-9844-35F1-AD32-98F038001003}: while creating a render audio
        // client, mmdevapi opens the "AudioSrv" service through it (OpenSCManagerW -> OpenServiceW ->
        // SubscribeServiceChangeNotifications) so it can react to the audio service restarting. The same port
        // also carries the PnP / Plug-and-Play interface (CM_* config-manager notifications); those callers
        // tolerate an empty success reply, which is the fallback below.
        constexpr std::array<uint8_t, 16> k_iface_svcctl = {0x81, 0xbb, 0x7a, 0x36, 0x44, 0x98, 0xf1, 0x35,
                                                            0xad, 0x32, 0x98, 0xf0, 0x38, 0x00, 0x10, 0x03};

        // svcctl opnums. NOTE: this Windows build's svcctl client uses opnum numbers shifted from the classic
        // MS-SCMR table - observed on the wire: ROpenSCManagerW arrives as opnum 64 (its [in] is the
        // "ServicesActive" database string + access, with no input handle). Follow-on opnums are discovered the
        // same way; the classic numbers are kept as fallbacks.
        constexpr uint32_t k_svcctl_close_handle = 0;           // RCloseServiceHandle ([in,out] handle)
        constexpr uint32_t k_svcctl_open_service_w = 16;        // ROpenServiceW (SCM handle + name + access)
        constexpr uint32_t k_svcctl_subscribe = 55;             // SubscribeServiceChangeNotifications (handle + mask)
        constexpr uint32_t k_svcctl_open_sc_manager_w = 64;     // ROpenSCManagerW (database string + access)

        constexpr uint32_t k_error_success = 0;

        // Fabricated SC_RPC_HANDLEs. The SCM is fully stubbed, so the bytes only need to round-trip: the client
        // stores the handle from Open* and hands it back on the follow-on Open/Close/Notify calls.
        constexpr std::array<uint8_t, 16> k_scm_handle_uuid = {0x53, 0x6f, 0x67, 0x65, 0x6e, 0x53, 0x43, 0x4d,
                                                               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
        constexpr std::array<uint8_t, 16> k_service_handle_uuid = {0x53, 0x6f, 0x67, 0x65, 0x6e, 0x53, 0x76, 0x63,
                                                                   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02};
        constexpr std::array<uint8_t, 16> k_null_handle_uuid = {};

        // An NDR [out] context handle: a 4-byte attributes field followed by the 16-byte handle UUID.
        void write_context_handle(utils::aligned_binary_writer& writer, const std::array<uint8_t, 16>& uuid)
        {
            writer.write<uint32_t>(0); // context handle attributes
            writer.write(uuid.data(), uuid.size(), 1);
        }

        void write_return(utils::aligned_binary_writer& writer, const uint32_t win32_error)
        {
            writer.align_to(sizeof(uint32_t));
            writer.write<uint32_t>(win32_error);
        }

        struct service_control_port : rpc_port
        {
            NTSTATUS handle_rpc(windows_emulator& win_emu, const uint32_t procedure_id, const lpc_request_context& c,
                                utils::aligned_binary_writer& writer, std::vector<alpc_reply_handle>& /*reply_handles*/) override
            {
                if (this->bound_interface() != k_iface_svcctl)
                {
                    // PnP / other ntsvcs interfaces (e.g. CM_Register_Notification): an empty success reply is
                    // enough for the current callers to continue.
                    return STATUS_SUCCESS;
                }

                if (getenv("EMULATOR_LOG_RPC"))
                {
                    win_emu.log.error("[svcctl] opnum=%u send=%u\n", procedure_id, c.send_buffer_length);
                }

                switch (procedure_id)
                {
                case k_svcctl_open_sc_manager_w:
                    write_context_handle(writer, k_scm_handle_uuid);
                    write_return(writer, k_error_success);
                    return STATUS_SUCCESS;

                case k_svcctl_open_service_w:
                    write_context_handle(writer, k_service_handle_uuid);
                    write_return(writer, k_error_success);
                    return STATUS_SUCCESS;

                case k_svcctl_subscribe:
                    // I_ScQueryServiceConfig, the worker behind SubscribeServiceChangeNotifications. Its [out]
                    // is (classic NDR) a non-encapsulated UNION discriminated by the change-mask; the selected
                    // arm is a unique pointer to a struct carrying a WNF state name, which the client then
                    // subscribes to via RtlSubscribeWnfStateChangeNotification. Marshalling that nested
                    // union/pointer/struct (and modelling the service-state WNF) is not implemented yet, so the
                    // subscribe currently fails and mmdevapi aborts the render-client setup here. The
                    // OpenSCManager / OpenService / CloseServiceHandle handlers above already round-trip
                    // correctly. TODO: marshal the union arm to finish DirectSound device creation.
                    win_emu.log.error("[svcctl] SubscribeServiceChangeNotifications ([out] union) not implemented\n");
                    return STATUS_NOT_SUPPORTED;

                case k_svcctl_close_handle:
                    // [in, out] SC_RPC_HANDLE: the client expects the handle nulled out on a successful close.
                    write_context_handle(writer, k_null_handle_uuid);
                    write_return(writer, k_error_success);
                    return STATUS_SUCCESS;

                default:
                {
                    std::string hex;
                    const auto count = std::min<ULONG>(c.send_buffer_length, 64);
                    std::vector<uint8_t> bytes(count, 0);
                    if (count)
                    {
                        win_emu.emu().read_memory(c.send_buffer, bytes.data(), bytes.size());
                    }
                    char tmp[4];
                    for (const auto b : bytes)
                    {
                        (void)snprintf(tmp, sizeof(tmp), "%02x ", b);
                        hex += tmp;
                    }
                    win_emu.log.error("[svcctl] UNHANDLED opnum=%u send_len=%u in: %s\n", procedure_id,
                                      c.send_buffer_length, hex.c_str());
                    return STATUS_NOT_SUPPORTED;
                }
                }
            }
        };
    }

    std::unique_ptr<port> create_service_control_port()
    {
        return std::make_unique<service_control_port>();
    }

} // namespace sogen
