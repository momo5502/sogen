#include "../std_include.hpp"
#include "mig_kernel_servers.hpp"

#include <set>

#include "../macos_emulator.hpp"
#include "mach_exception.hpp"

#include <algorithm>
#include <array>

namespace sogen::mach
{
    namespace
    {
        std::optional<exception_handler_entry> first_handler_in_mask(const macos_emulator& emu, const uint32_t mask)
        {
            for (uint32_t type = 0; type < 32; ++type)
            {
                if ((mask & (1u << type)) != 0)
                {
                    if (auto handler = emu.mach.exceptions.find_handler(type); handler.has_value())
                    {
                        return handler;
                    }
                }
            }

            return std::nullopt;
        }

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

        std::vector<uint8_t> counted_reply(mach_port_namespace& ports, const mig_request& request, const std::span<const uint32_t> words,
                                           const uint32_t requested)
        {
            const auto count = std::min<uint32_t>(requested, static_cast<uint32_t>(words.size()));

            mig_reply_builder builder{request.call, ports};
            builder.append_ndr();
            builder.append_u32(static_cast<uint32_t>(kr::success));
            builder.append_u32(count);
            for (uint32_t i = 0; i < count; ++i)
            {
                builder.append_u32(words[i]);
            }

            return builder.finish();
        }

        std::vector<uint8_t> task_info_routine(macos_emulator& emu, const mig_request& request)
        {
            if (!request.has_ndr())
            {
                return make_mig_error_bytes(request, mig_error::bad_arguments);
            }

            const auto flavor_id = request.arg_u32(0);
            const auto requested = request.arg_u32(1);
            const auto& process = emu.process;

            if (flavor_id == flavor::task_audit_token)
            {
                const std::array<uint32_t, flavor::task_audit_token_count> token{
                    process.uid,        process.euid, process.egid, process.uid, process.gid, process.pid, process.audit_session_id,
                    process.pid_version};
                return counted_reply(emu.mach.ports, request, token, requested);
            }

            if (flavor_id == flavor::task_dyld_info)
            {
                const std::array<uint32_t, flavor::task_dyld_info_count> info{
                    static_cast<uint32_t>(emu.mach.all_image_info_address & 0xFFFFFFFFull),
                    static_cast<uint32_t>(emu.mach.all_image_info_address >> 32),
                    static_cast<uint32_t>(emu.mach.all_image_info_size & 0xFFFFFFFFull),
                    static_cast<uint32_t>(emu.mach.all_image_info_size >> 32), flavor::task_dyld_all_image_info_64};
                return counted_reply(emu.mach.ports, request, info, requested);
            }

            return make_mig_error_bytes(request, kr::invalid_argument);
        }

        // The reply to this routine is the only way the guest ever learns the bootstrap port's name, so
        // every XPC exchange in Task 13 depends on it.
        std::vector<uint8_t> task_get_special_port_routine(macos_emulator& emu, const mig_request& request)
        {
            if (!request.has_ndr())
            {
                return make_mig_error_bytes(request, mig_error::bad_arguments);
            }

            return port_reply(emu.mach.ports, request, emu.mach.get_task_special_port(static_cast<int32_t>(request.arg_u32(0))),
                              kr::failure);
        }

        std::vector<uint8_t> task_set_special_port_routine(macos_emulator& emu, const mig_request& request)
        {
            const auto descriptor = request.descriptor(0);
            if (!descriptor.has_value())
            {
                return make_mig_error_bytes(request, mig_error::bad_arguments);
            }

            const auto which = static_cast<int32_t>(request.arg_u32(0));
            const auto result = emu.mach.set_task_special_port(which, descriptor->name);

            return make_mig_error_bytes(request, result);
        }

        // MIG's own reply path asks for a send-once right on the reply port it is about to use, and that
        // request is itself a MIG call needing a reply port. On a real system the second acquisition
        // finds the port already in the thread's TSD; here, leaving this routine unimplemented made
        // libSystem retry until the stack ran out -- 40,327 mach_msg2 calls deep.
        std::vector<uint8_t> port_extract_right_routine(macos_emulator& emu, const mig_request& request)
        {
            if (!request.has_ndr())
            {
                return make_mig_error_bytes(request, mig_error::bad_arguments);
            }

            const auto name = request.arg_u32(0);
            const auto requested = static_cast<uint8_t>(request.arg_u32(1));

            if (!emu.mach.ports.exists(name))
            {
                return make_mig_error_bytes(request, kr::invalid_name);
            }

            // The caller gets a *new* right, under a name of its own. Handing back the receive port's
            // own name instead looks like success and is not: the caller ends up holding what it
            // already had, decides the extraction did not happen, and asks again forever.
            port_name_t extracted = PORT_NULL;

            switch (requested)
            {
            case disposition::make_send_once:
            case disposition::move_send_once:
                extracted = emu.mach.ports.allocate_send_once_right(name);
                break;

            case disposition::make_send:
            case disposition::copy_send:
            case disposition::move_send:
                extracted = emu.mach.ports.insert_send_right(name);
                break;

            default:
                return make_mig_error_bytes(request, kr::invalid_value);
            }

            if (extracted == PORT_NULL)
            {
                return make_mig_error_bytes(request, kr::resource_shortage);
            }

            // The right was manufactured above, so the reply moves it rather than asking the builder to
            // make a second one under the same name.
            const auto handed_over = (requested == disposition::make_send_once || requested == disposition::move_send_once)
                                         ? disposition::move_send_once
                                         : disposition::move_send;

            mig_reply_builder builder{request.call, emu.mach.ports};
            builder.append_port_descriptor({.name = extracted, .disposition = handed_over, .type = descriptor_type::port});
            return builder.finish();
        }

        std::vector<uint8_t> semaphore_create_routine(macos_emulator& emu, const mig_request& request)
        {
            if (!request.has_ndr())
            {
                return make_mig_error_bytes(request, mig_error::bad_arguments);
            }

            const auto policy = static_cast<int32_t>(request.arg_u32(0));
            const auto value = static_cast<int32_t>(request.arg_u32(1));

            return port_reply(emu.mach.ports, request, emu.mach.create_semaphore(policy, value), kr::resource_shortage);
        }

        std::vector<uint8_t> set_exception_ports_routine(macos_emulator& emu, const mig_request& request, const bool thread_level)
        {
            if (!request.has_ndr())
            {
                return make_mig_error_bytes(request, mig_error::bad_arguments);
            }

            const auto descriptor = request.descriptor(0);
            if (!descriptor.has_value())
            {
                return make_mig_error_bytes(request, mig_error::bad_arguments);
            }

            const auto result = emu.mach.exceptions.set_ports(thread_level, request.arg_u32(0), descriptor->name, request.arg_u32(1),
                                                              static_cast<int32_t>(request.arg_u32(2)));

            return make_mig_error_bytes(request, result);
        }

        std::vector<uint8_t> task_set_exception_ports_routine(macos_emulator& emu, const mig_request& request)
        {
            return set_exception_ports_routine(emu, request, false);
        }

        std::vector<uint8_t> thread_set_exception_ports_routine(macos_emulator& emu, const mig_request& request)
        {
            return set_exception_ports_routine(emu, request, true);
        }

        // find_handler already prefers the thread-level entry over the task-level one, so both routine
        // ids resolve through the same lookup.
        std::vector<uint8_t> get_exception_ports_routine(macos_emulator& emu, const mig_request& request)
        {
            if (!request.has_ndr())
            {
                return make_mig_error_bytes(request, mig_error::bad_arguments);
            }

            const auto mask = request.arg_u32(0);
            const auto handler = first_handler_in_mask(emu, mask);

            mig_reply_builder builder{request.call, emu.mach.ports};
            if (!handler.has_value())
            {
                builder.append_ndr();
                builder.append_u32(0);
                return builder.finish();
            }

            builder.append_port_descriptor({.name = handler->port, .disposition = disposition::copy_send, .type = descriptor_type::port});
            builder.append_ndr();
            builder.append_u32(1);
            builder.append_u32(handler->mask);
            builder.append_u32(handler->behavior);
            builder.append_u32(static_cast<uint32_t>(handler->flavor));
            return builder.finish();
        }

        std::vector<uint8_t> swap_exception_ports_routine(macos_emulator& emu, const mig_request& request, const bool thread_level)
        {
            const auto previous = first_handler_in_mask(emu, request.has_ndr() ? request.arg_u32(0) : 0u);
            auto reply = set_exception_ports_routine(emu, request, thread_level);

            if (!previous.has_value() || reply.size() == MIG_REPLY_ERROR_SIZE)
            {
                return reply;
            }

            mig_reply_builder builder{request.call, emu.mach.ports};
            builder.append_port_descriptor({.name = previous->port, .disposition = disposition::copy_send, .type = descriptor_type::port});
            builder.append_ndr();
            builder.append_u32(1);
            builder.append_u32(previous->mask);
            builder.append_u32(previous->behavior);
            builder.append_u32(static_cast<uint32_t>(previous->flavor));
            return builder.finish();
        }

        std::vector<uint8_t> task_swap_exception_ports_routine(macos_emulator& emu, const mig_request& request)
        {
            return swap_exception_ports_routine(emu, request, false);
        }

        std::vector<uint8_t> thread_swap_exception_ports_routine(macos_emulator& emu, const mig_request& request)
        {
            return swap_exception_ports_routine(emu, request, true);
        }

        // mach_port_set_attributes(task, name, flavor, info, count). Only the queue limit changes
        // anything observable here; the rest describe scheduling policies -- importance donation, denap
        // receivership, throttling -- that a cooperative single-CPU scheduler has no expression for, and
        // are accepted with a named report rather than refused, because a caller that gets an error back
        // treats its port as unusable.
        std::vector<uint8_t> port_set_attributes_routine(macos_emulator& emu, const mig_request& request)
        {
            constexpr uint32_t PORT_LIMITS_INFO = 1;

            if (!request.has_ndr())
            {
                return make_mig_error_bytes(request, mig_error::bad_arguments);
            }

            const auto name = request.arg_u32(0);
            const auto flavor = request.arg_u32(1);
            const auto count = request.arg_u32(2);

            auto* entry = emu.mach.ports.find(name);
            if (entry == nullptr)
            {
                return make_mig_error_bytes(request, kr::invalid_name);
            }

            if (flavor == PORT_LIMITS_INFO)
            {
                if (count < 1)
                {
                    return make_mig_error_bytes(request, kr::invalid_value);
                }

                entry->queue_limit = request.arg_u32(3);
            }
            else
            {
                static std::set<uint32_t> reported{};
                if (reported.insert(flavor).second)
                {
                    emu.log.warn("mach_port_set_attributes flavor %u is accepted but changes nothing in sogen\n", flavor);
                }
            }

            mig_reply_builder builder{request.call, emu.mach.ports};
            builder.append_ndr();
            builder.append_u32(kr::success);
            return builder.finish();
        }

        // Apple ships this only on hardened runtimes; refusing it is what the guest sees on a stock
        // kernel, and libSystem treats the refusal as "not hardened" rather than as an error.
        std::vector<uint8_t> register_hardened_exception_handler_routine(macos_emulator&, const mig_request& request)
        {
            return make_mig_error_bytes(request, kr::not_supported);
        }
    }

    namespace
    {
        // 3457. A token the kernel hands out so a task can name itself to a daemon without handing over a
        // task port. CarbonCore's file-URL machinery and SkyLight's connection bring-up both ask for one;
        // nothing sogen runs ever redeems it, so it is a live port that no routine answers on.
        std::vector<uint8_t> task_create_identity_token_routine(macos_emulator& emu, const mig_request& request)
        {
            const auto token = emu.mach.ports.allocate_receive_right();
            if (token == PORT_NULL)
            {
                return make_mig_error_bytes(request, kr::resource_shortage);
            }

            mig_reply_builder builder{request.call, emu.mach.ports};
            builder.append_port_descriptor({.name = token, .disposition = disposition::make_send, .type = descriptor_type::port});
            return builder.finish();
        }
    }

    void register_task_routines(mig_server_table& table)
    {
        table.register_routine(kernel_object_kind::task, 3215, port_extract_right_routine, "mach_port_extract_right");
        table.register_routine(kernel_object_kind::task, 3218, port_set_attributes_routine, "mach_port_set_attributes");
        table.register_routine(kernel_object_kind::task, 3405, task_info_routine, "task_info");
        table.register_routine(kernel_object_kind::task, 3409, task_get_special_port_routine, "task_get_special_port");
        table.register_routine(kernel_object_kind::task, 3410, task_set_special_port_routine, "task_set_special_port");
        table.register_routine(kernel_object_kind::task, 3413, task_set_exception_ports_routine, "task_set_exception_ports");
        table.register_routine(kernel_object_kind::task, 3414, get_exception_ports_routine, "task_get_exception_ports");
        table.register_routine(kernel_object_kind::task, 3415, task_swap_exception_ports_routine, "task_swap_exception_ports");
        table.register_routine(kernel_object_kind::task, 3418, semaphore_create_routine, "semaphore_create");
        table.register_routine(kernel_object_kind::task, 3457, task_create_identity_token_routine, "task_create_identity_token");
        table.register_routine(kernel_object_kind::task, 3465, register_hardened_exception_handler_routine,
                               "task_register_hardened_exception_handler");
        table.register_routine(kernel_object_kind::thread, 3613, thread_set_exception_ports_routine, "thread_set_exception_ports");
        table.register_routine(kernel_object_kind::thread, 3614, get_exception_ports_routine, "thread_get_exception_ports");
        table.register_routine(kernel_object_kind::thread, 3615, thread_swap_exception_ports_routine, "thread_swap_exception_ports");
    }
}
