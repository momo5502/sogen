#pragma once

#include <chrono>
#include <cstdint>
#include <cstddef>
#include <vector>

namespace sogen
{

    struct cpu_interface
    {
        virtual ~cpu_interface() = default;

        // Identity of this virtual CPU within its machine, in [0, vcpu_count).
        virtual size_t index() const = 0;

        struct descriptor_table_register
        {
            uint64_t base{};
            uint32_t limit{};
        };

        virtual bool read_descriptor_table(int reg, descriptor_table_register& table) = 0;

        virtual void start(size_t count = 0) = 0;
        virtual void stop() = 0;

        virtual size_t read_raw_register(int reg, void* value, size_t size) = 0;
        virtual size_t write_raw_register(int reg, const void* value, size_t size) = 0;

        virtual std::vector<std::byte> save_registers() const = 0;
        virtual void restore_registers(const std::vector<std::byte>& register_data) = 0;

        // TODO: Remove this
        virtual bool has_violation() const = 0;

        virtual bool supports_instruction_counting() const = 0;

        // Whether this backend reports read_instruction_pointer() already advanced past a trapping
        // INT3 (0xCC) byte for a breakpoint exception, rather than at the INT3 itself. Real x86
        // hardware treats #BP as a trap (RIP already past it when the fault is observed), which is
        // why NT's KiBreakpointTrap decrements the trap-frame RIP by 1 to report the INT3's own
        // address to guest handlers - but not every backend's *emulation* of that trap surfaces the
        // same already-advanced value: KVM/WHP both catch INT3 by decoding/exiting at the
        // instruction's own address (pre-advance, no correction needed), while FEXCore's JIT
        // translation of INT3 sets SetRIPToNext before the host trap fires (already advanced,
        // needs the same -1 correction real hardware's trap frame would have received). Defaults to
        // false (matches every backend except FEX).
        virtual bool reports_breakpoint_rip_past_instruction() const
        {
            return false;
        }

        // Whether this backend maintains separate, independently-active 32-bit and 64-bit CPU
        // engine contexts for a WoW64 thread ("dual-engine"/gate-crossing backends), as opposed to
        // a single unified CPU state that's already bitness-aware. Only a dual-engine backend can
        // have its 64-bit engine "transiently active" mid gate-crossing while wow64.dll's real
        // CONTEXT32->CONTEXT64 marshaling code runs - the scenario syscalls/thread.cpp's WoW64
        // NtContinue reverse-gate exists to handle. On a single-engine backend there's no such
        // transient state to correct: the normal cpu_context::restore path already resumes a WoW64
        // thread correctly, and taking the reverse-gate path anyway hijacks an ordinary NtContinue
        // call, corrupting the resume. Defaults to false (matches every backend except FEX).
        virtual bool has_separate_bitness_engines() const
        {
            return false;
        }

        // Whether stop() may be safely called from a different thread while the CPU is executing.
        // Hypervisor-backed backends can cancel execution from any thread; the JIT/interpreter
        // backends cannot, so they must be preempted cooperatively from the CPU thread instead.
        virtual bool is_stop_thread_safe() const = 0;
    };

} // namespace sogen
