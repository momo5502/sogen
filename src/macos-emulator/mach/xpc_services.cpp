#include "../std_include.hpp"
#include "xpc_services.hpp"

#include "../macos_emulator.hpp"
#include "mig_kernel_servers.hpp"
#include "nsxpc_reply.hpp"
#include "xpc_bootstrap.hpp"

#include <set>
#include <utility>

namespace sogen::xpc
{
    namespace
    {
        // notify_ipc (Libnotify notify_ipc.defs; subsystem base 1000 with two leading skips) reaches the
        // notification-center service port as plain MIG alongside the XPC dictionaries. libnotify's
        // client setup must complete before the guest's TCC path proceeds: notify_client.c turns a
        // failed checkin into NOTIFY_STATUS_SERVER_CHECKIN_FAILED and the guest stalls before the
        // connection's own check-in. The answers below are notifyd's measured replies (2026-08-27,
        // cgsdemo under lldb on the host); registrations are accepted and nothing is ever delivered
        // from them -- sogen runs no notification poster.
        constexpr int32_t NOTIFY_CHECK = 0x3ea;                // 1002 check
        constexpr int32_t NOTIFY_REGISTER_CHECK = 0x3f4;       // 1012 register_check_2
        constexpr int32_t NOTIFY_GENERATE_COMMON_PORT = 0x401; // 1025 generate_common_port
        constexpr int32_t NOTIFY_CHECKIN = 0x3ff;              // 1023 checkin
        constexpr int32_t NOTIFY_FILTERED_CHECKIN = 0x404;     // 1028 filtered_checkin

        std::vector<uint8_t> notify_checkin_reply(macos_emulator& emu, const mach::msg_call& call)
        {
            // Measured body: NDR, a zero word, version 3 (notify_client.c rejects anything below
            // NOTIFY_IPC_VERSION_MIN_SUPPORTED), the server's pid (416 was notifyd's on the measured
            // host; the client only compares it across regeneration events), status OK.
            mach::mig_reply_builder builder{call, emu.mach.ports};
            builder.append_ndr();
            builder.append_u32(0);
            builder.append_u32(3);
            builder.append_u32(416);
            builder.append_u32(0);
            return builder.finish();
        }

        std::vector<uint8_t> notify_register_check_reply(macos_emulator& emu, const mach::msg_call& call)
        {
            // Measured body: NDR, a zero word, a slot, a running name id, status OK, pad.
            //
            // The measured host reply named a real shared-memory slot, and notify.c then maps notifyd's
            // segment and reads the name's counter straight out of it. sogen has no POSIX shared memory
            // namespace, so shm_open("apple.shm.notification_center") failed, shm_attach() gave up and
            // every notify_register_check() in the guest reported NOTIFY_STATUS_FAILED -- measured
            // 2026-08-30 against clickalert and TextEdit. -1 is notifyd's own answer for "no slot for
            // this name": notify.c then leaves the segment alone and asks the server on every check
            // (routine 1002) instead.
            static uint64_t next_name_id = 1;

            mach::mig_reply_builder builder{call, emu.mach.ports};
            builder.append_ndr();
            builder.append_u32(0);
            builder.append_u32(0xffffffff);
            builder.append_u64(next_name_id++);
            builder.append_u32(0);
            builder.append_u32(0);
            return builder.finish();
        }

        std::vector<uint8_t> notify_check_reply(macos_emulator& emu, const mach::msg_call& call)
        {
            // NDR, a zero word, the changed flag, status OK. Nothing in sogen posts a notification, so
            // the honest answer to "did this name change since I last asked" is no, forever.
            mach::mig_reply_builder builder{call, emu.mach.ports};
            builder.append_ndr();
            builder.append_u32(0);
            builder.append_u32(0);
            builder.append_u32(0);
            return builder.finish();
        }

        std::vector<uint8_t> notify_generate_common_port_reply(macos_emulator& emu, const mach::msg_call& call)
        {
            // Measured: complex reply carrying a receive right for the fresh common port, then NDR and
            // status OK. The port is real -- the guest registers it (register_common_port, a
            // simpleroutine) and would receive notifications on it; none are ever posted.
            const auto common = emu.mach.ports.allocate_receive_right();

            mach::mig_reply_builder builder{call, emu.mach.ports};
            builder.append_port_descriptor(
                {.name = common, .disposition = mach::disposition::move_receive, .type = mach::descriptor_type::port});
            builder.append_ndr();
            builder.append_u32(0);
            return builder.finish();
        }
    }

    std::optional<xpc_service_decision> decide_xpc_service(const std::string_view name)
    {
        // sogen runs no daemons, so no lookup can ever produce a real one. It can still produce a live
        // connection whose every message is refused, and that is what a client copes with best:
        // CoreFoundation falls back to reading preferences from disk, TCC reads as denied, and libxpc's
        // own error path stays out of it. A *failed* lookup is the harsher answer -- libxpc leaves the
        // connection with no send right, and a synchronous send on it waits for a reply the client will
        // never generate -- so it is the exception, not the default, and every name that is not
        // measured to need it gets a port.
        //
        // The exceptions are measured, and in each the client treats only a failed lookup as terminal:
        // libtrace's os_log dies with MIG_REPLY_MISMATCH in the firehose push against a live-but-dead
        // logd port, but runs its no-logd degradation when the logd lookup fails; and
        // -[RBSConnection _handshake] retries a refused handshake exactly 1000 times and then throws
        // NSInternalInconsistencyException("BUG IN RUNNINGBOARD 1000 RunningBoard handshakes failed"),
        // which kills the process -- measured 2026-08-28 under sogen, where a failed lookup instead lets
        // an AppKit app carry on into its run loop.
        static constexpr std::string_view lookup_must_fail[] = {
            "com.apple.logd",
            "com.apple.runningboard",
        };

        // The one name sogen serves for real. HIToolbox's _CheckEventsInited builds its whole event
        // machinery on CGWindowServerCFMachPort, which is CGSLookupServerPort plus CFMachPortCreateWithPort;
        // CGSLookupServerPort looks this name up and sends 29010 GetSessionPort to what comes back. A
        // failed lookup makes CGWindowServerCFMachPort return NULL, _CheckEventsInited bail before
        // INIT_AppleEvents, and the app never post NSApplicationDidFinishLaunchingNotification --
        // measured 2026-08-28 against src/tools/macos-gui-probe/appkitwin.
        static constexpr std::string_view window_server = "com.apple.windowserver.active";

        if (name.empty())
        {
            return std::nullopt;
        }

        if (name == window_server)
        {
            return xpc_service_decision::window_server;
        }

        for (const auto service : lookup_must_fail)
        {
            if (name == service)
            {
                return std::nullopt;
            }
        }

        return xpc_service_decision::refuse_connection_invalid;
    }

    std::vector<uint8_t> refused_service_reply_body()
    {
        // Measured 2026-08-27: tccd answers a message it does not understand with an empty dictionary,
        // and libxpc delivers a wire dictionary as-is -- no dictionary a daemon sends can arrive as an
        // XPC_TYPE_ERROR, those objects are fabricated by the client side of libxpc.
        return serialize(make_dictionary({}));
    }

    std::optional<std::vector<uint8_t>> nsxpc_refusal_body(macos_emulator& emu, const std::span<const uint8_t> body)
    {
        // NSXPC rides on the same wire dictionary as any other XPC message, and an empty one is not an
        // answer it can decode: -[NSXPCConnection _decodeAndInvokeReplyBlockWithEvent:] finds no "root"
        // and reports a connection error, which the client retries. A reply block whose arguments are all
        // nil is decodable, and it is the truth -- sogen runs no daemon, so the call returned nothing.
        // Measured 2026-08-28: LaunchServices answers a nil store with
        // "store (null) or url (null) was nil" and degrades after ten tries, and appkitwin then puts a
        // window on screen; against the empty dictionary it never stops retrying.
        uint32_t unsupported_tag = 0;
        const auto request = parse(body, &unsupported_tag);
        if (!request.has_value())
        {
            if (unsupported_tag != 0)
            {
                static std::set<uint32_t> reported{};
                if (reported.insert(unsupported_tag).second)
                {
                    emu.log.warn("unimplemented XPC value tag 0x%x in a message to an xpc_service port\n", unsupported_tag);
                }
            }

            return std::nullopt;
        }

        const auto* signature = find(*request, "replysig");
        if (signature == nullptr || signature->type_tag != type::string)
        {
            return std::nullopt;
        }

        std::string unsupported{};
        const auto root = nsxpc::empty_reply_root(signature->text, &unsupported);
        if (!root.has_value())
        {
            static std::set<std::string> reported{};
            if (reported.insert(unsupported).second)
            {
                emu.log.warn("unimplemented NSXPC reply argument type '%s' in reply signature '%s'\n", unsupported.c_str(),
                             signature->text.c_str());
            }

            return std::nullopt;
        }

        static std::set<std::string> reported_selectors{};
        const auto* invocation = find(*request, "root");
        const auto selector = invocation != nullptr ? nsxpc::invocation_selector(invocation->bytes) : std::nullopt;
        if (reported_selectors.insert(selector.value_or(signature->text)).second)
        {
            emu.log.info("NSXPC %s answered with a nil reply (%s)\n", selector.value_or("<unnamed>").c_str(), signature->text.c_str());
        }

        return serialize(make_dictionary({{"root", make_data(*root)}}));
    }

    std::vector<uint8_t> answer_service_message(macos_emulator& emu, const mach::msg_call& call, const std::span<const uint8_t> body)
    {
        // 'w00t' is a connection's check-in: it carries no dictionary, only the receive right of the
        // connection's channel port, handed to the daemon. From then on the channel is the daemon's to
        // answer, exactly like the service port it was reached through.
        constexpr int32_t XPC_CONNECTION_CHECKIN = 0x77303074;

        // Measured 2026-08-28 on the host (lldb over appkitwin, correlating each libxpc send API with the
        // msgh_id the mach_msg2 under it carries): libxpc stamps 0x10000000 on every ASYNCHRONOUS send --
        // xpc_connection_send_message with no reply port, and xpc_connection_send_message_with_reply with
        // a fresh send-once reply port per call -- and 0x40000000 only on
        // xpc_connection_send_message_with_reply_sync. The answer to the asynchronous form goes to that
        // send-once port and libdispatch merges it through the unote it registered for it; sogen has no
        // path for that, and the answer it queued on the connection's channel instead was never drained.
        // It only ever surfaced as "queued with no waiter", and when a waiter did drain the channel it
        // killed the guest with "BUG IN CLIENT OF LIBDISPATCH: Reply received on unexpected port".
        constexpr int32_t XPC_ASYNC_MESSAGE = 0x10000000;
        if (call.header.id == XPC_ASYNC_MESSAGE)
        {
            if (call.header.local_port != mach::PORT_NULL)
            {
                static bool reported = false;
                if (!std::exchange(reported, true))
                {
                    emu.log.warn("an asynchronous XPC send carries a send-once reply port sogen cannot answer on; "
                                 "its reply handler never runs\n");
                }
            }

            return {};
        }

        if (call.header.id == XPC_CONNECTION_CHECKIN)
        {
            const auto request = mach::make_mig_request(call, body, mach::kernel_object_kind::xpc_service);
            for (size_t i = 0; const auto descriptor = request.descriptor(i); ++i)
            {
                if (descriptor->type != mach::descriptor_type::port || descriptor->disposition != mach::disposition::move_receive)
                {
                    continue;
                }

                if (auto* entry = emu.mach.ports.find(descriptor->name);
                    entry != nullptr && entry->object.kind == mach::kernel_object_kind::none)
                {
                    entry->object.kind = mach::kernel_object_kind::xpc_service;
                    emu.log.info("XPC connection adopted channel port 0x%x\n", descriptor->name);
                }
            }

            // A check-in is send-only: it hands the daemon a right and expects nothing back. Answering
            // it leaves a reply queued on the service port that nobody will ever receive, and every
            // connection the guest opens adds one.
            return {};
        }
        else if (body.size() < 4 || mach::read_u32(body, 0) != MAGIC)
        {
            // Two protocols reach a service port: XPC dictionaries (od's getpwuid, a TCC preflight) and
            // plain MIG (notifyd's checkin family on the notification-center port, logd's firehose). A
            // MIG routine sogen does not implement gets the same named bad-id it would get on any other
            // port kind.
            switch (call.header.id)
            {
            case NOTIFY_CHECKIN:
            case NOTIFY_FILTERED_CHECKIN:
                return notify_checkin_reply(emu, call);
            case NOTIFY_REGISTER_CHECK:
                return notify_register_check_reply(emu, call);
            case NOTIFY_CHECK:
                return notify_check_reply(emu, call);
            case NOTIFY_GENERATE_COMMON_PORT:
                return notify_generate_common_port_reply(emu, call);
            default:
                break;
            }

            static std::set<int32_t> reported{};
            if (reported.insert(call.header.id).second)
            {
                emu.log.warn("unimplemented MIG routine %d on an xpc_service port\n", call.header.id);
            }

            return mach::make_mig_error_reply(call, mach::mig_error::bad_id).bytes;
        }

        mach::mig_reply_builder builder{call, emu.mach.ports};
        builder.append_bytes(nsxpc_refusal_body(emu, body).value_or(refused_service_reply_body()));
        return builder.finish_with_id(XPC_REPLY_ID);
    }
}
