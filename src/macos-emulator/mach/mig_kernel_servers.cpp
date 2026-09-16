#include "../std_include.hpp"
#include "mig_kernel_servers.hpp"

#include "../macos_emulator.hpp"
#include "mig_routines_iokit.hpp"
#include "xpc_bootstrap.hpp"
#include "xpc_services.hpp"

#include <set>

#include <algorithm>

namespace sogen::mach
{
    // Inspects the body rather than trusting args_offset, so a request built directly -- as a routine's
    // own unit test does -- reports the same answer as one built by make_mig_request.
    bool mig_request::has_ndr() const
    {
        const auto prefix = this->args_offset >= NDR_RECORD_SIZE ? this->args_offset - NDR_RECORD_SIZE : this->args_offset;

        return prefix <= this->body.size() && NDR_RECORD_SIZE <= this->body.size() - prefix &&
               std::equal(NDR_RECORD.begin(), NDR_RECORD.end(), this->body.begin() + static_cast<ptrdiff_t>(prefix));
    }

    size_t mig_request::effective_args_offset() const
    {
        if (this->args_offset != 0)
        {
            return this->args_offset;
        }

        return this->has_ndr() ? NDR_RECORD_SIZE : 0;
    }

    uint32_t mig_request::arg_u32(const size_t index) const
    {
        const auto offset = this->effective_args_offset() + index * sizeof(uint32_t);
        if (offset > this->body.size() || sizeof(uint32_t) > this->body.size() - offset)
        {
            return 0;
        }

        return read_u32(this->body, offset);
    }

    uint64_t mig_request::arg_u64(const size_t byte_offset) const
    {
        const auto offset = this->effective_args_offset() + byte_offset;
        if (offset > this->body.size() || sizeof(uint64_t) > this->body.size() - offset)
        {
            return 0;
        }

        return read_u64(this->body, offset);
    }

    std::optional<port_descriptor> mig_request::descriptor(const size_t index) const
    {
        if ((this->call.header.bits & BITS_COMPLEX) == 0 || index >= this->call.descriptor_count)
        {
            return std::nullopt;
        }

        auto offset = MSG_BODY_SIZE;
        for (size_t i = 0; i < index; ++i)
        {
            if (offset > this->body.size() || PORT_DESCRIPTOR_SIZE > this->body.size() - offset)
            {
                return std::nullopt;
            }

            offset += descriptor_size(read_port_descriptor(this->body.subspan(offset)).type);
        }

        if (offset > this->body.size() || PORT_DESCRIPTOR_SIZE > this->body.size() - offset)
        {
            return std::nullopt;
        }

        return read_port_descriptor(this->body.subspan(offset));
    }

    // A complex request begins with a mach_msg_body_t and its descriptors, and only then the NDR record.
    // Assuming the arguments start at a fixed offset reads a port name as an address -- routine 4811's
    // arguments begin 24 bytes in (4 + 12 + 8) for exactly this reason.
    mig_request make_mig_request(const msg_call& call, const std::span<const uint8_t> body, const kernel_object_kind destination)
    {
        size_t offset = 0;

        if ((call.header.bits & BITS_COMPLEX) != 0)
        {
            offset = MSG_BODY_SIZE;

            for (uint32_t i = 0; i < call.descriptor_count; ++i)
            {
                if (offset > body.size() || PORT_DESCRIPTOR_SIZE > body.size() - offset)
                {
                    break;
                }

                offset += descriptor_size(read_port_descriptor(body.subspan(offset)).type);
            }
        }

        if (offset <= body.size() && NDR_RECORD_SIZE <= body.size() - offset &&
            std::equal(NDR_RECORD.begin(), NDR_RECORD.end(), body.begin() + static_cast<ptrdiff_t>(offset)))
        {
            offset += NDR_RECORD_SIZE;
        }

        return {.call = call, .body = body, .destination = destination, .id = call.header.id, .args_offset = offset};
    }

    mig_reply_builder::mig_reply_builder(const msg_call& call, mach_port_namespace& ports)
        : call_(call),
          ports_(ports),
          bytes_(MSG_HEADER_SIZE, 0)
    {
    }

    void mig_reply_builder::set_complex()
    {
        if (!this->complex_)
        {
            this->complex_ = true;
            this->bytes_.resize(this->bytes_.size() + MSG_BODY_SIZE, 0);
        }
    }

    void mig_reply_builder::append_ndr()
    {
        this->append_bytes(NDR_RECORD);
    }

    void mig_reply_builder::append_u32(const uint32_t value)
    {
        std::array<uint8_t, sizeof(uint32_t)> scratch{};
        write_u32(scratch, 0, value);
        this->append_bytes(scratch);
    }

    void mig_reply_builder::append_u64(const uint64_t value)
    {
        std::array<uint8_t, sizeof(uint64_t)> scratch{};
        write_u64(scratch, 0, value);
        this->append_bytes(scratch);
    }

    void mig_reply_builder::append_port_descriptor(const port_descriptor& descriptor)
    {
        this->set_complex();

        // Translated here rather than at every call site: a routine that forgets is not obviously
        // broken -- the guest accepts the reply and then tries to extract the right it was promised,
        // which is another message with the same defect.
        auto received = descriptor;
        received.disposition = received_disposition(descriptor.disposition);

        // make_send and copy_send mean the kernel manufactures a right *for the receiver*, so the guest
        // ends up holding one more send uref than before; a routine that hands over a port without this
        // leaves the guest with a name it cannot retain. libxpc's bootstrap pipe is where that first
        // shows: it calls mach_port_mod_refs(SEND, +1) on the port and treats the failure as fatal.
        // move_send is a transfer of a right the routine already created, so it is left alone.
        if (descriptor.disposition == disposition::make_send || descriptor.disposition == disposition::copy_send)
        {
            this->ports_.insert_send_right(descriptor.name);
        }

        std::array<uint8_t, PORT_DESCRIPTOR_SIZE> scratch{};
        write_port_descriptor(scratch, received);
        this->append_bytes(scratch);

        ++this->descriptor_count_;
        write_u32(this->bytes_, MSG_HEADER_SIZE, this->descriptor_count_);
    }

    void mig_reply_builder::append_bytes(const std::span<const uint8_t> bytes)
    {
        this->bytes_.insert(this->bytes_.end(), bytes.begin(), bytes.end());
    }

    std::vector<uint8_t> mig_reply_builder::finish(const int32_t reply_id_offset)
    {
        return this->finish_with_id(this->call_.header.id + reply_id_offset);
    }

    // XPC does not follow MIG's id + 100 convention: launchd's lookup results and daemon replies carry
    // an absolute id of their own, so the caller names it instead of an offset.
    std::vector<uint8_t> mig_reply_builder::finish_with_id(const int32_t id)
    {
        write_msg_header(this->bytes_, {
                                           .bits = reply_bits_for(this->call_.header.bits, this->complex_),
                                           .size = static_cast<uint32_t>(this->bytes_.size()),
                                           .remote_port = PORT_NULL,
                                           .local_port = this->call_.header.local_port,
                                           .voucher_port = 0,
                                           .id = id,
                                       });

        return std::move(this->bytes_);
    }

    void mig_server_table::register_routine(const kernel_object_kind destination, const int32_t id, mig_routine routine, std::string name)
    {
        this->routines_[{destination, id}] = entry{.routine = std::move(routine), .name = std::move(name)};
    }

    const mig_routine* mig_server_table::find(const kernel_object_kind destination, const int32_t id) const
    {
        const auto match = this->routines_.find({destination, id});
        return match == this->routines_.end() ? nullptr : &match->second.routine;
    }

    std::string_view mig_server_table::name_of(const kernel_object_kind destination, const int32_t id) const
    {
        const auto match = this->routines_.find({destination, id});
        return match == this->routines_.end() ? std::string_view{} : std::string_view{match->second.name};
    }

    std::vector<uint8_t> make_mig_error_bytes(const mig_request& request, const kern_return_t code)
    {
        return make_mig_error_reply(request.call, code).bytes;
    }

    // Construction order is explicit rather than relying on static initialisation across translation
    // units, so a routine can never be registered before the table exists.
    mig_server_table& kernel_mig_servers()
    {
        static mig_server_table table = [] {
            mig_server_table built{};
            register_host_routines(built);
            register_task_routines(built);
            register_vm_routines(built);
            register_restartable_routines(built);
            register_iokit_routines(built);
            return built;
        }();

        return table;
    }

    namespace
    {
        std::string_view describe_port_kind(const kernel_object_kind kind)
        {
            switch (kind)
            {
            case kernel_object_kind::task:
                return "task";
            case kernel_object_kind::host:
                return "host";
            case kernel_object_kind::host_priv:
                return "host_priv";
            case kernel_object_kind::thread:
                return "thread";
            case kernel_object_kind::semaphore:
                return "semaphore";
            case kernel_object_kind::clock:
                return "clock";
            case kernel_object_kind::voucher:
                return "voucher";
            case kernel_object_kind::bootstrap:
                return "bootstrap";
            case kernel_object_kind::xpc_service:
                return "xpc_service";
            case kernel_object_kind::memory_entry:
                return "memory_entry";
            case kernel_object_kind::exception_handler:
                return "exception_handler";
            case kernel_object_kind::window_server:
                return "window_server";
            case kernel_object_kind::render_server:
                return "render_server";
            case kernel_object_kind::window_server_event:
                return "window_server_event";
            case kernel_object_kind::window_server_connection:
                return "window_server_connection";
            case kernel_object_kind::timer:
                return "timer";
            case kernel_object_kind::io_master:
                return "io_master";
            case kernel_object_kind::io_object:
                return "io_object";
            default:
                return "unknown";
            }
        }
    }

    namespace
    {
        constexpr int32_t XPC_ROUTINE_CHECKIN_ID = 0x40000323;
        constexpr int32_t XPC_ROUTINE_LOOKUP_ID = 0x40000324;
        constexpr int32_t XPC_ROUTINE_LOOKUP_BY_NAME_ID = 0x400000cf;
    }

    std::vector<uint8_t> dispatch_kernel_message(macos_emulator& emu, const msg_call& call, const std::span<const uint8_t> body,
                                                 const kernel_object_kind destination)
    {
        // A service port from an XPC lookup speaks @XPC like the bootstrap port does: same flat TLV, and
        // the reply id convention is not MIG's.
        if (destination == kernel_object_kind::xpc_service)
        {
            return xpc::answer_service_message(emu, call, body);
        }

        // The bootstrap port speaks @XPC rather than MIG: its replies carry the SAME msgh_id, never
        // id + 100, so it cannot go through the MIG table.
        if (destination == kernel_object_kind::bootstrap)
        {
            // Reported once per routine. An unimplemented one here used to be silent, unlike an
            // unimplemented MIG routine, so a guest stuck on a service sogen does not run gave no clue
            // which request it was stuck on.
            if (xpc::bootstrap_responder::is_xpc_routine(call.header.id))
            {
                static std::set<int32_t> reported{};
                if (call.header.id != XPC_ROUTINE_CHECKIN_ID && call.header.id != XPC_ROUTINE_LOOKUP_ID &&
                    call.header.id != XPC_ROUTINE_LOOKUP_BY_NAME_ID && reported.insert(call.header.id).second)
                {
                    emu.log.warn("unimplemented XPC routine 0x%x on the bootstrap port\n", call.header.id);
                }
            }

            if (auto reply = xpc::bootstrap_responder::respond(emu, call, body); reply.has_value())
            {
                return *reply;
            }

            return make_mig_error_reply(call, mig_error::bad_id).bytes;
        }

        const auto* routine = kernel_mig_servers().find(destination, call.header.id);
        if (routine == nullptr)
        {
            // An unimplemented MIG routine used to be silent, unlike an unimplemented syscall. A guest
            // that retries one -- and libSystem does, from inside the path that acquires its own reply
            // port -- then spins until the stack runs out, with nothing naming the routine responsible.
            // Reported once per (port kind, id) so a loop does not bury the rest of the run.
            static std::set<std::pair<kernel_object_kind, int32_t>> reported{};
            if (reported.emplace(destination, call.header.id).second)
            {
                emu.log.warn("unimplemented MIG routine %d on a %.*s port\n", call.header.id,
                             static_cast<int>(describe_port_kind(destination).size()), describe_port_kind(destination).data());
            }

            return make_mig_error_reply(call, mig_error::bad_id).bytes;
        }

        return (*routine)(emu, make_mig_request(call, body, destination));
    }
}
