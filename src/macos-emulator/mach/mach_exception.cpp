#include "../std_include.hpp"
#include "mach_exception.hpp"

#include "../macos_emulator.hpp"

#include <algorithm>
#include <ranges>
#include <cinttypes>
#include <cstdio>

namespace sogen::mach
{
    namespace
    {
        constexpr int32_t EXCEPTION_RAISE_ID = 2401;
        constexpr int32_t MACH_EXCEPTION_RAISE_ID = 2405;

        bool masks(const exception_handler_entry& entry, const uint32_t type)
        {
            return type < 32 && (entry.mask & (1u << type)) != 0;
        }

        void replace_masked(std::vector<exception_handler_entry>& handlers, const exception_handler_entry& entry)
        {
            std::erase_if(handlers, [&](const exception_handler_entry& existing) { return (existing.mask & ~entry.mask) == 0; });

            for (auto& existing : handlers)
            {
                existing.mask &= ~entry.mask;
            }

            if (entry.port != PORT_NULL)
            {
                handlers.push_back(entry);
            }
        }

        std::vector<uint8_t> build_exception_message(macos_emulator& emu, const exception_handler_entry& handler,
                                                     const raised_exception& raised)
        {
            const auto wide = (handler.behavior & exception_behavior::mach_codes) != 0;
            const auto code_size = wide ? sizeof(uint64_t) : sizeof(uint32_t);
            const auto size =
                MSG_HEADER_SIZE + MSG_BODY_SIZE + 2 * PORT_DESCRIPTOR_SIZE + NDR_RECORD_SIZE + 2 * sizeof(uint32_t) + 2 * code_size;

            std::vector<uint8_t> message(size, 0);
            write_msg_header(message, {.bits = BITS_COMPLEX | make_bits(disposition::copy_send, 0),
                                       .size = static_cast<uint32_t>(size),
                                       .remote_port = handler.port,
                                       .local_port = PORT_NULL,
                                       .voucher_port = 0,
                                       .id = wide ? MACH_EXCEPTION_RAISE_ID : EXCEPTION_RAISE_ID});

            auto offset = MSG_HEADER_SIZE;
            write_u32(message, offset, 2);
            offset += MSG_BODY_SIZE;

            write_port_descriptor(std::span{message}.subspan(offset), {.name = emu.mach.thread_self_for(MACH_MAIN_THREAD_ID),
                                                                       .disposition = disposition::copy_send,
                                                                       .type = descriptor_type::port});
            offset += PORT_DESCRIPTOR_SIZE;

            write_port_descriptor(std::span{message}.subspan(offset),
                                  {.name = emu.mach.task_self, .disposition = disposition::copy_send, .type = descriptor_type::port});
            offset += PORT_DESCRIPTOR_SIZE;

            std::ranges::copy(NDR_RECORD, message.begin() + static_cast<ptrdiff_t>(offset));
            offset += NDR_RECORD_SIZE;

            write_u32(message, offset, raised.type);
            offset += sizeof(uint32_t);
            write_u32(message, offset, 2);
            offset += sizeof(uint32_t);

            if (wide)
            {
                write_u64(message, offset, raised.code);
                write_u64(message, offset + sizeof(uint64_t), raised.subcode);
            }
            else
            {
                write_u32(message, offset, static_cast<uint32_t>(raised.code));
                write_u32(message, offset + sizeof(uint32_t), static_cast<uint32_t>(raised.subcode));
            }

            return message;
        }
    }

    void exception_handler_entry::serialize(utils::buffer_serializer& buffer) const
    {
        buffer.write(this->mask);
        buffer.write(this->port);
        buffer.write(this->behavior);
        buffer.write(this->flavor);
    }

    void exception_handler_entry::deserialize(utils::buffer_deserializer& buffer)
    {
        buffer.read(this->mask);
        buffer.read(this->port);
        buffer.read(this->behavior);
        buffer.read(this->flavor);
    }

    void raised_exception::serialize(utils::buffer_serializer& buffer) const
    {
        buffer.write(this->type);
        buffer.write(this->code);
        buffer.write(this->subcode);
        buffer.write(this->pc);
        buffer.write(this->signal);
        buffer.write(this->delivered);
        buffer.write(this->address_known);
    }

    void raised_exception::deserialize(utils::buffer_deserializer& buffer)
    {
        buffer.read(this->type);
        buffer.read(this->code);
        buffer.read(this->subcode);
        buffer.read(this->pc);
        buffer.read(this->signal);
        buffer.read(this->delivered);
        buffer.read(this->address_known);
    }

    kern_return_t exception_state::set_ports(const bool thread_level, const uint32_t mask, const port_name_t port, const uint32_t behavior,
                                             const int32_t flavor)
    {
        if (mask == 0)
        {
            return kr::invalid_argument;
        }

        const auto raw_behavior = behavior & ~exception_behavior::mach_codes;
        if (raw_behavior < exception_behavior::defaults || raw_behavior > exception_behavior::state_identity)
        {
            return kr::invalid_argument;
        }

        replace_masked(thread_level ? this->thread_handlers_ : this->task_handlers_,
                       {.mask = mask, .port = port, .behavior = behavior, .flavor = flavor});

        return kr::success;
    }

    std::optional<exception_handler_entry> exception_state::find_handler(const uint32_t type) const
    {
        for (const auto* handlers : {&this->thread_handlers_, &this->task_handlers_})
        {
            const auto entry =
                std::ranges::find_if(*handlers, [type](const exception_handler_entry& candidate) { return masks(candidate, type); });
            if (entry != handlers->end())
            {
                return *entry;
            }
        }

        return std::nullopt;
    }

    void exception_state::serialize(utils::buffer_serializer& buffer) const
    {
        buffer.write_vector(this->thread_handlers_);
        buffer.write_vector(this->task_handlers_);
    }

    void exception_state::deserialize(utils::buffer_deserializer& buffer)
    {
        buffer.read_vector(this->thread_handlers_);
        buffer.read_vector(this->task_handlers_);
    }

    int32_t exception_to_signal(const uint32_t type)
    {
        switch (type)
        {
        case exception_type::bad_access:
            return signal_number::sigsegv;
        case exception_type::bad_instruction:
            return signal_number::sigill;
        case exception_type::arithmetic:
            return signal_number::sigfpe;
        case exception_type::breakpoint:
            return signal_number::sigtrap;
        case exception_type::software:
        case exception_type::syscall:
        case exception_type::mach_syscall:
            return signal_number::sigsys;
        default:
            return signal_number::sigabrt;
        }
    }

    std::string_view exception_type_name(const uint32_t type)
    {
        switch (type)
        {
        case exception_type::bad_access:
            return "EXC_BAD_ACCESS";
        case exception_type::bad_instruction:
            return "EXC_BAD_INSTRUCTION";
        case exception_type::arithmetic:
            return "EXC_ARITHMETIC";
        case exception_type::emulation:
            return "EXC_EMULATION";
        case exception_type::software:
            return "EXC_SOFTWARE";
        case exception_type::breakpoint:
            return "EXC_BREAKPOINT";
        case exception_type::syscall:
            return "EXC_SYSCALL";
        case exception_type::mach_syscall:
            return "EXC_MACH_SYSCALL";
        case exception_type::rpc_alert:
            return "EXC_RPC_ALERT";
        case exception_type::crash:
            return "EXC_CRASH";
        case exception_type::guard:
            return "EXC_GUARD";
        default:
            return "EXC_UNKNOWN";
        }
    }

    std::string_view signal_name(const int32_t signal)
    {
        switch (signal)
        {
        case signal_number::sigill:
            return "SIGILL";
        case signal_number::sigtrap:
            return "SIGTRAP";
        case signal_number::sigabrt:
            return "SIGABRT";
        case signal_number::sigfpe:
            return "SIGFPE";
        case signal_number::sigbus:
            return "SIGBUS";
        case signal_number::sigsegv:
            return "SIGSEGV";
        case signal_number::sigsys:
            return "SIGSYS";
        default:
            return "SIG?";
        }
    }

    raised_exception raise_guest_exception(macos_emulator& emu, const uint32_t type, const uint64_t code, const uint64_t subcode)
    {
        raised_exception raised{.type = type,
                                .code = code,
                                .subcode = subcode,
                                .pc = emu.emu().read_instruction_pointer(),
                                .signal = exception_to_signal(type),
                                .delivered = false};

        if (const auto handler = emu.mach.exceptions.find_handler(type); handler.has_value())
        {
            if (auto* entry = emu.mach.ports.destination_of(handler->port); entry != nullptr)
            {
                entry->queue.push_back(build_exception_message(emu, *handler, raised));
                raised.delivered = true;
            }
        }

        emu.mach.last_exception = raised;
        return raised;
    }

    raised_exception report_cpu_exception(macos_emulator& emu, const uint32_t exception_index, const uint64_t pc)
    {
        // QEMU's excp_index values, which unicorn forwards verbatim.
        constexpr uint32_t EXCP_UDEF = 1;
        constexpr uint32_t EXCP_PREFETCH_ABORT = 3;
        constexpr uint32_t EXCP_DATA_ABORT = 4;
        constexpr uint32_t EXCP_BKPT = 7;

        uint32_t type = exception_type::bad_instruction;
        switch (exception_index)
        {
        case EXCP_UDEF:
            type = exception_type::bad_instruction;
            break;
        case EXCP_PREFETCH_ABORT:
        case EXCP_DATA_ABORT:
            // Only an abort unicorn's own mapping layer declined to handle gets here; unmapped and
            // permission faults go to the memory-violation hook instead. What is left is the checks the
            // ARM model applies before a translation is attempted: the alignment rules for exclusive
            // accesses, and an address whose top bits fail the address-size check -- which is what a
            // pointer that failed its PAC authentication looks like.
            type = exception_type::bad_access;
            break;
        case EXCP_BKPT:
            type = exception_type::breakpoint;
            break;
        default:
            type = exception_type::emulation;
            break;
        }

        // No subcode: unicorn stops before the abort reaches EL1, so FAR_EL1 reads back zero and the
        // faulting address exists only in QEMU's own exception record. Reporting zero would be a
        // fabricated address, and one that reads exactly like a null dereference -- which is what sent
        // an arm64e PAC failure looking like a null pointer for hours.
        auto raised = raise_guest_exception(emu, type, exception_index, 0);
        raised.address_known = false;
        raised.pc = pc;
        emu.mach.last_exception = raised;
        return raised;
    }

    raised_exception report_memory_violation(macos_emulator& emu, const uint64_t address, const bool protection_failure, const uint64_t pc)
    {
        auto raised = raise_guest_exception(emu, exception_type::bad_access,
                                            protection_failure ? kr::protection_failure : kr::invalid_address, address);
        raised.pc = pc;
        emu.mach.last_exception = raised;
        return raised;
    }

    std::string format_exception_detail(macos_emulator& emu, const raised_exception& raised)
    {
        auto& cpu = emu.emu();

        // The faulting instruction and its caller are named, because a raw pair of cache addresses tells
        // a reader nothing about which function crashed or who called it -- and that is the first thing
        // anyone wants from a fault.
        const auto where = emu.symbolizer.format(raised.pc);
        const auto caller = emu.symbolizer.format(cpu.reg(arm64_register::x30));

        // For a bad access the address that faulted is the whole question, and it is already carried in
        // the subcode -- reporting the instruction without it leaves the reader to guess which operand
        // was the bad one.
        std::array<char, 768> buffer{};
        if (raised.type == exception_type::bad_access && !raised.address_known)
        {
            std::snprintf(buffer.data(), buffer.size(), "%.*s (%.*s) at %s, called from %s, x0=0x%" PRIx64 " x1=0x%" PRIx64,
                          static_cast<int>(exception_type_name(raised.type).size()), exception_type_name(raised.type).data(),
                          static_cast<int>(signal_name(raised.signal).size()), signal_name(raised.signal).data(), where.c_str(),
                          caller.c_str(), cpu.reg(arm64_register::x0), cpu.reg(arm64_register::x1));

            std::string detail{buffer.data()};
            for (const auto& frame : emu.backtrace(12))
            {
                detail += "\n    " + frame;
            }

            return detail;
        }

        if (raised.type == exception_type::bad_access)
        {
            std::snprintf(buffer.data(), buffer.size(),
                          "%.*s (%.*s) accessing 0x%" PRIx64 " at %s, called from %s, x0=0x%" PRIx64 " x1=0x%" PRIx64,
                          static_cast<int>(exception_type_name(raised.type).size()), exception_type_name(raised.type).data(),
                          static_cast<int>(signal_name(raised.signal).size()), signal_name(raised.signal).data(), raised.subcode,
                          where.c_str(), caller.c_str(), cpu.reg(arm64_register::x0), cpu.reg(arm64_register::x1));

            // Two frames name the faulting instruction; they do not name what the process was doing.
            // A fault inside the C++ unwinder is the case that makes the difference -- the throw site
            // is several frames up, and without it the report says only that libunwind dereferenced
            // null, which is true of every uncaught ObjC exception the guest ever raises.
            std::string detail{buffer.data()};
            for (const auto& frame : emu.backtrace(12))
            {
                detail += "\n    " + frame;
            }

            return detail;
        }

        std::snprintf(buffer.data(), buffer.size(), "%.*s (%.*s) at %s, called from %s, x0=0x%" PRIx64 " x1=0x%" PRIx64 " sp=0x%" PRIx64,
                      static_cast<int>(exception_type_name(raised.type).size()), exception_type_name(raised.type).data(),
                      static_cast<int>(signal_name(raised.signal).size()), signal_name(raised.signal).data(), where.c_str(), caller.c_str(),
                      cpu.reg(arm64_register::x0), cpu.reg(arm64_register::x1), cpu.reg(arm64_register::sp));

        std::string detail = buffer.data();
        for (const auto& frame : emu.backtrace(12))
        {
            detail += "\n    " + frame;
        }
        return detail;
    }
}
