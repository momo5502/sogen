#include "../std_include.hpp"
#include "macos_process_manager_routines.hpp"

#include "../macos_emulator.hpp"
#include "../macos_platform.hpp"

#include <array>
#include <optional>
#include <set>
#include <string_view>

namespace sogen
{
    namespace
    {
        // Carbon OSStatus values, from MacErrors.h.
        constexpr int32_t NO_ERR = 0;
        constexpr int32_t PARAM_ERR = -50;
        constexpr int32_t PROC_NOT_FOUND = -600;

        // ProcessSerialNumber.lowLongOfPSN for kCurrentProcess, from MacTypes.h.
        constexpr uint32_t CURRENT_PROCESS = 2;

        void ret_status(const macos_native_call& call, const int32_t status)
        {
            call.ret(static_cast<uint64_t>(static_cast<uint32_t>(status)));
        }

        void report_once(const macos_native_call& call, const char* routine)
        {
            static std::set<std::string_view> reported{};
            if (reported.emplace(routine).second)
            {
                call.emu_ref.log.info("Process Manager: %s answered by sogen\n", routine);
            }
        }

        bool write_serial_number(const macos_native_call& call, const uint64_t address)
        {
            const std::array<uint32_t, 2> psn{MACOS_PROCESS_SERIAL_NUMBER_HIGH, MACOS_PROCESS_SERIAL_NUMBER_LOW};
            return call.emu_ref.memory.try_write_memory(address, psn.data(), sizeof(psn));
        }

        // GetCurrentProcess runs _RegisterApplication behind a dispatch_once, and _RegisterApplication
        // registers the process with coreservicesd over MIG 10050. sogen runs no coreservicesd, and the
        // registration is not optional to its caller: it calls abort() from inside the once block when
        // the round trip fails. Answering the Process Manager surface directly is what keeps that whole
        // path off a daemon that does not exist.
        void get_current_process(const macos_native_call& call)
        {
            report_once(call, "GetCurrentProcess");

            if (call.arg(0) == 0)
            {
                ret_status(call, PARAM_ERR);
                return;
            }

            ret_status(call, write_serial_number(call, call.arg(0)) ? NO_ERR : PARAM_ERR);
        }

        void register_application(const macos_native_call& call)
        {
            report_once(call, "_RegisterApplication");
            ret_status(call, NO_ERR);
        }

        void get_process_for_pid(const macos_native_call& call)
        {
            const auto pid = static_cast<int32_t>(call.arg(0));
            const auto out = call.arg(1);

            if (out == 0)
            {
                ret_status(call, PARAM_ERR);
                return;
            }

            if (pid != static_cast<int32_t>(call.emu_ref.process.pid))
            {
                ret_status(call, PROC_NOT_FOUND);
                return;
            }

            ret_status(call, write_serial_number(call, out) ? NO_ERR : PARAM_ERR);
        }

        // The emulated session has exactly one process and no Dock, so it is always frontmost and its
        // type never changes anything observable.
        void accept_process_request(const macos_native_call& call)
        {
            ret_status(call, NO_ERR);
        }

        // -[NSApplication init] decides on this call whether the application comes up active. At
        // init+1688 (25G76 AppKit) it sends -_isActiveApp, which is _NXIsActiveApp: GetFrontProcess()
        // followed by SameProcess(front, kCurrentProcess), and only a yes reaches -setIsActive:YES.
        // GetFrontProcess is _LSCopyFrontApplication and SameProcess is _LSCompareASNsLong, so both
        // answers come from lsd. sogen runs no lsd: the copy is null, GetFrontProcess falls through to
        // procNotFound, and every GUI guest comes up believing it is in the background -- Calculator
        // hands its three traffic-light layers rgb(35,35,35) instead of the lit colours.
        void get_front_process(const macos_native_call& call)
        {
            report_once(call, "GetFrontProcess");

            if (call.arg(0) == 0)
            {
                ret_status(call, PARAM_ERR);
                return;
            }

            ret_status(call, write_serial_number(call, call.arg(0)) ? NO_ERR : PARAM_ERR);
        }

        std::optional<std::array<uint32_t, 2>> read_serial_number(const macos_native_call& call, const uint64_t address)
        {
            std::array<uint32_t, 2> psn{};
            if (address == 0 || !call.emu_ref.memory.try_read_memory(address, psn.data(), sizeof(psn)))
            {
                return std::nullopt;
            }

            // kCurrentProcess is an alias, not a number: LaunchServices resolves {0, 2} to the caller's
            // own serial number before comparing, which is how _NXIsActiveApp asks "is the front process
            // me" without ever naming itself.
            if (psn[0] == 0 && psn[1] == CURRENT_PROCESS)
            {
                return std::array<uint32_t, 2>{MACOS_PROCESS_SERIAL_NUMBER_HIGH, MACOS_PROCESS_SERIAL_NUMBER_LOW};
            }

            return psn;
        }

        void same_process(const macos_native_call& call)
        {
            report_once(call, "SameProcess");

            const auto out = call.arg(2);
            const auto first = read_serial_number(call, call.arg(0));
            const auto second = read_serial_number(call, call.arg(1));

            uint8_t same = 0;
            if (!first || !second)
            {
                call.emu_ref.memory.try_write_memory(out, &same, sizeof(same));
                ret_status(call, PARAM_ERR);
                return;
            }

            same = *first == *second ? 1 : 0;
            ret_status(call, call.emu_ref.memory.try_write_memory(out, &same, sizeof(same)) ? NO_ERR : PARAM_ERR);
        }
    }

    void register_process_manager_routines(macos_native_dispatch& dispatch)
    {
        const std::string hi_services{MACOS_HI_SERVICES_IMAGE_PATH};

        dispatch.register_routine(hi_services, "_GetCurrentProcess", get_current_process);
        dispatch.register_routine(hi_services, "__RegisterApplication", register_application);
        dispatch.register_routine(hi_services, "_GetProcessForPID", get_process_for_pid);
        dispatch.register_routine(hi_services, "_TransformProcessType", accept_process_request);
        dispatch.register_routine(hi_services, "_SetFrontProcess", accept_process_request);
        dispatch.register_routine(hi_services, "_SetFrontProcessWithOptions", accept_process_request);
        dispatch.register_routine(hi_services, "_GetFrontProcess", get_front_process);
        dispatch.register_routine(hi_services, "_SameProcess", same_process);
    }
}
