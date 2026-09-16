#include "../std_include.hpp"
#include "mig_kernel_servers.hpp"

#include "../macos_emulator.hpp"

#include <chrono>

#include <algorithm>
#include <array>

namespace sogen::mach
{
    namespace
    {
        std::vector<uint8_t> port_reply(mach_port_namespace& ports, const mig_request& request, const port_name_t name,
                                        const kern_return_t code)
        {
            if (name == PORT_NULL)
            {
                return make_mig_error_bytes(request, code);
            }

            mig_reply_builder builder{request.call, ports};
            builder.append_port_descriptor({.name = name, .disposition = disposition::make_send, .type = descriptor_type::port});
            return builder.finish();
        }

        // host_info is libpthread's first Mach message and is fatal when stubbed. Every field comes from
        // macos_system_info, which the commpage and sysctl also read -- a guest cross-checking hw.ncpu
        // against host_info must not see a contradiction.
        std::vector<uint8_t> host_info_routine(macos_emulator& emu, const mig_request& request)
        {
            if (!request.has_ndr())
            {
                return make_mig_error_bytes(request, mig_error::bad_arguments);
            }

            // libpthread asks for this one before anything else runs and treats a refusal as fatal --
            // it crashes with MIG_TYPE_ERROR rather than degrading. The values are measured from this
            // host; they are a property of the kernel's scheduler, not of any particular machine.
            if (request.arg_u32(0) == flavor::host_priority_info)
            {
                constexpr std::array<uint32_t, flavor::host_priority_info_count> priorities{80, 80, 64, 31, 0, 0, 0, 79};
                const auto wanted = std::min<uint32_t>(request.arg_u32(1), flavor::host_priority_info_count);

                mig_reply_builder priority_reply{request.call, emu.mach.ports};
                priority_reply.append_ndr();
                priority_reply.append_u32(static_cast<uint32_t>(kr::success));
                priority_reply.append_u32(wanted);
                for (uint32_t i = 0; i < wanted; ++i)
                {
                    priority_reply.append_u32(priorities.at(i));
                }

                return priority_reply.finish();
            }

            if (request.arg_u32(0) != flavor::host_basic_info)
            {
                return make_mig_error_bytes(request, kr::invalid_argument);
            }

            const auto& system = emu.system_info;

            std::array<uint32_t, flavor::host_basic_info_count> info{};
            info[0] = system.ncpus;
            info[1] = system.active_cpus;
            info[2] = static_cast<uint32_t>(std::min<uint64_t>(system.memory_size, 0xFFFFFFFFull));
            info[3] = CPU_TYPE_ARM64;
            info[4] = CPU_SUBTYPE_ARM64E;
            info[5] = 0;
            info[6] = system.physical_cpus;
            info[7] = system.physical_cpus;
            info[8] = system.logical_cpus;
            info[9] = system.logical_cpus;
            info[10] = static_cast<uint32_t>(system.memory_size & 0xFFFFFFFFull);
            info[11] = static_cast<uint32_t>(system.memory_size >> 32);

            const auto count = std::min<uint32_t>(request.arg_u32(1), flavor::host_basic_info_count);

            mig_reply_builder builder{request.call, emu.mach.ports};
            builder.append_ndr();
            builder.append_u32(static_cast<uint32_t>(kr::success));
            builder.append_u32(count);
            for (uint32_t i = 0; i < count; ++i)
            {
                builder.append_u32(info.at(i));
            }

            return builder.finish();
        }

        std::vector<uint8_t> host_get_clock_service_routine(macos_emulator& emu, const mig_request& request)
        {
            if (!request.has_ndr())
            {
                return make_mig_error_bytes(request, mig_error::bad_arguments);
            }

            return port_reply(emu.mach.ports, request, emu.mach.clock_service(request.arg_u32(0)), kr::failure);
        }

        // host_get_io_master (mach_host.defs 205), host_get_io_main_port in newer headers. Everything
        // IOKit does is MIG to this one port, so refusing it does not keep IOKit out of the run -- it
        // sends to MACH_PORT_NULL instead and then waits forever for the reply. Handing out a real port
        // is what turns that hang into a named unimplemented-routine report per IOKit call.
        std::vector<uint8_t> host_get_io_master_routine(macos_emulator& emu, const mig_request& request)
        {
            return port_reply(emu.mach.ports, request, emu.mach.io_master_port(), kr::failure);
        }

        // mach/clock.defs. CoreFoundation and libdispatch call clock_get_time on the ports
        // host_get_clock_service hands out, and libtrace prints "clock_get_time() failed" once per call
        // when it is refused -- which is how this gap surfaced.
        //
        // SYSTEM_CLOCK (0) counts uptime, CALENDAR_CLOCK (1) counts the epoch. The port carries which one
        // it is in its kernel-object id, so the two never share an answer.
        std::vector<uint8_t> clock_get_time_routine(macos_emulator& emu, const mig_request& request)
        {
            constexpr uint32_t CALENDAR_CLOCK = 1;
            constexpr uint64_t NSEC_PER_SECOND = 1000000000ULL;

            const auto clock_id = emu.mach.ports.object_of(request.call.header.remote_port).id;

            uint64_t nanoseconds = 0;
            if (clock_id == CALENDAR_CLOCK)
            {
                const auto epoch = std::chrono::system_clock::now().time_since_epoch();
                nanoseconds = static_cast<uint64_t>(std::max<int64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(epoch).count(), 0));
            }
            else
            {
                const auto frequency = emu.emu().read_system_register(3, 3, 14, 0, 0);
                const auto counter = emu.emu().read_system_register(3, 3, 14, 0, 2);
                nanoseconds = frequency == 0 ? 0 : (counter / frequency) * NSEC_PER_SECOND + ((counter % frequency) * NSEC_PER_SECOND) / frequency;
            }

            // mach_timespec_t is two 32-bit words, unlike the 64-bit struct timespec the BSD side uses.
            mig_reply_builder builder{request.call, emu.mach.ports};
            builder.append_ndr();
            builder.append_u32(kr::success);
            builder.append_u32(static_cast<uint32_t>(nanoseconds / NSEC_PER_SECOND));
            builder.append_u32(static_cast<uint32_t>(nanoseconds % NSEC_PER_SECOND));
            return builder.finish();
        }

        // The resolution of the clock above, in the same two-word form. Measured on this host: both
        // clocks report a one-nanosecond resolution.
        std::vector<uint8_t> clock_get_attributes_routine(macos_emulator& emu, const mig_request& request)
        {
            constexpr uint32_t CLOCK_GET_TIME_RES = 1;

            if (!request.has_ndr())
            {
                return make_mig_error_bytes(request, mig_error::bad_arguments);
            }

            const auto flavor = request.arg_u32(0);
            if (flavor != CLOCK_GET_TIME_RES)
            {
                return make_mig_error_bytes(request, kr::invalid_value);
            }

            mig_reply_builder builder{request.call, emu.mach.ports};
            builder.append_ndr();
            builder.append_u32(kr::success);
            builder.append_u32(1);
            builder.append_u32(1);
            return builder.finish();
        }

        std::vector<uint8_t> host_get_special_port_routine(macos_emulator& emu, const mig_request& request)
        {
            if (!request.has_ndr())
            {
                return make_mig_error_bytes(request, mig_error::bad_arguments);
            }

            return port_reply(emu.mach.ports, request, emu.mach.get_host_special_port(static_cast<int32_t>(request.arg_u32(1))),
                              kr::failure);
        }
    }

    void register_host_routines(mig_server_table& table)
    {
        table.register_routine(kernel_object_kind::host, 200, host_info_routine, "host_info");
        table.register_routine(kernel_object_kind::clock, 1000, clock_get_time_routine, "clock_get_time");
        table.register_routine(kernel_object_kind::clock, 1001, clock_get_attributes_routine, "clock_get_attributes");
        table.register_routine(kernel_object_kind::host, 205, host_get_io_master_routine, "host_get_io_master");
        table.register_routine(kernel_object_kind::host, 206, host_get_clock_service_routine, "host_get_clock_service");
        table.register_routine(kernel_object_kind::host, 412, host_get_special_port_routine, "host_get_special_port");
    }
}
