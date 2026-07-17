// _GNU_SOURCE is required for mremap() (used to alias host memory into the guest address space).
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#define FEX_EMULATOR_IMPL
#include "fex_x86_64_emulator.hpp"
#include "fex_x86_64_common.hpp"
#include "fex_x86_64_marshal.hpp"

// ---------------------------------------------------------------------------------------------------
// FEX-Emu backend (basic support).
//
// FEX (https://fex-emu.com) is an in-process x86/x86-64 -> AArch64 binary translator. Unlike the
// Unicorn/Icicle/KVM backends, FEX does NOT manage a sandboxed guest address space: it executes the
// translated guest inside the *host* process and treats guest virtual addresses as host virtual
// addresses (a 1:1 mapping). Consequences that shape this backend:
//
//   * map_memory() is a real mmap(MAP_FIXED) at the guest address; read/write_memory() is a direct
//     host memcpy once the range is known to be mapped.
//   * The guest runs natively (JITed), so - exactly like the KVM backend - there is no per-access or
//     per-instruction instrumentation point. Memory/execution/basic-block hooks are accepted for API
//     compatibility but never fire.
//   * Guest `syscall` instructions are routed back to sogen through a FEXCore::HLE::SyscallHandler,
//     which invokes the registered syscall instruction-hook. That is what lets the Windows emulation
//     layer service NT syscalls.
//
// This file targets AArch64 Linux/Android hosts (FEX only JITs to ARM64). It is written against the
// FEXCore embedding API; spots that depend on FEX-version-specific internals are marked TODO(fex).
// ---------------------------------------------------------------------------------------------------

#include <cstdlib>
#include <sys/mman.h>
#include <unistd.h>

// backtrace()/backtrace_symbols() are used unconditionally by the std::terminate handler below
// (initialize_context()). SOGEN_ENABLE_FEX only ever enables this backend on Linux or Darwin (see
// the top-level CMakeLists.txt gate), and both platforms' libc provide <execinfo.h>, so this
// include does not need to be Apple-only.
#include <execinfo.h>

#ifdef __APPLE__
#include <sys/sysctl.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/arm/thread_status.h>
#include <signal.h>
#include <pthread.h>
#include <libkern/OSCacheControl.h>
#include <libproc.h>
#endif

#include <atomic>
#include <bit>
#include <cerrno>
#include <cstring>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <utils/object.hpp>

// FEXCore embedding headers. These are only available when building against a FEX checkout/install;
// the CMake glue gates this whole target behind SOGEN_ENABLE_FEX so non-ARM builds never reach here.
#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/HostFeatures.h>
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/Core/X86Enums.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/Utils/LongJump.h>
#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/Utils/AllocatorHooks.h>

namespace sogen::fex
{
    class fex_x86_64_emulator;

    namespace
    {
        constexpr size_t page_size = 0x1000;

        // On Apple Silicon macOS, every process has a mandatory, unshrinkable 4GB __PAGEZERO
        // segment - the entire address range a real 32-bit x86 (WoW64) guest process needs (image
        // base, stack, heap, both 32-bit ntdll/kernel32) is permanently unmappable there. FEXCore's
        // JIT rebases every 32-bit-mode guest memory dereference/instruction fetch by a
        // per-Context, runtime-configurable offset (FEXCore::Context::Config.Wow64GuestRebaseValue,
        // set via SetWow64GuestRebaseValue - see its doc comment, public Context.h) so real host
        // memory can back 32-bit guest addresses at guest_addr + that offset instead. Everything
        // above this backend (sogen's PE loader, syscall handlers) continues to deal exclusively in
        // ordinary guest addresses, unaware this translation exists.
        //
        // wow64_guest_rebase_default is only the FIRST candidate this backend's own
        // reserve_wow64_host_window() tries (see its doc comment for the full rationale - in short,
        // no single fixed compile-time offset is safe: what host VA is actually free depends on
        // what else this specific process/machine has already mapped, which a hardcoded guess
        // cannot account for). It's also the value used unconditionally on any platform where that
        // dynamic search doesn't run (e.g. Linux ARM64 - this backend is shared, not Apple-
        // exclusive), preserving this backend's original, pre-dynamic-rebase behavior there exactly.
        constexpr uint64_t wow64_guest_rebase_default = 0x400000000ULL;

        // A 32-bit x86 guest's entire architectural address space is bounded by its 32-bit
        // pointers: [0, 4GB). This is the ONLY range that ever needs the wow64 rebase applied - kept
        // separate from the rebase offset itself (this backend used to conflate the two, which was
        // a real bug: real 64-bit ntdll/win32u modules are placed starting at memory_manager's
        // DEFAULT_ALLOCATION_ADDRESS_64BIT, which is exactly 4GB - i.e. *below* the old 16GB
        // rebase-offset default - so a wow64 process's real 64-bit ntdll could land in [4GB, 16GB)
        // and get incorrectly rebased right along with genuine 32-bit addresses, corrupting where
        // its real host memory actually lives relative to where context_'s 64-bit JIT - which
        // applies no internal rebase of its own, see GuestMemoryRebase()'s Config.Is64BitMode()
        // gate in deps/FEX - expects to find it).
        constexpr uint64_t wow64_guest_address_space_size = 0x100000000ULL;

        // A real wow64 process maps BOTH a 32-bit executable/ntdll32 (living in [0, 4GB), needing
        // the rebase) AND the real 64-bit ntdll/win32u/wow64*.dll support modules (living anywhere
        // from 4GB up, needing NO rebase at all) - module_manager::load_wow64_modules maps both
        // kinds while this backend's is_wow64_process_ is already true. Blanket-applying the rebase
        // whenever is_wow64_process_ is set - rather than per-address - would incorrectly shift the
        // 64-bit modules' own addresses too. Gate on the address itself: anything at or past the
        // true 32-bit address-space boundary (wow64_guest_address_space_size) is left alone,
        // regardless of how far below the rebase offset (the unrelated value actually added) it
        // happens to sit.

        // The 64-bit user code-segment selector (matches sogen::wow64::heaven_gate::kUserCodeSelector
        // in src/windows-emulator/wow64_heaven_gate.hpp - kept as a local constant to avoid pulling
        // the windows-emulator include tree into this backend). A gate crossing whose target CS is
        // this selector is entering 64-bit mode (context_); anything else (0x23, the 32-bit compat
        // selector) is entering 32-bit mode (context32_). See perform_gate_crossing.
        constexpr uint16_t wow64_user_code_selector_64bit = 0x33;

#ifdef __APPLE__
        // FEXCore's own JITWriteScope (deps/FEX/FEXCore/include/FEXCore/Utils/AllocatorHooks.h) is a
        // plain ctor/dtor boolean toggle around pthread_jit_write_protect_np, not a nesting counter.
        // FEXCore::CPU::Arm64JITCore::ExitFunctionLink and its two delinker helpers (JIT.cpp) patch an
        // already-JIT-compiled call site - self-modifying code needing write-protect disabled - but
        // were written with no JITWriteScope of their own (upstream FEX targets Linux, which has no
        // per-thread MAP_JIT W^X concept). Our own wrapper around Pointers.ExitFunctionLink brackets
        // that call with a toggle to cover it, but ExitFunctionLink's "not yet compiled" path calls
        // CompileBlock, which uses ITS OWN nested JITWriteScope - and that nested scope's destructor
        // re-enables write-protect *before* control returns to our still-writing outer call, faulting
        // on the self-modifying store with a real (not synthetic) protection violation.
        //
        // Tried and abandoned: interposing pthread_jit_write_protect_np itself (via the __interpose
        // section) to make it reentrant. Both escape hatches for reaching the *real* implementation
        // from inside the interposer - dlsym(RTLD_NEXT, ...) and dlopen(RTLD_NOLOAD)+dlsym-by-handle -
        // resolved back to the interposed replacement itself (confirmed via a stack-overflow crash
        // both times), meaning dyld's __interpose rewrite here isn't scoped to "other images calling
        // in" the way DYLD_INTERPOSE is normally described; there's no reliable way to bypass it once
        // installed for this exact symbol name process-wide.
        //
        // Instead: react to the fault itself. The *executing* code at the moment of the crash
        // (ExitFunctionLink, a plain libFEXCore.dylib function) is not MAP_JIT memory - only the
        // CodeBuffer it writes into is - so leaving write-protect disabled for longer than strictly
        // necessary is harmless as long as it's re-enabled before control returns to actually
        // executing JIT-compiled (MAP_JIT) code again. See handle_fault_signal's SEGV_ACCERR branch:
        // on a protection fault whose PC lands outside the dispatcher/guest range entirely (i.e.
        // inside FEXCore's own regular code, not a Break-op trampoline), disable write-protect and
        // retry the same faulting instruction rather than treating it as unhandled. Bounded per fault
        // address so a genuinely different bug can't spin forever.
        //
        // This tracking used to be a std::unordered_map<uint64_t, int>, but operator[] can insert a
        // node or trigger a rehash - both call into the heap allocator - and this code runs inside a
        // real hardware signal handler that can interrupt program execution at an arbitrary point,
        // including mid-malloc()/free() of a totally unrelated allocation. Calling a non-async-
        // signal-safe function from a signal handler in that situation is undefined behavior, and was
        // confirmed to be the actual cause of a rare, ASLR-timing-dependent heap corruption bug (the
        // allocator's internal free-list/lock can be left mid-update if re-entered). Replaced with a
        // small fixed-size, non-allocating array, linear-scanned: genuinely async-signal-safe, and
        // sufficient because guest execution is single-threaded/cooperative (this handler resolves
        // one fault to completion before the interrupted code can trigger another), so realistically
        // only one address is ever mid-retry at a time; a healthy call site resolves in <= 1 retry and
        // never spins, so eviction (once all slots are in use) can never take budget away from an
        // address that's actually mid-retry.
        struct jit_write_protect_retry_slot
        {
            uint64_t address = 0;
            int count = 0;
            bool used = false;
            uint64_t last_fault_ns = 0;
        };

        constexpr size_t jit_write_protect_retry_slot_count = 8;
        jit_write_protect_retry_slot g_jit_write_protect_retry_slots[jit_write_protect_retry_slot_count];
        size_t g_jit_write_protect_retry_next_evict = 0;

        // A burst of retries for the same address within this window counts toward the retry bound;
        // a gap at least this long since the address last faulted means it's being reused healthily
        // (a fresh, unrelated race resolved in the meantime), not spinning - see
        // jit_write_protect_retry_count_for's doc comment.
        constexpr uint64_t jit_write_protect_retry_reset_window_ns = 100'000'000; // 100ms

        uint64_t monotonic_now_ns()
        {
            struct timespec ts{};
            ::clock_gettime(CLOCK_MONOTONIC, &ts);
            return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL + static_cast<uint64_t>(ts.tv_nsec);
        }

        // Returns the retry counter for fault_addr, resetting it first if the address hasn't
        // faulted within jit_write_protect_retry_reset_window_ns - otherwise a slot that legitimately
        // resolves this race many times over a long run would eventually exhaust its retry budget
        // (it was previously only ever incremented, never reset) and start being treated as
        // unresolvable, turning an occasional benign race into an eventual hard crash. Async-signal-
        // safe: fixed-array scan, no allocation; clock_gettime(CLOCK_MONOTONIC) is vDSO-backed and
        // safe to call from a signal handler.
        int& jit_write_protect_retry_count_for(const uint64_t fault_addr)
        {
            const uint64_t now_ns = monotonic_now_ns();
            jit_write_protect_retry_slot* free_slot = nullptr;
            for (auto& slot : g_jit_write_protect_retry_slots)
            {
                if (slot.used && slot.address == fault_addr)
                {
                    if (now_ns - slot.last_fault_ns > jit_write_protect_retry_reset_window_ns)
                    {
                        slot.count = 0;
                    }
                    slot.last_fault_ns = now_ns;
                    return slot.count;
                }
                if (free_slot == nullptr && !slot.used)
                {
                    free_slot = &slot;
                }
            }

            auto& slot = (free_slot != nullptr)
                             ? *free_slot
                             : g_jit_write_protect_retry_slots[g_jit_write_protect_retry_next_evict++ % jit_write_protect_retry_slot_count];
            slot.address = fault_addr;
            slot.count = 0;
            slot.used = true;
            slot.last_fault_ns = now_ns;
            return slot.count;
        }

        // Apple Silicon's fixed host mmap/mprotect granularity (no way to get 4KB host pages).
        constexpr size_t host_page_size_apple = 0x4000;

        uint64_t host_page_align_down_apple(const uint64_t value)
        {
            return value & ~(host_page_size_apple - 1);
        }

        uint64_t host_page_align_up_apple(const uint64_t value)
        {
            return (value + host_page_size_apple - 1) & ~(host_page_size_apple - 1);
        }

        // Decodes just enough of an AArch64 "Load register" instruction to service an mmio_region
        // fault: destination register, transfer size, and zero/sign extension. Deliberately does not
        // decode the addressing mode (immediate, unscaled, register-offset, ...) - the effective
        // address is already known exactly (it's the fault address FEXCore's guest-VA==host-VA model
        // uses directly), so only the fields the "Load/store register" encoding class keeps at fixed
        // bit positions across every addressing-mode sub-form are needed (size/opc at [31:30]/[23:22],
        // Rt at [4:0] - see the AArch64 ISA's C4.1.3 "Loads and stores" encoding table). Stores are not
        // decoded: this backend's only MMIO consumer is read-only (see mmio_region's doc comment).
        struct decoded_arm64_load
        {
            uint32_t size = 0; // bytes: 1, 2, 4, 8, 16
            bool sign_extend = false;
            bool dest_is_64bit = false;
            bool is_vector = false; // true: rt names a SIMD&FP register (Qt), not a GPR
            uint32_t rt = 0;
        };

        std::optional<decoded_arm64_load> decode_arm64_load(const uint32_t insn)
        {
            struct encoding
            {
                uint32_t value;
                uint32_t size;
                bool sign_extend;
                bool dest_is_64bit;
                bool is_vector = false;
            };

            // "Load register (unsigned immediate)" - top 10 bits (size:2, fixed:6, opc:2).
            static constexpr encoding unsigned_imm_loads[] = {
                {0x39400000U, 1, false, false}, // LDRB
                {0x39800000U, 1, true, true},   // LDRSB, 64-bit dest
                {0x39C00000U, 1, true, false},  // LDRSB, 32-bit dest
                {0x79400000U, 2, false, false}, // LDRH
                {0x79800000U, 2, true, true},   // LDRSH, 64-bit dest
                {0x79C00000U, 2, true, false},  // LDRSH, 32-bit dest
                {0xB9400000U, 4, false, false}, // LDR Wt
                {0xB9800000U, 4, true, true},   // LDRSW
                {0xF9400000U, 8, false, true},  // LDR Xt
            };

            // "Load register (register offset)" - top 22 bits + fixed low bits (opt/S/1/0 = 0x800).
            static constexpr encoding reg_offset_loads[] = {
                {0x38600800U, 1, false, false}, // LDRB (register)
                {0x38A00800U, 1, true, true},   // LDRSB (register), 64-bit dest
                {0x38E00800U, 1, true, false},  // LDRSB (register), 32-bit dest
                {0x78600800U, 2, false, false}, // LDRH (register)
                {0x78A00800U, 2, true, true},   // LDRSH (register), 64-bit dest
                {0x78E00800U, 2, true, false},  // LDRSH (register), 32-bit dest
                {0xB8600800U, 4, false, false}, // LDR Wt (register)
                {0xB8A00800U, 4, true, true},   // LDRSW (register)
                {0xF8600800U, 8, false, true},  // LDR Xt (register)
            };

            // "Load register (unscaled immediate, LDUR)" - same top22+low-bits mask width as the
            // register-offset form above, but bit21=0 (vs. 1) and bits[11:10]="00" (vs. "10"); the
            // 9-bit signed immediate at bits[20:12] is variable/unchecked, same as Rn/Rt.
            static constexpr encoding unscaled_loads[] = {
                {0x38400000U, 1, false, false},        // LDURB
                {0x38800000U, 1, true, true},          // LDURSB, 64-bit dest
                {0x38C00000U, 1, true, false},         // LDURSB, 32-bit dest
                {0x78400000U, 2, false, false},        // LDURH
                {0x78800000U, 2, true, true},          // LDURSH, 64-bit dest
                {0x78C00000U, 2, true, false},         // LDURSH, 32-bit dest
                {0xB8400000U, 4, false, false},        // LDUR Wt
                {0xB8800000U, 4, true, true},          // LDURSW
                {0xF8400000U, 8, false, true},         // LDUR Xt
                {0x3CC00000U, 16, false, false, true}, // LDUR Qt
            };

            // "Load-acquire register (LDAPR/LDAPRB/LDAPRH, and the older base LDAR/LDARB/LDARH from
            // the Load/store-exclusive class)" - the forms FEX emits for guest memory reads to model
            // x86's stronger memory ordering on ARM's weaker one. No addressing mode at all (always
            // [Xn], Rm fixed to the 11111 placeholder), so only Rn/Rt vary - mask out the low 10 bits.
            // No signed variants exist for either family.
            static constexpr encoding acquire_loads[] = {
                {0x38BFC000U, 1, false, false}, // LDAPRB
                {0x78BFC000U, 2, false, false}, // LDAPRH
                {0xB8BFC000U, 4, false, false}, // LDAPR Wt
                {0xF8BFC000U, 8, false, true},  // LDAPR Xt
                {0x08DFFC00U, 1, false, false}, // LDARB
                {0x48DFFC00U, 2, false, false}, // LDARH
                {0x88DFFC00U, 4, false, false}, // LDAR Wt
                {0xC8DFFC00U, 8, false, true},  // LDAR Xt
            };

            // "Load SIMD&FP register (unsigned immediate), 128-bit" - shares unsigned_imm_loads' mask/
            // position (V=1, size=00, opc=11 is the reserved combination meaning a 128-bit Q register
            // rather than a scalar B/H/S/D FP register). FEX uses this for a wide (16-byte) guest read.
            static constexpr encoding vector_loads[] = {
                {0x3DC00000U, 16, false, false, true}, // LDR Qt
            };

            const uint32_t rt = insn & 0x1FU;
            const uint32_t top10 = insn & 0xFFC00000U;
            const uint32_t top22_fixed_low = insn & 0xFFE00C00U;
            const uint32_t acquire_fixed = insn & 0xFFFFFC00U;

            for (const auto& enc : unsigned_imm_loads)
            {
                if (top10 == enc.value)
                {
                    return decoded_arm64_load{enc.size, enc.sign_extend, enc.dest_is_64bit, enc.is_vector, rt};
                }
            }

            for (const auto& enc : reg_offset_loads)
            {
                if (top22_fixed_low == enc.value)
                {
                    return decoded_arm64_load{enc.size, enc.sign_extend, enc.dest_is_64bit, enc.is_vector, rt};
                }
            }

            for (const auto& enc : unscaled_loads)
            {
                if (top22_fixed_low == enc.value)
                {
                    return decoded_arm64_load{enc.size, enc.sign_extend, enc.dest_is_64bit, enc.is_vector, rt};
                }
            }

            for (const auto& enc : acquire_loads)
            {
                if (acquire_fixed == enc.value)
                {
                    return decoded_arm64_load{enc.size, enc.sign_extend, enc.dest_is_64bit, enc.is_vector, rt};
                }
            }

            for (const auto& enc : vector_loads)
            {
                if (top10 == enc.value)
                {
                    return decoded_arm64_load{enc.size, enc.sign_extend, enc.dest_is_64bit, enc.is_vector, rt};
                }
            }

            return std::nullopt;
        }

        // Decodes an AArch64 "Store-release register" (STLR/STLRB/STLRH) instruction - the
        // counterpart to decode_arm64_load's LDAR/LDAPR handling, used only for the misaligned-atomic
        // fallback in handle_fault_signal (see its doc comment), never for mmio_region (this backend's
        // only MMIO consumer is read-only). Same fixed-Rm, no-addressing-mode shape as LDAR/LDAPR.
        //
        // Deliberately does NOT also decode plain STR/STUR (unlike decode_arm64_load, which does
        // cover the equivalent plain-load forms): a broadened version covering those was tried for
        // handle_general_memory_violation's Category-3 "false fault" case and caused a real,
        // reproducible hang (confirmed via bisection - isolated to this table specifically, not
        // decode_arm64_load's equivalent broadening, which the KUSD/MMIO path already exercises
        // safely) whose exact root cause wasn't pinned down before time ran out on the investigation.
        // Category-3 false-fault emulation is therefore currently limited to loads and STLR-family
        // stores; a plain-store false fault falls through to a crash instead of being emulated.
        //
        // This has a real, understood downstream consequence beyond just that crash, tracked
        // separately: handle_general_memory_violation also uses this same decoder to classify a
        // fault's memory_operation (write vs. read) for the *genuine* protection-violation dispatch
        // path, not just the false-fault path - so a plain guest store to a legitimately read-only
        // page is misclassified as a read, which can route it into the false-fault recovery above
        // instead of correctly raising STATUS_ACCESS_VIOLATION to the guest. See the plan file /
        // tracked follow-up for the full analysis; not fixed here since a safe fix needs a
        // classification-only check independent of this decode table (which is unsafe to broaden,
        // per the hang above) and that hasn't been implemented/verified yet.
        struct decoded_arm64_store
        {
            uint32_t size = 0; // bytes: 1, 2, 4, 8
            uint32_t rt = 0;
        };

        std::optional<decoded_arm64_store> decode_arm64_store(const uint32_t insn)
        {
            struct encoding
            {
                uint32_t value;
                uint32_t size;
            };

            static constexpr encoding stores[] = {
                {0x089FFC00U, 1}, // STLRB
                {0x489FFC00U, 2}, // STLRH
                {0x889FFC00U, 4}, // STLR Wt
                {0xC89FFC00U, 8}, // STLR Xt
            };

            const uint32_t rt = insn & 0x1FU;
            const uint32_t fixed = insn & 0xFFFFFC00U;

            for (const auto& enc : stores)
            {
                if (fixed == enc.value)
                {
                    return decoded_arm64_store{enc.size, rt};
                }
            }

            return std::nullopt;
        }
#endif

        bool is_page_aligned(const uint64_t value)
        {
            return (value & (page_size - 1)) == 0;
        }

        int to_prot(const memory_permission permissions)
        {
            int prot = PROT_NONE;
            if ((permissions & memory_permission::read) != memory_permission::none)
            {
                prot |= PROT_READ;
            }
            if ((permissions & memory_permission::write) != memory_permission::none)
            {
                prot |= PROT_WRITE;
            }
            if ((permissions & memory_permission::exec) != memory_permission::none)
            {
                prot |= PROT_EXEC;
            }
            return prot;
        }

#ifdef __APPLE__
        // Apple Silicon's kernel categorically refuses simultaneous write+exec on any mapping that
        // isn't MAP_JIT-backed (mprotect fails outright with EACCES) - unlike Linux, where W^X for
        // guest memory is advisory at best. This is independent of the 16KB/4KB reconciliation
        // above: even a single guest region directly requesting RWX hits it, which real PE loaders
        // do routinely (map .text RWX to patch ASLR relocations, then narrow to RX before the module
        // ever executes). Favoring write over exec here handles that common, well-defined sequence
        // correctly; it would be wrong for a page that is genuinely written and executed in the same
        // window without an intervening apply_memory_protection call, which is not how real PE
        // loading behaves.
        int to_prot_apple(const memory_permission permissions)
        {
            int prot = to_prot(permissions);
            if ((prot & PROT_WRITE) && (prot & PROT_EXEC))
            {
                prot &= ~PROT_EXEC;
            }
            return prot;
        }
#endif

        // Bit-for-bit reimplementation of FEXCore::Context::ContextImpl::ReconstructCompactedEFLAGS /
        // SetFlagsFromCompactedEFLAGS (FEXCore's Core.cpp), operating directly on a CPUState instead
        // of a live InternalThreadState. FEXCore's originals unconditionally dereference the Thread
        // pointer to reach CurrentFrame->State, which doesn't exist yet for the staged CPUState the
        // Windows loader populates before the first start()/create_thread(). The NZCV bit positions
        // below (28-31) mirror FEXCore's fixed IR::OpDispatchBuilder::IndexNZCV mapping, an internal
        // compiler detail with no public header.
        uint32_t index_nzcv(unsigned bit_offset)
        {
            switch (bit_offset)
            {
            case FEXCore::X86State::RFLAG_OF_RAW_LOC:
                return 28;
            case FEXCore::X86State::RFLAG_CF_RAW_LOC:
                return 29;
            case FEXCore::X86State::RFLAG_ZF_RAW_LOC:
                return 30;
            case FEXCore::X86State::RFLAG_SF_RAW_LOC:
                return 31;
            default:
                return 0;
            }
        }

        uint32_t reconstruct_compacted_eflags(const FEXCore::Core::CPUState& state)
        {
            uint32_t eflags = 0;

            for (size_t i = 0; i < FEXCore::Core::CPUState::NUM_EFLAG_BITS; ++i)
            {
                switch (i)
                {
                case FEXCore::X86State::RFLAG_CF_RAW_LOC:
                case FEXCore::X86State::RFLAG_PF_RAW_LOC:
                case FEXCore::X86State::RFLAG_AF_RAW_LOC:
                case FEXCore::X86State::RFLAG_TF_RAW_LOC:
                case FEXCore::X86State::RFLAG_ZF_RAW_LOC:
                case FEXCore::X86State::RFLAG_SF_RAW_LOC:
                case FEXCore::X86State::RFLAG_OF_RAW_LOC:
                case FEXCore::X86State::RFLAG_DF_RAW_LOC:
                    break;
                default:
                    eflags |= uint32_t{state.flags[i]} << i;
                    break;
                }
            }

            uint32_t packed_nzcv = 0;
            std::memcpy(&packed_nzcv, &state.flags[FEXCore::X86State::RFLAG_NZCV_LOC], sizeof(packed_nzcv));

            const uint32_t of = (packed_nzcv >> index_nzcv(FEXCore::X86State::RFLAG_OF_RAW_LOC)) & 1;
            uint32_t cf = (packed_nzcv >> index_nzcv(FEXCore::X86State::RFLAG_CF_RAW_LOC)) & 1;
            const uint32_t zf = (packed_nzcv >> index_nzcv(FEXCore::X86State::RFLAG_ZF_RAW_LOC)) & 1;
            const uint32_t sf = (packed_nzcv >> index_nzcv(FEXCore::X86State::RFLAG_SF_RAW_LOC)) & 1;
            cf ^= 1;

            eflags |= of << FEXCore::X86State::RFLAG_OF_RAW_LOC;
            eflags |= cf << FEXCore::X86State::RFLAG_CF_RAW_LOC;
            eflags |= zf << FEXCore::X86State::RFLAG_ZF_RAW_LOC;
            eflags |= sf << FEXCore::X86State::RFLAG_SF_RAW_LOC;

            const uint32_t pf_byte = state.pf_raw & 0xff;
            const uint32_t pf = static_cast<uint32_t>(std::popcount(pf_byte ^ 1u)) & 1;
            eflags |= pf << FEXCore::X86State::RFLAG_PF_RAW_LOC;

            const uint32_t af = ((state.af_raw ^ pf_byte) & (1 << 4)) ? 1 : 0;
            eflags |= af << FEXCore::X86State::RFLAG_AF_RAW_LOC;

            const uint8_t tf_byte = state.flags[FEXCore::X86State::RFLAG_TF_RAW_LOC];
            eflags |= (tf_byte & 1) << FEXCore::X86State::RFLAG_TF_RAW_LOC;

            const uint8_t df_byte = state.flags[FEXCore::X86State::RFLAG_DF_RAW_LOC];
            if (df_byte & 0x80)
            {
                eflags |= 1 << FEXCore::X86State::RFLAG_DF_RAW_LOC;
            }

            return eflags;
        }

        void set_flags_from_compacted_eflags(FEXCore::Core::CPUState& state, uint32_t eflags)
        {
            for (size_t i = 0; i < FEXCore::Core::CPUState::NUM_EFLAG_BITS; ++i)
            {
                switch (i)
                {
                case FEXCore::X86State::RFLAG_OF_RAW_LOC:
                case FEXCore::X86State::RFLAG_CF_RAW_LOC:
                case FEXCore::X86State::RFLAG_ZF_RAW_LOC:
                case FEXCore::X86State::RFLAG_SF_RAW_LOC:
                    break;
                case FEXCore::X86State::RFLAG_AF_RAW_LOC:
                    state.af_raw = (eflags & (1U << i)) ? (1 << 4) : 0;
                    break;
                case FEXCore::X86State::RFLAG_PF_RAW_LOC:
                    state.pf_raw = (eflags & (1U << i)) ? 0 : 1;
                    break;
                case FEXCore::X86State::RFLAG_DF_RAW_LOC:
                    state.flags[i] = (eflags & (1U << i)) ? 0xff : 1;
                    break;
                default:
                    state.flags[i] = (eflags & (1U << i)) ? 1 : 0;
                    break;
                }
            }

            uint32_t packed_nzcv = 0;
            packed_nzcv |=
                (eflags & (1U << FEXCore::X86State::RFLAG_OF_RAW_LOC)) ? 1U << index_nzcv(FEXCore::X86State::RFLAG_OF_RAW_LOC) : 0U;
            packed_nzcv |=
                (eflags & (1U << FEXCore::X86State::RFLAG_CF_RAW_LOC)) ? 0U : 1U << index_nzcv(FEXCore::X86State::RFLAG_CF_RAW_LOC);
            packed_nzcv |=
                (eflags & (1U << FEXCore::X86State::RFLAG_ZF_RAW_LOC)) ? 1U << index_nzcv(FEXCore::X86State::RFLAG_ZF_RAW_LOC) : 0U;
            packed_nzcv |=
                (eflags & (1U << FEXCore::X86State::RFLAG_SF_RAW_LOC)) ? 1U << index_nzcv(FEXCore::X86State::RFLAG_SF_RAW_LOC) : 0U;
            std::memcpy(&state.flags[FEXCore::X86State::RFLAG_NZCV_LOC], &packed_nzcv, sizeof(packed_nzcv));

            state.flags[FEXCore::X86State::RFLAG_RESERVED_LOC] = 1;
            state.flags[FEXCore::X86State::RFLAG_IF_LOC] = 1;
        }

        struct mapped_region
        {
            size_t size = 0;
            memory_permission permissions = memory_permission::none;
            bool owned = true; // false for map_host_memory aliases we must not munmap
        };

        // FEX runs the guest natively with guest VA == host VA, so there is no per-instruction hook to
        // intercept a specific address the way an interpreted backend can - every access to this
        // backend's only current MMIO consumer, KUSER_SHARED_DATA, would otherwise cost a real
        // hardware fault + signal round-trip (measured as the dominant cost of a whole run when guest
        // code polls it at high frequency, e.g. a loading-screen timer). At its raw guest address
        // (0x7ffe0000) the region falls inside __PAGEZERO and truly cannot be backed by real memory
        // (confirmed via both mmap(MAP_FIXED) and mach_vm_allocate) - but for a wow64 process it is
        // rebased above 4GB like any other sub-4GB guest address (see wow64_guest_rebase), where real
        // backing is possible. map_mmio opportunistically mmaps read-only real memory there and keeps
        // it fresh once per quantum (see refresh_mmio_backings); host_backing stays null wherever the
        // real mapping isn't possible (e.g. a native 64-bit process), and the region falls back to the
        // original always-unmapped, fault-and-emulate behavior via handle_mmio_fault. Read-only either
        // way: this backend's only consumer treats guest writes as a no-op, so a write here is left to
        // surface as a normal access violation, matching real Windows (KUSER_SHARED_DATA is read-only
        // user-mapped memory there too) - not a general MMIO implementation.
        struct mmio_region
        {
            uint64_t address = 0;
            size_t size = 0;
            mmio_read_callback read_cb;
            void* host_backing = nullptr;
            size_t host_backing_size = 0;
        };

        struct hook_entry
        {
            x86_hookable_instructions type = x86_hookable_instructions::invalid;
            instruction_hook_callback callback;
        };

#ifdef __APPLE__
        bool sysctl_flag(const char* name)
        {
            int32_t value = 0;
            size_t size = sizeof(value);
            if (::sysctlbyname(name, &value, &size, nullptr, 0) != 0)
            {
                return false;
            }
            return value != 0;
        }

        // Real Apple Silicon feature detection via sysctlbyname's hw.optional.arm.* namespace, in
        // place of FEXCore's own FetchHostFeatures() (Linux-only: reads MIDR_EL1, which isn't
        // EL0-readable/trap-emulated on Darwin the way arm64 Linux's kernel does it). Confirmed
        // present on Apple Silicon (M-series) via `sysctl -a | grep hw.optional`; anything not
        // confirmed present, or without a clear ARM-feature mapping, is conservatively left false -
        // that only costs codegen quality, never correctness.
        FEXCore::HostFeatures fetch_host_features_apple()
        {
            FEXCore::HostFeatures features{};

            uint64_t cache_line_size = 64;
            size_t cache_line_size_len = sizeof(cache_line_size);
            ::sysctlbyname("hw.cachelinesize", &cache_line_size, &cache_line_size_len, nullptr, 0);
            features.DCacheLineSize = static_cast<uint32_t>(cache_line_size);
            features.ICacheLineSize = static_cast<uint32_t>(cache_line_size);
            features.SupportsCacheMaintenanceOps = true;

            features.SupportsAES = sysctl_flag("hw.optional.arm.FEAT_AES");
            features.SupportsCRC = sysctl_flag("hw.optional.arm.FEAT_CRC32");
            features.SupportsAtomics = sysctl_flag("hw.optional.arm.FEAT_LSE");
            features.SupportsRCPC = sysctl_flag("hw.optional.arm.FEAT_LRCPC");
            features.SupportsRAND = sysctl_flag("hw.optional.arm.FEAT_RNG");
            features.SupportsSHA = sysctl_flag("hw.optional.arm.FEAT_SHA1") && sysctl_flag("hw.optional.arm.FEAT_SHA256");
            features.SupportsPMULL_128Bit = sysctl_flag("hw.optional.arm.FEAT_PMULL");
            features.SupportsCSSC = sysctl_flag("hw.optional.arm.FEAT_CSSC");
            features.SupportsFCMA = sysctl_flag("hw.optional.arm.FEAT_FCMA");
            features.SupportsFlagM = sysctl_flag("hw.optional.arm.FEAT_FlagM");
            features.SupportsFlagM2 = sysctl_flag("hw.optional.arm.FEAT_FlagM2");
            features.SupportsRPRES = sysctl_flag("hw.optional.arm.FEAT_RPRES");
            features.SupportsFRINTTS = sysctl_flag("hw.optional.arm.FEAT_FRINTTS");
            features.SupportsECV = sysctl_flag("hw.optional.arm.FEAT_ECV");
            features.SupportsWFXT = sysctl_flag("hw.optional.arm.FEAT_WFxT");
            features.SupportsAFP = sysctl_flag("hw.optional.arm.FEAT_AFP");
            features.SupportsMOPS = sysctl_flag("hw.optional.arm.FEAT_MOPS");

            // No Apple Silicon hardware supports SVE/SVE2 as of this writing.
            features.SupportsSVE128 = false;
            features.SupportsSVE256 = false;
            features.SupportsAVX = false;
            features.SupportsSVEBitPerm = false;

            // TPIDRRO_EL0 is not confirmed to carry a CPU index on Darwin the way arm64 Linux's
            // kernel populates it; the fork's DEF_OP(ProcessorID) treats the non-TPIDRRO fallback as
            // unsupported (matching the Windows/wine precedent), so leaving this false means a guest
            // RDTSCP/RDPID will hard-error rather than silently read garbage. Known gap, not a
            // correctness risk: real-world guest code rarely depends on RDTSCP/RDPID succeeding.
            features.SupportsCPUIndexInTPIDRRO = false;

            // FEXCore's CPUID brand-string leaves (0x80000002-4) index PerCPUData, whose size is
            // CPUMIDRs.size(); an empty CPUMIDRs leaves PerCPUData empty and the leaf null-derefs
            // its ProductName. Linux's FetchHostFeatures() populates this by reading MIDR_EL1 per
            // core, which is unavailable from EL0 on Darwin. Report the host logical-CPU count with
            // a placeholder MIDR of 0: the M-series parts aren't in FEXCore's MIDR table anyway, so
            // it resolves to the "Unknown ARM CPU" brand string (FEXCore's own fallback) rather
            // than crashing, and keeps the guest-visible core count (derived from Cores) realistic.
            uint32_t logical_cpus = 1;
            size_t logical_cpus_len = sizeof(logical_cpus);
            if (::sysctlbyname("hw.logicalcpu", &logical_cpus, &logical_cpus_len, nullptr, 0) != 0 || logical_cpus == 0)
            {
                logical_cpus = 1;
            }
            features.CPUMIDRs.assign(logical_cpus, 0u);

            return features;
        }

        // sogen runs one FEX-backed guest thread per process (the cooperative, single-emulation-
        // -host-thread model - see the class-level comment), so a single active-instance pointer is
        // enough for the signal handler below to reach the emulator's hook tables/thread state. Real
        // signal handlers can't be non-static member functions, so this indirection is required.
        fex_x86_64_emulator* g_active_emulator = nullptr;

        void fault_signal_handler(int sig, siginfo_t* info, void* raw_ucontext);

        void install_fault_signal_handlers(fex_x86_64_emulator& emulator)
        {
            g_active_emulator = &emulator;

            // A dedicated alternate signal stack (SA_ONSTACK), so a second, different signal
            // (SIGBUS/SIGILL) arriving while this handler is already executing on the faulting
            // thread's normal stack doesn't have to nest on that same, potentially near-exhausted,
            // faulting stack.
            static std::byte alt_stack[64 * 1024];
            stack_t ss{};
            ss.ss_sp = alt_stack;
            ss.ss_size = sizeof(alt_stack);
            ss.ss_flags = 0;
            ::sigaltstack(&ss, nullptr);

            struct sigaction action = {};
            action.sa_sigaction = fault_signal_handler;
            action.sa_flags = SA_SIGINFO | SA_ONSTACK;
            sigemptyset(&action.sa_mask);

            ::sigaction(SIGSEGV, &action, nullptr);
            ::sigaction(SIGBUS, &action, nullptr);
            ::sigaction(SIGILL, &action, nullptr);

            // FEXCore's IR "Break" op (see handle_fault_signal's FaultToTopAndGeneratedException
            // comment) models x86 HLT/UD2/INT3/INT1/INTO/unhandled-INT-N uniformly via distinct native
            // trap instructions chosen per Dispatcher.cpp's GuestSignal_SIG* stubs: HLT/UDF raise
            // SIGILL, BRK raises SIGTRAP. INT3 (x86_64_dbgbreak/DebugBreak()) goes through the SIGTRAP
            // stub - without a handler registered here, that BRK was an entirely unhandled hardware
            // trap, terminating the process (exit 128+SIGTRAP) instead of reaching the vector-dispatch
            // logic below, which already handles vector 3 (breakpoint) correctly once it runs.
            ::sigaction(SIGTRAP, &action, nullptr);
        }

        // FEXCore::CPU::Arm64JITCore::ExitFunctionLink (JIT.cpp) patches an already-compiled call
        // site once its target block becomes known - self-modifying code, writing directly into a
        // MAP_JIT code buffer. It doesn't bracket that write with a JIT-write-protect toggle (see
        // FEXCore::Allocator::JITWriteScope) because it's written for Linux, which has no per-thread
        // W^X state to worry about; on Apple Silicon the write faults with a real (not synthetic)
        // protection violation, since MAP_JIT enforces write-XOR-execute per calling thread, not per
        // mapping. FEXCore::HLE::CpuStateFrame::Pointers.ExitFunctionLink is a plain function-pointer
        // slot JIT-compiled code calls through (see JIT.cpp's InitThreadPointers), so it can be
        // intercepted here with a toggling wrapper instead of touching deps/FEX.
        uint64_t g_original_exit_function_link = 0;

        uint64_t exit_function_link_jit_write_wrapper(FEXCore::Core::CpuStateFrame* frame, void* record)
        {
            using exit_function_link_fn = uint64_t (*)(FEXCore::Core::CpuStateFrame*, void*);
            const auto real = reinterpret_cast<exit_function_link_fn>(g_original_exit_function_link);

            ::pthread_jit_write_protect_np(0);
            const uint64_t result = real(frame, record);
            ::pthread_jit_write_protect_np(1);
            return result;
        }

        // ===========================================================================================
        // FEXCore-internal host allocation arena (Apple, guest VA == host VA).
        //
        // This backend runs guest VA == host VA, so any host allocation the kernel is free to place
        // wherever it likes can land inside the guest's own address space. FEXCore obtains all of its
        // internal buffers - the BlockLinks red-black-tree storage (fextl monotonic buffer resource),
        // the JIT CodeBuffer, and the dispatcher - through FEXCore::Allocator::mmap(nullptr, ...) (via
        // Allocator::VirtualAlloc), which by default is a raw ::mmap(NULL, ...) with zero coordination
        // with sogen's guest bookkeeping. When one of those buffers happens to be placed at a host
        // address the guest also owns, an ordinary guest SIMD/vector store silently overwrites
        // FEXCore's own bookkeeping - the root cause of the long-standing __tree_balance_after_insert /
        // AddBlockLink corruption (a wild guest store scribbling a live BlockLinks tree node with
        // packed 16-bit-lane vector data).
        //
        // Fix: reserve one large host arena up-front, register its whole range as a reserved_host_range
        // (so sogen's memory manager steers every guest allocation away from it - a genuine two-way
        // exclusion), and satisfy every FEXCore::Allocator::mmap(nullptr, ...) request from inside it.
        // Non-executable requests are placed with MAP_FIXED over the reserved region. Executable
        // (MAP_JIT) requests cannot use MAP_FIXED - Apple rejects MAP_JIT|MAP_FIXED with EINVAL - so
        // the sub-region is unmapped first and MAP_JIT is requested with the hole's address as a
        // (non-fixed) hint, which the kernel reliably honors because nothing else competes for space
        // inside the arena. The result is verified to land inside the arena or the request fails
        // loudly (ENOMEM) rather than silently falling back to an unconstrained mapping that would
        // reintroduce the exact aliasing hazard this exists to prevent.
        // ===========================================================================================
        class fex_internal_arena
        {
          public:
            // Sized to comfortably exceed any realistic FEXCore-internal need (BlockLinks growth, the
            // JIT CodeBuffer and its growth, the dispatcher) for a full game/application workload.
            // Pure VA reservation (PROT_NONE) until sub-regions are actually committed.
            static constexpr size_t arena_size = 0x1'0000'0000ULL; // 4 GiB

            static fex_internal_arena& instance()
            {
                static fex_internal_arena arena;
                return arena;
            }

            // Reserve the arena and install the FEXCore allocator hooks. Must run before the first
            // FEXCore-internal allocation (context/CodeBuffer creation) and before the memory
            // manager first queries reserved_host_ranges(). Idempotent.
            //
            // This runs before notify_process_bitness (bitness isn't known yet at CPU-backend
            // construction time), so if the kernel's ASLR happens to place this arena's own
            // mmap(nullptr, ...) inside [wow64_guest_rebase, wow64_guest_rebase +
            // wow64_guest_address_space_size), that later reservation (see
            // reserve_wow64_host_window's doc comment) finds the window already occupied - by us -
            // and skips itself, silently losing the fix it exists to provide. Bitness isn't known
            // here, so just always avoid that range for the arena's own placement (harmless for a
            // non-wow64 process too - the arena has no reason to prefer any particular address).
            // Bounded retry: an ordinary mmap(nullptr, ...) with no hint gets a fresh ASLR pick each
            // call, so a handful of retries converges quickly; give up and accept the collision
            // rather than spin forever on a determined adversary (in which case
            // reserve_wow64_host_window's own probe-and-skip still keeps this safe, just without
            // the improvement).
            void install()
            {
                if (this->base_ != 0)
                {
                    return;
                }

                void* base = MAP_FAILED;
                constexpr int max_attempts = 8;
                for (int attempt = 0; attempt < max_attempts; ++attempt)
                {
                    void* candidate = ::mmap(nullptr, arena_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                    if (candidate == MAP_FAILED)
                    {
                        break;
                    }

                    const auto candidate_addr = reinterpret_cast<uint64_t>(candidate);
                    const auto candidate_end = candidate_addr + arena_size;
                    const bool overlaps_wow64_window = candidate_addr < wow64_guest_rebase_default + wow64_guest_address_space_size &&
                                                       candidate_end > wow64_guest_rebase_default;
                    if (!overlaps_wow64_window)
                    {
                        base = candidate;
                        break;
                    }

                    ::munmap(candidate, arena_size);
                }

                if (base == MAP_FAILED)
                {
                    throw std::runtime_error("Failed to reserve FEXCore-internal host arena");
                }

                this->base_ = reinterpret_cast<uintptr_t>(base);
                this->cursor_ = this->base_;

                FEXCore::Allocator::mmap = &fex_internal_arena::hook_mmap;
                FEXCore::Allocator::munmap = &fex_internal_arena::hook_munmap;
            }

            uintptr_t base() const
            {
                return this->base_;
            }

            size_t size() const
            {
                return arena_size;
            }

            bool active() const
            {
                return this->base_ != 0;
            }

          private:
            uintptr_t base_ = 0;
            uintptr_t cursor_ = 0; // bump pointer, monotonically increasing within the arena
            std::mutex lock_;

            struct free_block
            {
                uintptr_t addr;
                size_t size;
            };

            std::vector<free_block> free_list_;

            bool owns(const void* p) const
            {
                const auto a = reinterpret_cast<uintptr_t>(p);
                return this->base_ != 0 && a >= this->base_ && a < this->base_ + arena_size;
            }

            // Reserve `size` bytes of VA inside the arena. Reuses a freed block (first fit, splitting
            // any remainder back onto the free list) before extending the bump cursor. Returns 0 on
            // exhaustion. Caller holds lock_.
            uintptr_t reserve(const size_t size)
            {
                for (auto it = this->free_list_.begin(); it != this->free_list_.end(); ++it)
                {
                    if (it->size >= size)
                    {
                        const auto addr = it->addr;
                        if (it->size > size)
                        {
                            it->addr += size;
                            it->size -= size;
                        }
                        else
                        {
                            this->free_list_.erase(it);
                        }
                        return addr;
                    }
                }

                if (this->cursor_ + size > this->base_ + arena_size)
                {
                    return 0;
                }
                const auto addr = this->cursor_;
                this->cursor_ += size;
                return addr;
            }

            void* allocate(void* addr, size_t length, int prot, int flags, int fd, off_t offset)
            {
                // Only kernel-choose anonymous requests need steering into the arena. Fixed-address or
                // file-backed requests are honored verbatim - FEXCore issues a fixed-address request
                // only where it deliberately targets a specific base it has already reserved itself.
                if (addr != nullptr || (flags & MAP_ANONYMOUS) == 0)
                {
                    return ::mmap(addr, length, prot, flags, fd, offset);
                }

                const size_t rounded = host_page_align_up_apple(length);

                std::lock_guard<std::mutex> guard(this->lock_);
                const uintptr_t slot = this->reserve(rounded);
                if (slot == 0)
                {
                    fprintf(stderr,
                            "[FEX backend] FATAL: FEXCore-internal host arena exhausted (%zu MiB); "
                            "refusing an unconstrained host mmap that could alias guest memory\n",
                            arena_size >> 20);
                    errno = ENOMEM;
                    return MAP_FAILED;
                }

                if (flags & MAP_JIT)
                {
                    // FEXCore's CodeBuffer sizes its request to include a trailing guard page at the
                    // very end (see CPUBackend.h's UsableSize(): AllocatedSize - FEX_HOST_PAGE_SIZE) and
                    // tries to mprotect(PROT_NONE) that last page itself. That mprotect() call always
                    // fails on Apple Silicon (EACCES) - MAP_JIT protection can only be fixed at mmap()
                    // time, not adjusted after the fact by an ordinary mprotect() - which is exactly the
                    // "Failed to mprotect last page of code buffer" diagnostic CPUBackend.cpp logs.
                    // Provide a REAL guard here instead: make only the leading portion (excluding that
                    // final host page) the actual executable MAP_JIT mapping, and simply never touch the
                    // trailing page - it stays part of the arena's permanent PROT_NONE reservation, which
                    // genuinely faults on any access, landing exactly where UsableSize() already expects
                    // the buffer to end. `Ptr`/`AllocatedSize` as seen by the caller are unaffected; only
                    // how much of that byte range is truly writable/executable changes. Skipped for
                    // requests no bigger than one host page (e.g. Dispatcher's fixed-size, guardless
                    // buffer) so a small allocation isn't shrunk into uselessness.
                    const size_t exec_size = rounded > host_page_size_apple ? rounded - host_page_size_apple : rounded;

                    // MAP_JIT | MAP_FIXED is rejected on Apple, so punch a hole in the arena and place
                    // the executable mapping there via a (non-fixed) address hint the kernel honors.
                    ::munmap(reinterpret_cast<void*>(slot), exec_size);
                    void* result = ::mmap(reinterpret_cast<void*>(slot), exec_size, prot, flags, fd, offset);
                    if (result == reinterpret_cast<void*>(slot))
                    {
                        return result;
                    }

                    // The kernel didn't honor the hint. A mapping outside the arena would reintroduce
                    // the aliasing hazard, so fail loudly instead of handing it back.
                    if (result != MAP_FAILED)
                    {
                        ::munmap(result, exec_size);
                    }
                    // Restore the arena's PROT_NONE reservation over the whole slot (including the guard
                    // portion) so it never becomes an unmapped gap the guest could be handed.
                    ::mmap(reinterpret_cast<void*>(slot), rounded, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
                    this->free_list_.push_back({slot, rounded});
                    fprintf(stderr, "[FEX backend] FATAL: could not place MAP_JIT code buffer inside the FEXCore arena\n");
                    errno = ENOMEM;
                    return MAP_FAILED;
                }

                // Non-executable: commit directly over the reserved region.
                return ::mmap(reinterpret_cast<void*>(slot), rounded, prot, flags | MAP_FIXED, fd, offset);
            }

            int release(void* addr, size_t length)
            {
                if (!this->owns(addr))
                {
                    return ::munmap(addr, length);
                }

                const size_t rounded = host_page_align_up_apple(length);

                std::lock_guard<std::mutex> guard(this->lock_);
                // Return the region to the reserved (PROT_NONE) state so it stays part of the arena's
                // contiguous reservation, and record it for reuse. Arena VA is never returned to the OS.
                void* r = ::mmap(addr, rounded, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
                if (r == addr)
                {
                    this->free_list_.push_back({reinterpret_cast<uintptr_t>(addr), rounded});
                    return 0;
                }
                return r == MAP_FAILED ? -1 : 0;
            }

            static void* hook_mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset)
            {
                return instance().allocate(addr, length, prot, flags, fd, offset);
            }

            static int hook_munmap(void* addr, size_t length)
            {
                return instance().release(addr, length);
            }
        };
#endif
    }

    class fex_x86_64_emulator;

    // -----------------------------------------------------------------------------------------------
    // The syscall handler bridges FEX's guest `syscall` exits to the registered instruction hook.
    // Method bodies are defined out of line (after fex_x86_64_emulator is complete) since they touch
    // the emulator's internals.
    // -----------------------------------------------------------------------------------------------
    class fex_syscall_handler final : public FEXCore::HLE::SyscallHandler
    {
      public:
        explicit fex_syscall_handler(fex_x86_64_emulator& emulator)
            : emulator_(emulator)
        {
            // OS_GENERIC: FEX does no JIT-side syscall argument handling and spills/fills all registers,
            // which is what we want since the syscall is serviced entirely by sogen's own hook.
            this->OSABI = FEXCore::HLE::SyscallOSABI::OS_GENERIC;
        }

        uint64_t HandleSyscall(FEXCore::Core::CpuStateFrame* frame, FEXCore::HLE::SyscallArguments* args) override;
        FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(FEXCore::Core::InternalThreadState* thread, uint64_t address) override;
        std::optional<FEXCore::ExecutableFileSectionInfo> LookupExecutableFileSection(FEXCore::Core::InternalThreadState* thread,
                                                                                      uint64_t guest_addr) override;

      private:
        fex_x86_64_emulator& emulator_;
    };

    // -----------------------------------------------------------------------------------------------
    // The emulator itself.
    // -----------------------------------------------------------------------------------------------
    class fex_x86_64_emulator final : public x86_64_emulator
    {
      public:
        fex_x86_64_emulator()
        {
            this->initialize_context();
        }

        ~fex_x86_64_emulator() override
        {
            utils::reset_object_with_delayed_destruction(this->memory_read_hooks_);
            utils::reset_object_with_delayed_destruction(this->memory_write_hooks_);
            utils::reset_object_with_delayed_destruction(this->memory_execution_hooks_);
            utils::reset_object_with_delayed_destruction(this->memory_violation_hooks_);
            utils::reset_object_with_delayed_destruction(this->interrupt_hooks_);
            utils::reset_object_with_delayed_destruction(this->basic_block_hooks_);
            utils::reset_object_with_delayed_destruction(this->instruction_hooks_);

            if (this->thread_ != nullptr && this->context_)
            {
                this->context_->DestroyThread(this->thread_);
                this->thread_ = nullptr;
            }

            // thread32_ is never actually created yet (see its doc comment) - this mirrors the
            // 64-bit teardown above defensively, for whenever Phase B starts creating it.
            if (this->thread32_ != nullptr && this->context32_)
            {
                this->context32_->DestroyThread(this->thread32_);
                this->thread32_ = nullptr;
            }

            // Release everything we mmap'd into the (host == guest, modulo wow64_guest_rebase in
            // 32-bit mode) address space.
            for (const auto& [address, region] : this->regions_)
            {
                if (region.owned)
                {
                    const auto rebase = rebase_for(this->is_wow64_process_, address);
                    ::munmap(reinterpret_cast<void*>(address + rebase), region.size);
                }
            }

            // Per-logical-thread call-ret buffers (see ensure_callret_buffer) live in host allocator
            // space, not regions_, and are never freed as individual guest threads exit - release them
            // all here so they don't outlive this emulator instance.
            for (const auto& [alloc_base, alloc_size] : this->callret_buffers_)
            {
                FEXCore::Allocator::munmap(alloc_base, alloc_size);
            }
        }

        // --[ cpu_interface ]------------------------------------------------------------------------

        bool read_descriptor_table(int reg, descriptor_table_register& table) override
        {
            // FEX is a user-mode emulator: there is no real IDT, and the GDT is synthesized internally.
            // Only report the GDT base we were handed via load_gdt(); everything else is unsupported.
            if (reg == static_cast<int>(x86_register::gdtr))
            {
                table.base = this->gdt_base_;
                table.limit = this->gdt_limit_;
                return true;
            }
            return false;
        }

        void start(size_t count) override
        {
            this->refresh_mmio_backings();

            if (count != 0)
            {
                // FEX has CompileRIPCount() for bounded execution, but wiring exact instruction counts
                // through the JIT exit path is non-trivial; match the KVM backend and refuse for now.
                throw std::runtime_error("FEX backend does not support exact instruction counts yet");
            }

            if (this->active_thread_ == nullptr)
            {
                this->create_thread();
            }

            this->stop_requested_ = false;
            // Re-arm InterruptFaultPage for this quantum - see request_thread_stop's doc comment; a
            // prior stop() may have left it protected to force the last quantum's ExecuteThread to
            // return, and it must be writable again before the JIT's per-block-entry store runs.
            ::mprotect(this->active_thread_->InterruptFaultPage, sizeof(this->active_thread_->InterruptFaultPage), PROT_READ | PROT_WRITE);

            // ExecuteThread runs the translated guest until the thread is asked to stop (which the
            // syscall bridge does when a hook calls stop()), or the guest faults/exits.
#ifdef __APPLE__
            // On this platform it can also return early because handle_fault_signal deferred a hook
            // dispatch (see pending_fault_dispatch_'s doc comment) rather than a genuine stop -
            // dispatch it here, in normal call context where it's actually safe to do so, then simply
            // resume by calling ExecuteThread again (it always (re-)starts fresh from
            // CurrentFrame->State.rip, which the hook is free to have redirected), unless the hook
            // itself asked to stop.
            for (;;)
            {
                this->active_context_->ExecuteThread(this->active_thread_);

                if (!this->dispatch_pending_hook_if_any() || this->stop_requested_)
                {
                    break;
                }

                // A deferred hook is resuming (dispatch returned true, no stop pending). If it was a
                // WoW64 gate crossing, active_thread_ was just swapped to the OTHER FEXCore engine
                // mid-quantum. Each engine owns a distinct InterruptFaultPage (the cooperative-stop
                // mechanism - see request_thread_stop), but this quantum's re-arm above only touched
                // the engine active at entry. The newly-active engine's page may still be PROT_NONE
                // from a PRIOR quantum's stop (e.g. another logical thread yielded while running this
                // same shared 32-bit engine), which would make its very first block-entry interrupt
                // check fault and unwind ExecuteThread as a spurious "stop" - the second-32-bit-thread
                // startup failure at LdrInitializeThunk. Re-arm the now-active engine's page here (in
                // normal call context, and only on the continue path where no stop is pending) so it
                // resumes cleanly.
                ::mprotect(this->active_thread_->InterruptFaultPage, sizeof(this->active_thread_->InterruptFaultPage),
                           PROT_READ | PROT_WRITE);
            }
#else
            this->active_context_->ExecuteThread(this->active_thread_);
#endif
        }

        void stop() override
        {
            this->stop_requested_ = true;
            this->request_thread_stop();
        }

        size_t read_raw_register(int reg, void* value, size_t size) override
        {
            const auto xreg = static_cast<x86_register>(reg);
            const auto mapping = detail::map_register(xreg);
            auto& state = this->cpu_state();

            switch (mapping.kind)
            {
            case detail::register_kind::gpr: {
                // In a WoW64 process the 32-bit engine (context32_) has no architectural r8-r15: 32-bit
                // x86 cannot address them, and when the 32-bit engine takes a real fault its SRA spill
                // leaves those greg slots holding host register values (observed as host stack pointers).
                // The meaningful high-register state - the wow64cpu-reserved r12-r15 (r14 = the 64-bit
                // exception stack, r13 = CpuArea CONTEXT block, ...) that a 64-bit CONTEXT capture needs -
                // lives in the frozen 64-bit engine (thread_), maintained by the forward gate. Source
                // r8-r15 from there so dispatch_exception's CONTEXT64 (consumed by ntdll!
                // KiUserExceptionDispatcher -> wow64!Wow64PrepareForException) carries the real values.
                const FEXCore::Core::CPUState& gpr_state =
                    (this->is_wow64_process_ && this->active_thread_ == this->thread32_ && this->thread_ != nullptr &&
                     mapping.gpr.index >= detail::greg_r8 && mapping.gpr.index <= detail::greg_r8 + 7)
                        ? this->thread_->CurrentFrame->State
                        : state;
                uint64_t raw = gpr_state.gregs[mapping.gpr.index] >> (mapping.gpr.byte_offset * 8);
                std::memcpy(value, &raw, (std::min)(size, mapping.gpr.width));
                return size;
            }
            case detail::register_kind::rip:
                std::memcpy(value, &state.rip, (std::min)(size, sizeof(state.rip)));
                return size;
            case detail::register_kind::flags: {
                const uint64_t rflags = this->read_rflags();
                std::memcpy(value, &rflags, (std::min)(size, sizeof(rflags)));
                return size;
            }
            case detail::register_kind::xmm:
                // Low 128 bits of the (possibly AVX) vector register.
                std::memcpy(value, &state.xmm.avx.data[mapping.index][0], (std::min)(size, size_t{16}));
                return size;
            case detail::register_kind::mm:
                std::memcpy(value, &state.mm[mapping.index][0], (std::min)(size, size_t{16}));
                return size;
            case detail::register_kind::mxcsr:
                std::memcpy(value, &state.mxcsr, (std::min)(size, sizeof(state.mxcsr)));
                return size;
            case detail::register_kind::fcw:
                std::memcpy(value, &state.FCW, (std::min)(size, sizeof(state.FCW)));
                return size;
            case detail::register_kind::fs_base:
                std::memcpy(value, &state.fs_cached, (std::min)(size, sizeof(state.fs_cached)));
                return size;
            case detail::register_kind::gs_base:
                std::memcpy(value, &state.gs_cached, (std::min)(size, sizeof(state.gs_cached)));
                return size;
            case detail::register_kind::segment: {
                const uint16_t selector = this->segment_selector(mapping.index);
                std::memcpy(value, &selector, (std::min)(size, sizeof(selector)));
                return size;
            }
            case detail::register_kind::fsw:
            case detail::register_kind::unsupported:
            default:
                // Unknown/unsupported register: report zeroed value rather than throwing, matching the
                // lenient behavior of the other backends for rarely-used registers.
                std::memset(value, 0, size);
                return size;
            }
        }

        size_t write_raw_register(int reg, const void* value, size_t size) override
        {
            const auto xreg = static_cast<x86_register>(reg);
            const auto mapping = detail::map_register(xreg);
            auto& state = this->cpu_state();

            switch (mapping.kind)
            {
            case detail::register_kind::gpr: {
                auto& slot = state.gregs[mapping.gpr.index];
                if (mapping.gpr.width == 8)
                {
                    std::memcpy(&slot, value, sizeof(slot));
                }
                else if (mapping.gpr.zero_extend_32)
                {
                    uint32_t v = 0;
                    std::memcpy(&v, value, sizeof(v));
                    slot = v; // 32-bit writes clear the high 32 bits
                }
                else
                {
                    uint64_t incoming = 0;
                    std::memcpy(&incoming, value, mapping.gpr.width);
                    const auto shift = mapping.gpr.byte_offset * 8;
                    const uint64_t mask = ((1ULL << (mapping.gpr.width * 8)) - 1) << shift;
                    slot = (slot & ~mask) | ((incoming << shift) & mask);
                }
                return size;
            }
            case detail::register_kind::rip:
                std::memcpy(&state.rip, value, (std::min)(size, sizeof(state.rip)));
                return size;
            case detail::register_kind::flags: {
                uint64_t rflags = 0;
                std::memcpy(&rflags, value, (std::min)(size, sizeof(rflags)));
                this->write_rflags(rflags);
                return size;
            }
            case detail::register_kind::xmm:
                std::memcpy(&state.xmm.avx.data[mapping.index][0], value, (std::min)(size, size_t{16}));
                return size;
            case detail::register_kind::mm:
                std::memcpy(&state.mm[mapping.index][0], value, (std::min)(size, size_t{16}));
                return size;
            case detail::register_kind::mxcsr:
                std::memcpy(&state.mxcsr, value, (std::min)(size, sizeof(state.mxcsr)));
                return size;
            case detail::register_kind::fcw:
                std::memcpy(&state.FCW, value, (std::min)(size, sizeof(state.FCW)));
                return size;
            case detail::register_kind::fs_base:
                std::memcpy(&state.fs_cached, value, (std::min)(size, sizeof(state.fs_cached)));
                return size;
            case detail::register_kind::gs_base:
                std::memcpy(&state.gs_cached, value, (std::min)(size, sizeof(state.gs_cached)));
                return size;
            case detail::register_kind::segment:
                this->set_segment_selector(mapping.index, value, size);
                return size;
            case detail::register_kind::fsw:
            case detail::register_kind::unsupported:
            default:
                return size;
            }
        }

        // Extended WoW64 register-snapshot layout (see save_registers/restore_registers). A wow64
        // logical thread carries state in BOTH FEXCore engines simultaneously - the active one plus a
        // "parked" excursion frame in the other (a 32-bit thread mid-32-bit-code leaves its last 64-bit
        // RunSimulatedCode dispatch frame frozen in thread_; a thread mid-64-bit-syscall leaves its
        // last 32-bit state frozen in thread32_). Both engines are single, shared instances multiplexed
        // across every logical thread, so a snapshot of only the active engine loses the parked frame,
        // which the next logical thread to run that engine then overwrites.
        static constexpr size_t kWow64SnapshotHeader = 8; // uint64 active-is-32 flag, kept 8 for alignment

        static constexpr size_t wow64_snapshot_size()
        {
            return kWow64SnapshotHeader + 2 * sizeof(FEXCore::Core::CPUState);
        }

        std::vector<std::byte> save_registers() const override
        {
            // For a wow64 process, once the 32-bit engine exists a logical thread's full state spans
            // BOTH engines (active + parked). Snapshot both, tagged with which one is active, so a
            // thread switch preserves the parked excursion frame instead of leaking it to whichever
            // logical thread next runs the shared engine.
            if (this->is_wow64_process_ && this->thread32_ != nullptr && this->thread_ != nullptr)
            {
                std::vector<std::byte> data(wow64_snapshot_size());
                const uint64_t active_is_32 = (this->active_context_ == this->context32_.get()) ? 1 : 0;
                std::memcpy(data.data(), &active_is_32, sizeof(active_is_32));
                std::memcpy(data.data() + kWow64SnapshotHeader, &this->thread_->CurrentFrame->State, sizeof(FEXCore::Core::CPUState));
                std::memcpy(data.data() + kWow64SnapshotHeader + sizeof(FEXCore::Core::CPUState), &this->thread32_->CurrentFrame->State,
                            sizeof(FEXCore::Core::CPUState));
                return data;
            }

            // The whole architectural state lives in a single CPUState struct; snapshot it verbatim.
            const auto& state = this->cpu_state();
            std::vector<std::byte> data(sizeof(FEXCore::Core::CPUState));
            std::memcpy(data.data(), &state, sizeof(state));
            return data;
        }

        // Copies a saved CPUState blob into one engine's live frame while preserving the fields that are
        // genuinely per-FEXCore-engine-global rather than per-logical-guest-thread. L1Pointer/L1Mask (the
        // JIT lookup-cache pointers) are rewritten by FEXCore itself when the cache reallocates, so the
        // value already live in CurrentFrame->State is always the correct one - keep it across the memcpy
        // rather than letting a stale/foreign snapshot clobber it. callret_sp/_pad1 are handled by
        // ensure_callret_stack (see its doc comment).
        void restore_state_into(FEXCore::Core::InternalThreadState* thread, const std::byte* src)
        {
            auto& state = thread->CurrentFrame->State;
            const auto l1_pointer = state.L1Pointer;
            const auto l1_mask = state.L1Mask;
            std::memcpy(&state, src, sizeof(FEXCore::Core::CPUState));
            state.L1Pointer = l1_pointer;
            state.L1Mask = l1_mask;
            this->ensure_callret_buffer(state);
            thread->CallRetStackBase = reinterpret_cast<void*>(state._pad1);
        }

        void restore_registers(const std::vector<std::byte>& register_data) override
        {
            // Extended wow64 snapshot: restore BOTH engines (active + parked) and select the active one
            // from the saved flag. This is what keeps each logical thread's parked excursion frame
            // (the frozen state in whichever engine it is NOT currently running) intact across a thread
            // switch - without it, the reverse/forward gate later reads the OTHER logical thread's
            // residual engine state (stale TEB64/rsp) and mis-marshals, corrupting the 64-bit dispatch
            // stack (a wild 64-bit ret into a 32-bit-range address).
            if (register_data.size() == wow64_snapshot_size())
            {
                if (this->thread_ == nullptr)
                {
                    throw std::runtime_error("Extended wow64 snapshot restored before the 64-bit engine exists");
                }
                if (this->thread32_ == nullptr)
                {
                    this->create_thread32();
                }
                uint64_t active_is_32 = 0;
                std::memcpy(&active_is_32, register_data.data(), sizeof(active_is_32));
                this->restore_state_into(this->thread_, register_data.data() + kWow64SnapshotHeader);
                this->restore_state_into(this->thread32_, register_data.data() + kWow64SnapshotHeader + sizeof(FEXCore::Core::CPUState));
                if (active_is_32)
                {
                    this->active_context_ = this->context32_.get();
                    this->active_thread_ = this->thread32_;
                }
                else
                {
                    this->active_context_ = this->context_.get();
                    this->active_thread_ = this->thread_;
                }
                return;
            }

            if (register_data.size() != sizeof(FEXCore::Core::CPUState))
            {
                throw std::runtime_error("FEX register snapshot has unexpected size");
            }

            if (this->active_thread_ == nullptr)
            {
                // No thread yet: writing into staged_state_, which create_thread() will seed the
                // real thread from (including installing L1Pointer/L1Mask/callret_sp correctly
                // itself afterward) - a verbatim copy here is fine.
                std::memcpy(&this->staged_state_, register_data.data(), sizeof(FEXCore::Core::CPUState));
                return;
            }

            // Single-CPUState snapshot: the process is either pure-64-bit (native - always thread_) or a
            // wow64 thread that has not yet run 32-bit code (default_register_set / a freshly-seeded
            // thread, cs=0x33). Route by the saved cs selector (0x23 = 32-bit compat) so the state lands
            // in the matching engine. Strict no-op for a pure 64-bit process (is_wow64_process_ false ->
            // always thread_, already active).
            const auto incoming_cs = reinterpret_cast<const FEXCore::Core::CPUState*>(register_data.data())->cs_idx;
            const bool incoming_is_32bit = this->is_wow64_process_ && incoming_cs == 0x23;
            if (incoming_is_32bit)
            {
                if (this->thread32_ == nullptr)
                {
                    this->create_thread32();
                }
                this->active_context_ = this->context32_.get();
                this->active_thread_ = this->thread32_;
            }
            else
            {
                this->active_context_ = this->context_.get();
                this->active_thread_ = this->thread_;
            }

            this->restore_state_into(this->active_thread_, register_data.data());
        }

        bool has_violation() const override
        {
            return false;
        }

        bool supports_instruction_counting() const override
        {
            return false;
        }

        // FEXCore's INT3 handling (OpcodeDispatcher.cpp) sets SetRIPToNext, so the RIP observed once
        // the breakpoint fault surfaces here is already one past the 0xCC byte - unlike KVM/WHP, which
        // both catch INT3 at the instruction's own (pre-advance) address. See
        // reports_breakpoint_rip_past_instruction's doc comment.
        bool reports_breakpoint_rip_past_instruction() const override
        {
            return true;
        }

        // FEXCore maintains separate context_/thread_ (64-bit) and context32_/thread32_ (32-bit)
        // engines for a WoW64 process, only one of which is active at a time (see
        // perform_bitness_switch) - see has_separate_bitness_engines' doc comment for why this
        // matters for the WoW64 NtContinue reverse-gate.
        bool has_separate_bitness_engines() const override
        {
            return true;
        }

        // request_thread_stop() mprotects InterruptFaultPage to PROT_NONE, which is safe to call from
        // any host thread - the software-quantum watchdog thread in windows_emulator::start() relies on
        // exactly this (supports_instruction_counting() is false, so that path is the only time-slicing
        // mechanism available). Matches KVM's reasoning for the same accessor.
        bool is_stop_thread_safe() const override
        {
            return true;
        }

        // --[ emulator ]-----------------------------------------------------------------------------

        std::string get_name() const override
        {
            return "FEX";
        }

        bool supports_multiple_vcpus() const override
        {
            // sogen multiplexes logical guest threads onto a single FEXCore engine per bitness
            // (context_/context32_), cooperatively scheduled on one host thread - no multi-vCPU support.
            return false;
        }

        void serialize_state(utils::buffer_serializer& buffer, bool /*is_snapshot*/) const override
        {
            buffer.write_vector(this->save_registers());
            // TODO(fex): a full snapshot should also persist the mapped-memory layout and contents so a
            // restore can re-mmap and refill the (host == guest) address space. Registers-only for now.
        }

        void deserialize_state(utils::buffer_deserializer& buffer, bool /*is_snapshot*/) override
        {
            this->restore_registers(buffer.read_vector<std::byte>());
        }

        // --[ x86_emulator ]-------------------------------------------------------------------------

        void set_segment_base(x86_register base, pointer_type value) override
        {
            auto& state = this->cpu_state();
            if (base == x86_register::fs || base == x86_register::fs_base)
            {
                state.fs_cached = value;
            }
            else if (base == x86_register::gs || base == x86_register::gs_base)
            {
                // gs_cached stays a plain, logical (unrebased) guest address - deps/FEX's own JIT
                // now applies the wow64 rebase itself, conditionally, for GS-relative (and any
                // other) memory accesses under context_ when CONFIG_WOW64GUESTREBASE is set (see
                // ensure_context32's sibling call, notify_process_bitness, and
                // OpDispatchBuilder::GuestMemoryRebase()/Addressing.cpp in deps/FEX) - no embedder-
                // side adjustment needed here.
                state.gs_cached = value;
            }
        }

        pointer_type get_segment_base(x86_register base) override
        {
            const auto& state = this->cpu_state();
            if (base == x86_register::fs || base == x86_register::fs_base)
            {
                return state.fs_cached;
            }
            if (base == x86_register::gs || base == x86_register::gs_base)
            {
                return state.gs_cached;
            }
            return 0;
        }

        // Called once, before load_gdt() or create_thread(), right after the windows-emulator layer
        // determines the process's execution mode (see arch_emulator.hpp's doc comment on this
        // virtual). context_ (see its doc comment) is ALWAYS the 64-bit FEXCore::Context, wow64 or
        // not - see the WoW64 plan's "DEFINITIVE FINDING": a real wow64 process's thread genuinely
        // starts executing real 64-bit ntdll code before any 32-bit code ever runs, so rebuilding
        // context_ itself as 32-bit here (the old design) made the JIT mis-decode that unavoidable
        // 64-bit startup code as 32-bit garbage. is_wow64_process_ only gates the memory-interface-
        // level wow64_guest_rebase (needed regardless of which FEXCore::Context is executing, since
        // the 32-bit executable/ntdll32 modules live in the low address range either way) and
        // whether context32_ gets stood up at all; it no longer selects context_'s own bitness.
        void notify_process_bitness(bool is_wow64_process) override
        {
            this->is_wow64_process_ = is_wow64_process;
            // Tell context_'s JIT to conditionally rebase low (<4GB) addresses too - see
            // SetNeedsWow64GuestRebase's doc comment (public Context.h) for why this can't just be
            // CONFIG_IS64BIT_MODE's existing unconditional-rebase behavior: context_ stays 64-bit,
            // whose addresses aren't restricted to any range, so ordinary heap/stack/module
            // accesses must NOT be rebased - only content sogen deliberately placed below 4GB
            // (the wow64 TEB pair, wow64cpu.dll, the heaven's-gate trampoline) needs it.
            this->context_->SetNeedsWow64GuestRebase(is_wow64_process);
            if (is_wow64_process)
            {
                this->ensure_context32();
            }
        }

#ifdef __APPLE__
        // Picks a genuinely free 4GB host window for sub-4GB guest addresses to rebase into, and
        // reserves it up front as PROT_NONE before FEXCore or anything else in this process can
        // lazily claim any part of it - storing the choice in wow64_guest_rebase_. Deliberately does
        // NOT tell context_ about it here: context_ doesn't exist yet at this call site (this must
        // run before CreateNewContext() to have any chance of winning the race against FEXCore's own
        // internal allocations - see below), so initialize_context() calls
        // context_->SetWow64GuestRebaseValue(wow64_guest_rebase_) itself once context_ exists,
        // right after constructing it.
        //
        // A SINGLE FIXED candidate (the original design, always [0x400000000, 0x500000000)) is not
        // safe here: what host VA is actually free depends on what else this specific process/
        // machine has already mapped, and that varies with the host in ways no compile-time constant
        // can anticipate. Two real, independently-measured collisions with a fixed candidate:
        //   - This process links Cocoa/Metal (sogen's own GPU/window subsystem). Its dyld-load-time
        //     host VA reservation (up to ~12GB) is unbeatable from application code (confirmed: moving
        //     this reservation as early as initialize_context() made no measurable difference), and
        //     was observed clustering tightly within [0x418000000, 0x4fa000000] across 100+ runs on
        //     one machine - entirely inside the original [16GB, 20GB) candidate, causing "Failed to
        //     write FEX guest memory" at kCodeBase (the heaven's-gate trampoline) in ~20-40% of runs.
        //   - Simply moving the candidate higher (128GB) traded one fixed collision for another: this
        //     machine (64GB RAM) also has an always-present, non-ASLR'd ~384GB reservation starting at
        //     exactly its own physical RAM size (0x1000000000) - some RAM-proportional heuristic
        //     (malloc/VM compressor/similar), NOT a Metal/GPU artifact. A larger-RAM machine would push
        //     this reservation's start (and thus what it covers) even higher, so no single fixed
        //     candidate is safe across different host RAM sizes either.
        // So this tries a sequence of candidates, live-probed via mach_vm_region, jumping past
        // whatever occupies each rejected one (using the occupant's own reported extent, so a huge
        // reservation is skipped in one step rather than walked past 4GB at a time) until one is
        // found genuinely empty - bounded by both a candidate-count cap and an address ceiling chosen
        // to stay comfortably below AddressSanitizer's shadow-memory floor (~0x7e00000000 on macOS/
        // arm64 - see reserved_host_ranges()'s ASan-skip comment) for instrumented builds.
        //
        // Called once, unconditionally, from initialize_context() - this backend's own construction,
        // before FEXCore's context/CodeBuffer exist and before any guest or guest-triggered code has
        // run - regardless of whether this process turns out to be wow64 at all (bitness isn't known
        // this early; harmless for a plain 64-bit guest, which never rebases anything - see
        // rebase_for). This is also as early as this backend's own code can possibly run: moving it
        // even earlier isn't possible from here, since Cocoa/Metal's own reservation happens via dyld
        // loading the frameworks before main() - before ANY of this process's own C++ constructors,
        // sogen's included - even runs at all.
        //
        // Falls back to leaving wow64_guest_rebase_/wow64_host_window_reserved_ at their defaults
        // (unchanged, existing detect-and-retry behavior) if every candidate is exhausted - this can
        // only ever improve on that baseline, never regress it.
        //
        // Deliberately NOT surfaced as a "reserved" range via reserved_host_ranges()/
        // reserved_host_ranges_in() (contrast with fex_internal_arena, which IS surfaced there) -
        // unlike the arena, this window IS guest address space; guest memory is meant to live here.
        // A guest's own fixed-address mmap(MAP_FIXED) call silently overwrites this PROT_NONE
        // placeholder at the kernel level with no help needed. The only failure mode to avoid is
        // sogen's OWN collision detection mistaking this placeholder for a foreign occupant and
        // refusing the legitimate guest allocation that's supposed to land there - exactly the
        // regression reserved_host_ranges()'s doc comment documents for a different range (an
        // earlier fix reserved [0, first_hit) unconditionally and broke install_wow64_heaven_gate's
        // fixed allocate_memory(kCodeBase, ...) call outright, which has no fallback search to skip
        // past a conflicting reservation). Reuse/re-allocation correctness for addresses actually
        // inside this window is already handled independently by memory_manager's own
        // reserved_regions_/overlaps_reserved_region bookkeeping - these two functions only need to
        // stop reporting the window as "foreign" at all, which the skip checks below do.
        void reserve_wow64_host_window()
        {
            if (this->wow64_host_window_reserved_)
            {
                return;
            }

            constexpr uint64_t search_ceiling = 0x8000000000ULL; // 512 GiB
            constexpr int max_candidates = 32;

            uint64_t candidate = wow64_guest_rebase_default;
            for (int attempt = 0; attempt < max_candidates && candidate + wow64_guest_address_space_size <= search_ceiling; ++attempt)
            {
                mach_vm_address_t probe_addr = candidate;
                mach_vm_size_t probe_size = 0;
                vm_region_basic_info_data_64_t info{};
                mach_msg_type_number_t info_count = VM_REGION_BASIC_INFO_COUNT_64;
                mach_port_t object_name = MACH_PORT_NULL;
                const kern_return_t probe_result = mach_vm_region(mach_task_self(), &probe_addr, &probe_size, VM_REGION_BASIC_INFO_64,
                                                                  reinterpret_cast<vm_region_info_t>(&info), &info_count, &object_name);

                const bool candidate_is_free = probe_result != KERN_SUCCESS || probe_addr >= candidate + wow64_guest_address_space_size;
                if (!candidate_is_free)
                {
                    char path_buf[PROC_PIDPATHINFO_MAXSIZE] = {};
                    const int path_len = proc_regionfilename(getpid(), probe_addr, path_buf, sizeof(path_buf));
                    fprintf(stderr,
                            "[FEX backend] wow64 host window candidate [0x%llx, 0x%llx) occupied (mapping at 0x%llx "
                            "size=0x%llx prot=%d file=%s) - trying the next candidate\n",
                            static_cast<unsigned long long>(candidate),
                            static_cast<unsigned long long>(candidate + wow64_guest_address_space_size),
                            static_cast<unsigned long long>(probe_addr), static_cast<unsigned long long>(probe_size), info.protection,
                            path_len > 0 ? path_buf : "<none>");

                    const uint64_t occupant_end = probe_addr + probe_size;
                    candidate = (occupant_end + wow64_guest_address_space_size - 1) & ~(wow64_guest_address_space_size - 1);
                    continue;
                }

                void* const target = reinterpret_cast<void*>(candidate);
                void* const result =
                    ::mmap(target, wow64_guest_address_space_size, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                if (result != target)
                {
                    // A racer claimed this exact candidate between our probe and our mmap - move on.
                    fprintf(stderr, "[FEX backend] failed to reserve wow64 host window at 0x%llx - trying the next candidate\n",
                            static_cast<unsigned long long>(candidate));
                    if (result != MAP_FAILED)
                    {
                        ::munmap(result, wow64_guest_address_space_size);
                    }
                    candidate += wow64_guest_address_space_size;
                    continue;
                }

                this->wow64_guest_rebase_ = candidate;
                this->wow64_host_window_reserved_ = true;
                return;
            }

            fprintf(stderr,
                    "[FEX backend] exhausted %d candidates below 0x%llx searching for a free wow64 host window - "
                    "falling back to the default at 0x%llx with detect-and-retry\n",
                    max_candidates, static_cast<unsigned long long>(search_ceiling),
                    static_cast<unsigned long long>(wow64_guest_rebase_default));
        }
#endif

        void mark_guest_range_permanently_non_executable(pointer_type address, size_t size) override
        {
            this->permanently_non_executable_ranges_.emplace_back(address, size);
        }

        void register_gate_crossing(pointer_type address, size_t size, gate_crossing_kind kind) override
        {
            // A gate is inherently non-executable to the JIT (QueryGuestExecutableRange consults
            // gate_crossings_ directly, so reaching it raises FEXCore's synthetic #PF before any
            // byte there is compiled) - no separate mark_guest_range_permanently_non_executable call
            // is needed. handle_fault_signal's vector-14 path then recognizes the faulting RIP as a
            // gate and performs the actual crossing instead of dispatching a memory violation.
            this->gate_crossings_.push_back(gate_crossing{address, size, kind});
        }

        void load_gdt(pointer_type address, uint32_t limit) override
        {
            // Only remember the base/limit for callers querying gdtr (see read_descriptor_table).
            this->gdt_base_ = address;
            this->gdt_limit_ = limit;

            // sogen writes real GDT descriptors (matching FEXCore::Core::CPUState::gdt_segment's
            // bitfield layout byte-for-byte) directly into guest memory at `address`. Since guest VA
            // == host VA under this backend's model, point FEX's own segment table at that same
            // memory instead of duplicating it - CS/segment lookups (GetSegmentFromIndex) then see
            // whatever sogen's loader wrote, including the long-mode (L) bit, with no extra sync step.
            // In 32-bit mode this is a DIRECT pointer assignment FEXCore dereferences as a host
            // address outside of any guest instruction - it bypasses the JIT-side
            // WOW64_GUEST_REBASE logic entirely, so the rebase must be applied here explicitly (see
            // wow64_guest_rebase's doc comment) - though in practice GDT_ADDR (process_context.hpp)
            // already lives well above the rebase threshold on Apple Silicon, so this evaluates to 0.
            const auto rebase = rebase_for(this->is_wow64_process_, address);
            this->cpu_state().segment_arrays[0] = reinterpret_cast<FEXCore::Core::CPUState::gdt_segment*>(address + rebase);
        }

        // --[ memory_interface (public) ]------------------------------------------------------------

        void read_memory(uint64_t address, void* data, size_t size) const override
        {
            if (!this->try_read_memory(address, data, size))
            {
                throw std::runtime_error("Failed to read FEX guest memory");
            }
        }

        bool try_read_memory(uint64_t address, void* data, size_t size) const override
        {
            if (!this->is_range_mapped(address, size))
            {
                return false;
            }
            // Guest VA == host VA under FEX for 64-bit guests. For 32-bit (WoW64) guests, real host
            // memory instead lives at address + wow64_guest_rebase (see that constant's doc comment) -
            // regions_/is_range_mapped stay keyed by the plain guest address throughout.
            const auto rebase = rebase_for(this->is_wow64_process_, address);
            // memmove, not memcpy: callers (e.g. the PE loader's section copy) can hand this a source
            // buffer that overlaps the guest destination range - overlapping memcpy is undefined
            // behaviour. This is a generic guest memory-copy primitive with no non-overlap contract.
            std::memmove(data, reinterpret_cast<const void*>(address + rebase), size);
            return true;
        }

        void write_memory(uint64_t address, const void* data, size_t size) override
        {
            if (!this->try_write_memory(address, data, size))
            {
                char buf[128];
                snprintf(buf, sizeof(buf), "Failed to write FEX guest memory at 0x%llx size=0x%zx wow64=%d",
                         static_cast<unsigned long long>(address), size, this->is_wow64_process_ ? 1 : 0);
                throw std::runtime_error(buf);
            }
        }

        bool try_write_memory(uint64_t address, const void* data, size_t size) override
        {
            return this->try_write_memory_impl(address, data, size, /*invalidate_translations=*/true);
        }

        // Writes WoW64 gate-crossing CPU-state marshaling data (the CpuArea i386-CONTEXT block at
        // TEB64+0x1488+0x80, and the 64-bit RunSimulatedCode stack frame) into guest memory. These
        // targets are pure data structures that never hold JIT-compiled guest code, so the code-cache
        // invalidation try_write_memory normally performs is a guaranteed no-op in every FEXCore
        // context - skip it to avoid a GetCodeInvalidationMutex lock + Apple pthread_jit_write_protect
        // toggle pair per marshaled register (~25 per WoW64 syscall round-trip). If such an address
        // were ever repurposed as code, map_memory/apply_memory_protection would invalidate independently.
        bool write_marshal_state(uint64_t address, const void* data, size_t size)
        {
            return this->try_write_memory_impl(address, data, size, /*invalidate_translations=*/false);
        }

        bool try_write_memory_impl(uint64_t address, const void* data, size_t size, bool invalidate_translations)
        {
            if (!this->is_range_mapped(address, size))
            {
                return false;
            }

            // sogen's own loader writes guest memory it has already declared read-only (e.g. a PE
            // section's raw file bytes, before/regardless of the section's final protection). Unlike
            // Unicorn's uc_mem_write, which operates on emulated memory with no real host enforcement,
            // FEX's guest-VA==host-VA model is backed by actual host mprotect state, so such a write
            // needs a temporary permission bump around the memcpy.
            const memory_permission declared = this->permission_covering(address);
            const bool needs_temporary_write = (declared & memory_permission::write) == memory_permission::none;

            if (needs_temporary_write)
            {
                this->set_temporary_write_access(address, size, declared, true);
            }

            // See try_read_memory's doc comment on wow64_guest_rebase.
            const auto rebase = rebase_for(this->is_wow64_process_, address);
            // memmove, not memcpy: see try_read_memory - the source may overlap the guest destination.
            std::memmove(reinterpret_cast<void*>(address + rebase), data, size);

            if (needs_temporary_write)
            {
                this->set_temporary_write_access(address, size, declared, false);
            }

            if (invalidate_translations)
            {
                // Writing to a mapped region may overwrite already-translated code; drop FEX's cache for it.
                this->invalidate_code_range(address, size);
            }
            return true;
        }

        // --[ hook_interface ]-----------------------------------------------------------------------
        //
        // Like the KVM backend, FEX runs the guest natively, so fine-grained memory/execution/basic-
        // block hooks cannot fire. They are accepted (and tracked, so delete_hook works) for API
        // compatibility. Only instruction hooks for `syscall` are actually wired (see the syscall
        // bridge). cpuid/rdtsc could later be wired through FEX's CPUID/TSC override hooks.

        emulator_hook* hook_memory_execution(memory_execution_hook_callback callback) override
        {
            auto* hook = this->make_hook();
            this->memory_execution_hooks_[hook] = std::move(callback);
            return hook;
        }

        emulator_hook* hook_memory_execution(uint64_t /*address*/, memory_execution_hook_callback callback) override
        {
            auto* hook = this->make_hook();
            this->memory_execution_hooks_[hook] = std::move(callback);
            return hook;
        }

        emulator_hook* hook_memory_range_execution(uint64_t /*address*/, uint64_t /*size*/,
                                                   memory_execution_hook_callback callback) override
        {
            auto* hook = this->make_hook();
            this->memory_execution_hooks_[hook] = std::move(callback);
            return hook;
        }

        emulator_hook* hook_memory_read(uint64_t /*address*/, uint64_t /*size*/, memory_access_hook_callback callback) override
        {
            auto* hook = this->make_hook();
            this->memory_read_hooks_[hook] = std::move(callback);
            return hook;
        }

        emulator_hook* hook_memory_write(uint64_t /*address*/, uint64_t /*size*/, memory_access_hook_callback callback) override
        {
            auto* hook = this->make_hook();
            this->memory_write_hooks_[hook] = std::move(callback);
            return hook;
        }

        emulator_hook* hook_instruction(int instruction_type, instruction_hook_callback callback) override
        {
            auto* hook = this->make_hook();
            auto& entry = this->instruction_hooks_[hook];
            entry.type = static_cast<x86_hookable_instructions>(instruction_type);
            entry.callback = std::move(callback);
            if (entry.type == x86_hookable_instructions::syscall)
            {
                this->syscall_hook_ = &entry;
            }
            return hook;
        }

        emulator_hook* hook_interrupt(interrupt_hook_callback callback) override
        {
            auto* hook = this->make_hook();
            this->interrupt_hooks_[hook] = std::move(callback);
            return hook;
        }

        emulator_hook* hook_memory_violation(memory_violation_hook_callback callback) override
        {
            auto* hook = this->make_hook();
            this->memory_violation_hooks_[hook] = std::move(callback);
            return hook;
        }

        emulator_hook* hook_basic_block(basic_block_hook_callback callback) override
        {
            auto* hook = this->make_hook();
            this->basic_block_hooks_[hook] = std::move(callback);
            return hook;
        }

        void delete_hook(emulator_hook* hook) override
        {
            if (this->syscall_hook_ != nullptr)
            {
                const auto it = this->instruction_hooks_.find(hook);
                if (it != this->instruction_hooks_.end() && &it->second == this->syscall_hook_)
                {
                    this->syscall_hook_ = nullptr;
                }
            }

            this->instruction_hooks_.erase(hook);
            this->interrupt_hooks_.erase(hook);
            this->memory_read_hooks_.erase(hook);
            this->memory_write_hooks_.erase(hook);
            this->memory_execution_hooks_.erase(hook);
            this->memory_violation_hooks_.erase(hook);
            this->basic_block_hooks_.erase(hook);
        }

        bool supports_global_memory_execution_hooks() const override
        {
            // Native execution: global execution hooks would require single-stepping the JIT.
            return false;
        }

#ifdef __APPLE__
        std::vector<host_reserved_range> reserved_host_ranges() const override
        {
            // Guest VA == host VA, so this process's own memory (its loaded image, dyld, shared
            // libraries, thread stacks, heap) shares the same address space the guest uses - unlike
            // Unicorn/Icicle/KVM, which sandbox or translate the guest address space independently.
            // Enumerate everything currently mapped in this process via the Mach VM region API (the
            // Darwin equivalent of walking /proc/self/maps) so the memory manager can steer guest
            // allocations away from it. One-shot snapshot: see the interface doc comment for the
            // residual risk of host allocations made after this call.
            std::vector<host_reserved_range> ranges;

            // Every 64-bit Mach-O executable reserves a __PAGEZERO segment spanning at least [0, 4GB)
            // to make null-pointer dereferences fault. It's an OS/linker convention enforced at the
            // mmap syscall level (MAP_FIXED requests anywhere in this low range are refused), not
            // something that shows up as a discoverable, listed VM region via mach_vm_region below -
            // confirmed empirically: mach_vm_region's first real hit starts well above 4GB (its exact
            // position shifts with ASLR), yet mapping guest memory anywhere in the gap below it still
            // fails. Reserve the whole gap up to wherever the scan's first real region actually
            // starts, rather than guessing a fixed size.
            //
            // Always reserved from wow64_guest_address_space_size (4GB) up, wow64 process or not:
            // even for a wow64 process, real 64-bit modules (ntdll/win32u/wow64*.dll, see
            // module_manager::load_wow64_modules) execute under context_ (always the 64-bit
            // FEXCore::Context - see notify_process_bitness's doc comment), which applies NO
            // internal rebase of its own - their host backing must be genuinely mappable at their
            // raw guest address, so find_free_allocation_base must steer clear of this gap for them
            // exactly like it does for an ordinary 64-bit-only process. (Confirmed by a real
            // regression: with this gap left unreserved for wow64 processes, real ntdll's own
            // preferred-base placement fell back to find_free_allocation_base(...,
            // DEFAULT_ALLOCATION_ADDRESS_64BIT) - exactly 4GB, i.e. still inside this gap - and the
            // resulting raw, unrebased host mmap failed outright.)
            //
            // NOT reserved below 4GB, even for a wow64 process: unlike the plain 64-bit-only case,
            // sub-4GB guest addresses in a wow64 process are exactly this backend's
            // wow64_guest_rebase-shifted territory (the 32-bit executable/ntdll32, AND, just as
            // importantly, low-but-still-64-bit-content deliberately placed under 4GB for 32-bit-
            // pointer reachability - wow64cpu.dll via must_map_module_below_4gb, and this backend's
            // own fixed-address heaven's-gate trampoline at kCodeBase/0xFF300000,
            // wow64_heaven_gate.hpp) - all of it gets a real, valid, rebased host address up at
            // [wow64_guest_rebase, wow64_guest_rebase + 4GB) regardless, so none of it ever actually
            // touches this gap at the host level. (Confirmed by a second regression: reserving all
            // of [0, first_hit) unconditionally, as an earlier version of this fix did, blocked
            // install_wow64_heaven_gate's fixed allocate_memory(kCodeBase, ...) call outright, since
            // that call has no fallback search to skip past a conflicting reservation.)
            // The FEXCore-internal arena (see fex_internal_arena) is a live mapping the Mach scan
            // below would otherwise report as many separate sub-regions (PROT_NONE reservation, the
            // committed BlockLinks buffers, the MAP_JIT CodeBuffer, freed holes...). Skip all of them
            // and register the whole arena as a single reserved range instead, so the guest steers
            // clear of every part of it - including sub-regions allocated lazily after this one-shot
            // snapshot and any transient unmapped holes - with no dependence on the scan's timing.
            const auto& arena = fex_internal_arena::instance();
            const uint64_t arena_base = arena.base();
            const uint64_t arena_end = arena.active() ? arena_base + arena.size() : 0;
            if (arena.active())
            {
                ranges.push_back({.address = arena_base, .size = arena.size()});
            }

            mach_vm_address_t address = 0;
            bool first_region = true;
            while (true)
            {
                mach_vm_size_t size = 0;
                vm_region_basic_info_data_64_t info{};
                mach_msg_type_number_t info_count = VM_REGION_BASIC_INFO_COUNT_64;
                mach_port_t object_name = MACH_PORT_NULL;
                const kern_return_t result = mach_vm_region(mach_task_self(), &address, &size, VM_REGION_BASIC_INFO_64,
                                                            reinterpret_cast<vm_region_info_t>(&info), &info_count, &object_name);
                if (result != KERN_SUCCESS)
                {
                    break;
                }

                // Sub-regions of the FEXCore-internal arena are covered by the single explicit range
                // pushed above; don't double-report them (harmless but avoids overlap churn).
                if (arena.active() && address >= arena_base && address < arena_end)
                {
                    address += size;
                    continue;
                }

                // See reserve_wow64_host_window's doc comment: unlike the arena above, this window
                // is deliberately NOT added as a reserved range at all - it's guest address space,
                // and memory_manager's own reserved_regions_ already tracks whatever sogen has
                // legitimately placed inside it. Just don't let this one-shot scan report it as a
                // foreign occupant (whether it's still our own PROT_NONE placeholder or already-
                // committed guest content, neither is foreign).
                if (this->wow64_host_window_reserved_ && address >= this->wow64_guest_rebase_ &&
                    address < this->wow64_guest_rebase_ + wow64_guest_address_space_size)
                {
                    address += size;
                    continue;
                }

                if (first_region)
                {
                    const uint64_t gap_start = this->is_wow64_process_ ? wow64_guest_address_space_size : 0;
                    if (address > gap_start)
                    {
                        ranges.push_back({.address = gap_start, .size = static_cast<size_t>(address - gap_start)});
                    }
                    first_region = false;
                }

                // AddressSanitizer reserves an enormous, sparse shadow-memory map: individual
                // regions spanning tens of GB up to multiple TB, placed high in the address space
                // (0x600000000000+ and ~0x7e00000000 on macOS/arm64). They sit far above where the
                // guest actually allocates during execution, but feeding them to the memory manager
                // as reserved ranges bloats reserved_regions_ into the thousands, turning its
                // per-allocation O(n) overlap scans into an O(n^2) stall during process setup. Skip
                // these giant reservations in instrumented builds only; release builds are unaffected.
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
                constexpr mach_vm_size_t asan_shadow_region_threshold = 0x100000000ULL; // 4 GiB
                if (size >= asan_shadow_region_threshold)
                {
                    address += size;
                    continue;
                }
#endif
#endif

                ranges.push_back({.address = address, .size = static_cast<size_t>(size)});
                address += size;
            }
            return ranges;
        }

        std::vector<host_reserved_range> reserved_host_ranges_in(uint64_t address, size_t size) const override
        {
            // Targeted equivalent of reserved_host_ranges() for a single query window. The fixed-
            // address allocate_memory overload only needs to know whether THIS window has been
            // claimed by a foreign host mapping since sogen last released it - not to re-enumerate
            // every region in the process, whose count (and thus that walk's cost) grows unbounded
            // over a long session. mach_vm_region's start-address parameter lets the kernel skip
            // straight to the first region at or above the (rebased) window, so this visits only
            // regions actually inside the window (usually none).
            //
            // The arena and the __PAGEZERO gap are captured into reserved_regions_ by the first full
            // scan at startup and never released, so overlaps_reserved_region already rejects a
            // target landing in them without help here; the only thing a rescan of an otherwise-free
            // window can add is a foreign mapping in a gap an earlier guest unmap munmap'd back to
            // the OS - which is exactly what a bare "is anything mapped in this host window" probe finds.
            std::vector<host_reserved_range> ranges;

            const auto rebase = rebase_for(this->is_wow64_process_, address);

            // See reserve_wow64_host_window's doc comment. A non-zero rebase means this query
            // targets the wow64 rebase window, which - if the up-front reservation succeeded - is
            // never a foreign occupant: it's either still our own PROT_NONE placeholder, or guest
            // content memory_manager's own reserved_regions_ already tracks. Reporting nothing here
            // is exactly the fix; without it, this live probe would find our own placeholder mapped
            // at the target address and register it as host_reserved before the actual fixed
            // allocation gets a chance to proceed, incorrectly rejecting it.
            if (this->wow64_host_window_reserved_ && rebase != 0)
            {
                return ranges;
            }

            const mach_vm_address_t window_start = address + rebase;
            const mach_vm_address_t window_end = window_start + size;

            mach_vm_address_t probe = window_start;
            while (probe < window_end)
            {
                mach_vm_address_t region_addr = probe;
                mach_vm_size_t region_size = 0;
                vm_region_basic_info_data_64_t info{};
                mach_msg_type_number_t info_count = VM_REGION_BASIC_INFO_COUNT_64;
                mach_port_t object_name = MACH_PORT_NULL;
                if (mach_vm_region(mach_task_self(), &region_addr, &region_size, VM_REGION_BASIC_INFO_64,
                                   reinterpret_cast<vm_region_info_t>(&info), &info_count, &object_name) != KERN_SUCCESS)
                {
                    break;
                }
                if (region_addr >= window_end)
                {
                    break;
                }

                // Report in guest (unrebased) coordinates, matching reserved_host_ranges() and what
                // reserved_regions_ is keyed by.
                const uint64_t hit_start = std::max<uint64_t>(region_addr, window_start);
                const uint64_t hit_end = std::min<uint64_t>(region_addr + region_size, window_end);
                ranges.push_back({.address = hit_start - rebase, .size = static_cast<size_t>(hit_end - hit_start)});

                probe = region_addr + region_size;
            }

            return ranges;
        }
#endif

      private:
        friend class fex_syscall_handler;

        // --[ memory_interface (private) ]-----------------------------------------------------------

        void map_mmio(uint64_t address, size_t size, mmio_read_callback read_cb, mmio_write_callback /*write_cb*/) override
        {
            // See mmio_region's doc comment for the real-backing/fault-and-emulate split.
            if (!is_page_aligned(address) || !is_page_aligned(size))
            {
                throw std::runtime_error("FEX MMIO mappings must be page aligned");
            }

            void* host_backing = nullptr;
            size_t host_backing_size = 0;

#ifdef __APPLE__
            const auto rebase = rebase_for(this->is_wow64_process_, address);
            host_backing_size = host_page_align_up_apple(size);
            void* result = ::mmap(reinterpret_cast<void*>(address + rebase), host_backing_size, PROT_READ | PROT_WRITE,
                                  MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
            if (result != MAP_FAILED && result == reinterpret_cast<void*>(address + rebase))
            {
                host_backing = result;
            }
#endif

            if (host_backing != nullptr)
            {
                read_cb(0, host_backing, size);
                ::mprotect(host_backing, host_backing_size, PROT_READ);
            }

            this->mmio_regions_.emplace_back(mmio_region{.address = address,
                                                         .size = size,
                                                         .read_cb = std::move(read_cb),
                                                         .host_backing = host_backing,
                                                         .host_backing_size = host_backing_size});
        }

        // Rewrites every MMIO region's real backing (see mmio_region's doc comment) with fresh
        // content, run from the guest-execution thread itself at each quantum boundary (see start())
        // so it can never race a guest read of the same page.
        void refresh_mmio_backings()
        {
            for (const auto& region : this->mmio_regions_)
            {
                if (region.host_backing == nullptr)
                {
                    continue;
                }

                ::mprotect(region.host_backing, region.host_backing_size, PROT_READ | PROT_WRITE);
                region.read_cb(0, region.host_backing, region.size);
                ::mprotect(region.host_backing, region.host_backing_size, PROT_READ);
            }
        }

        void map_memory(uint64_t address, size_t size, memory_permission permissions) override
        {
            if (!is_page_aligned(address) || !is_page_aligned(size))
            {
                throw std::runtime_error("FEX memory mappings must be page aligned");
            }

#ifdef __APPLE__
            // The host mmap/mprotect calls happen at 16KB granularity via the shadow table (see
            // sync_host_page_apple); guest VA == host VA is unaffected, this only changes which host
            // syscalls actually get issued and at what alignment.
            this->set_shadow_range_apple(address, size, permissions);
            this->sync_host_pages_covering_apple(address, size);
#else
            // Place the guest pages at their guest address in the host address space (guest VA == host VA).
            void* result = ::mmap(reinterpret_cast<void*>(address), size, to_prot(permissions),
                                  MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
            if (result == MAP_FAILED || reinterpret_cast<uint64_t>(result) != address)
            {
                throw std::runtime_error("FEX backend failed to map guest memory at requested address");
            }
#endif

            this->erase_region_range(address, size);
            this->regions_[address] = mapped_region{.size = size, .permissions = permissions, .owned = true};
            this->mark_executable_range(address, size, permissions);
        }

#ifdef __APPLE__
        void reserve_guest_address_range(uint64_t address, size_t size) override
        {
            // Called for every guest allocation, including reserve-only ones that never reach
            // map_memory. Guest VA == host VA here, so a reserved-but-uncommitted range still needs to
            // be unavailable to the host's own allocator - otherwise something like a FEXCore JIT code
            // buffer (allocated via plain mmap(NULL, ...), unaware of sogen's guest bookkeeping) can be
            // handed this exact address before the guest range is ever committed.
            const uint64_t start = host_page_align_down_apple(address);
            const uint64_t end = host_page_align_up_apple(address + size);
            for (uint64_t host_page = start; host_page < end; host_page += host_page_size_apple)
            {
                if (this->mapped_host_pages_apple_.contains(host_page))
                {
                    // Already ours (from this or an adjacent guest region sharing the host page) -
                    // sync_host_page_apple/map_memory will apply the real permission when committed.
                    continue;
                }

                // See wow64_guest_rebase's doc comment - host_page is a guest address here
                // (mapped_host_pages_apple_ stays keyed by it), the real host mmap target is rebased.
                const auto rebase = rebase_for(this->is_wow64_process_, host_page);
                void* result = ::mmap(reinterpret_cast<void*>(host_page + rebase), host_page_size_apple, PROT_NONE,
                                      MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
                if (result == MAP_FAILED || result != reinterpret_cast<void*>(host_page + rebase))
                {
                    throw std::runtime_error("FEX backend failed to reserve guest address range at the host level");
                }
                this->mapped_host_pages_apple_.insert(host_page);
            }
        }
#endif

        void map_host_memory(uint64_t address, size_t size, void* host_pointer, memory_permission permissions) override
        {
            if (!is_page_aligned(address) || !is_page_aligned(size))
            {
                throw std::runtime_error("FEX host memory mappings must be page aligned");
            }

            // See wow64_guest_rebase's doc comment - regions_ stays keyed by the guest (unrebased)
            // address; the real host aliasing target is rebased in 32-bit mode.
            const auto rebase = rebase_for(this->is_wow64_process_, address);
            const uint64_t host_address = address + rebase;

#ifdef __APPLE__
            // Darwin has no mremap(); mach_vm_remap() is the Mach equivalent - it creates a new
            // mapping at `address` that refers to the same underlying pages as `host_pointer`
            // (VM_FLAGS_FIXED forces the target address; VM_FLAGS_OVERWRITE replaces whatever
            // reservation sogen's memory_manager already put there, matching mmap(MAP_FIXED)'s
            // semantics). copy=FALSE: alias, don't duplicate, matching Linux's MREMAP_MAYMOVE path.
            mach_vm_address_t target_address = host_address;
            vm_prot_t cur_protection = VM_PROT_NONE;
            vm_prot_t max_protection = VM_PROT_NONE;
            const kern_return_t result = ::mach_vm_remap(mach_task_self(), &target_address, size, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
                                                         mach_task_self(), reinterpret_cast<mach_vm_address_t>(host_pointer), FALSE,
                                                         &cur_protection, &max_protection, VM_INHERIT_NONE);
            if (result != KERN_SUCCESS || target_address != host_address)
            {
                throw std::runtime_error("FEX backend failed to alias host memory into the guest");
            }
#else
            // Move the existing host mapping so the guest sees it at `address` without a staging copy.
            // mremap with MREMAP_FIXED relocates the VMA; the caller must treat host_pointer as moved.
            void* result = ::mremap(host_pointer, size, size, MREMAP_MAYMOVE | MREMAP_FIXED, reinterpret_cast<void*>(host_address));
            if (result == MAP_FAILED || reinterpret_cast<uint64_t>(result) != host_address)
            {
                throw std::runtime_error("FEX backend failed to alias host memory into the guest");
            }
#endif

            ::mprotect(reinterpret_cast<void*>(host_address), size, to_prot(permissions));
            // owned=false: the memory belongs to the caller; we must not munmap it on teardown.
            this->erase_region_range(address, size);
            this->regions_[address] = mapped_region{.size = size, .permissions = permissions, .owned = false};
            this->mark_executable_range(address, size, permissions);
        }

        bool host_memory_aliasing_is_coherent() const override
        {
            // Conservative, matching the KVM backend's reasoning: Apple Silicon's unified memory
            // architecture makes CPU/GPU cache coherency for Metal buffers (what MoltenVK's Vulkan
            // buffers ultimately are) far more likely than on discrete-GPU x86 setups, but there is no
            // confirmed guarantee across every Metal storage mode this bridge might use. An
            // unnecessary flush on already-coherent memory is a harmless no-op; wrongly claiming
            // coherence when it isn't would surface as real rendering corruption, so default to false.
            return false;
        }

        void flush_host_memory_cache(const void* host_pointer, size_t size) override
        {
            if (host_pointer == nullptr || size == 0)
            {
                return;
            }

            // Evicts the CPU data cache for [host_pointer, host_pointer + size) out to memory, so a
            // GPU reading the same physical pages non-coherently sees the guest's writes - the ARM64
            // equivalent of the KVM backend's clflushopt+sfence pair.
#ifdef __APPLE__
            // Darwin's official public API for exactly this ("useful when dealing with cache
            // incoherent devices or DMA" - OSCacheControl.h) - prefer it over hand-rolled `dc civac`
            // inline asm, since EL0 access to cache-maintenance instructions isn't something this
            // embedder should assume is unconditionally permitted by the kernel.
            ::sys_dcache_flush(const_cast<void*>(host_pointer), size);
#else
            constexpr size_t cache_line_size = 64; // Conservative for all known ARM64 implementations.
            const auto first = reinterpret_cast<uintptr_t>(host_pointer) & ~(cache_line_size - 1);
            const auto last = reinterpret_cast<uintptr_t>(host_pointer) + size;
            for (auto line = first; line < last; line += cache_line_size)
            {
                __asm__ volatile("dc civac, %0" : : "r"(line) : "memory");
            }
            __asm__ volatile("dsb sy" ::: "memory");
#endif
        }

        void unmap_memory(uint64_t address, size_t size) override
        {
            // MMIO regions (see mmio_region's doc comment) were never really mapped at the host level
            // beyond their own optional host_backing, which is torn down here if present.
            if (std::erase_if(this->mmio_regions_, [address](const mmio_region& region) {
                    if (region.address != address)
                    {
                        return false;
                    }
                    if (region.host_backing != nullptr)
                    {
                        ::munmap(region.host_backing, region.host_backing_size);
                    }
                    return true;
                }))
            {
                return;
            }

#ifdef __APPLE__
            this->set_shadow_range_apple(address, size, std::nullopt);
            this->sync_host_pages_covering_apple(address, size);
#else
            ::munmap(reinterpret_cast<void*>(address), size);
#endif
            this->invalidate_code_range(address, size, /*include_inactive_contexts=*/true);
            this->erase_region_range(address, size);
        }

        void apply_memory_protection(uint64_t address, size_t size, memory_permission permissions) override
        {
#ifdef __APPLE__
            this->set_shadow_range_apple(address, size, permissions);
            this->sync_host_pages_covering_apple(address, size);
#else
            if (::mprotect(reinterpret_cast<void*>(address), size, to_prot(permissions)) != 0)
            {
                throw std::runtime_error("FEX backend failed to change memory protection");
            }
#endif

            this->set_region_range_permissions(address, size, permissions);

            // Permission changes can expose/retract executable code; keep FEX's translation cache honest.
            this->invalidate_code_range(address, size);
            this->mark_executable_range(address, size, permissions);
        }

        // --[ region bookkeeping ]-------------------------------------------------------------------

        // Returns the declared permission of the region containing `address` (memory_permission::none
        // if it isn't covered - callers should already have checked is_range_mapped).
        memory_permission permission_covering(uint64_t address) const
        {
            auto it = this->regions_.upper_bound(address);
            if (it == this->regions_.begin())
            {
                return memory_permission::none;
            }
            --it;
            const uint64_t region_end = it->first + it->second.size;
            if (address < it->first || address >= region_end)
            {
                return memory_permission::none;
            }
            return it->second.permissions;
        }

        // Temporarily grants write access to [address, address+size) for a loader-privileged write
        // (sogen itself writing guest memory it declared read-only, e.g. a PE section's initial file
        // content before its final permission is locked in) and reverts to the declared permission
        // afterwards. Unlike guest code, which can't write here at all, this path needs one because
        // FEX/KVM enforce the declared permission via real host protection - unlike Unicorn, whose
        // uc_mem_write operates on its own emulated memory independent of any host mprotect state.
        void set_temporary_write_access(uint64_t address, size_t size, memory_permission declared, bool enable)
        {
#ifdef __APPLE__
            (void)declared; // Apple restores via sync_host_page_apple, which re-derives it from the shadow table.
            const uint64_t start = host_page_align_down_apple(address);
            const uint64_t end = host_page_align_up_apple(address + size);
            for (uint64_t host_page = start; host_page < end; host_page += host_page_size_apple)
            {
                if (!enable)
                {
                    this->sync_host_page_apple(host_page);
                    continue;
                }

                memory_permission effective = memory_permission::none;
                for (uint64_t page = host_page; page < host_page + host_page_size_apple; page += page_size)
                {
                    const auto it = this->page_shadow_apple_.find(page);
                    if (it != this->page_shadow_apple_.end())
                    {
                        effective = effective | it->second;
                    }
                }
                // See wow64_guest_rebase's doc comment - host_page is a guest address here, rebase
                // needed for the real host mprotect target.
                const auto rebase = rebase_for(this->is_wow64_process_, host_page);
                ::mprotect(reinterpret_cast<void*>(host_page + rebase), host_page_size_apple,
                           to_prot_apple(effective | memory_permission::write));
            }
#else
            const uint64_t start = address & ~(page_size - 1);
            const uint64_t end = (address + size + page_size - 1) & ~(page_size - 1);
            const memory_permission perm = enable ? (declared | memory_permission::write) : declared;
            ::mprotect(reinterpret_cast<void*>(start), end - start, to_prot(perm));
#endif
        }

        // Removes every regions_ entry intersecting [address, address+size) so the map stays
        // non-overlapping (the invariant is_range_mapped/permission_covering rely on). The guest
        // memory manager tracks memory at a coarser granularity than this backend: it commits a
        // reserved region in gap-filling sub-ranges (each a separate map_memory here, so regions_
        // can hold several entries tiling one of its committed regions), but later decommits/releases
        // that committed region in one call - i.e. an unmap range that spans several regions_ entries
        // and does not start exactly at each entry's key. A plain regions_.erase(address) would drop
        // only the entry keyed at address and orphan the rest; a later allocation reusing that
        // address space then overlaps the orphan, and is_range_mapped's --upper_bound walk lands on
        // the stale inner entry (whose end is below the target) and wrongly reports "not mapped".
        // Entries straddling an edge of the range are trimmed/split so their part outside the range
        // survives (the host-level unmap/remap only ever touches [address, address+size)).
        void erase_region_range(uint64_t address, size_t size)
        {
            const uint64_t end = address + size;

            auto it = this->regions_.lower_bound(address);
            if (it != this->regions_.begin())
            {
                auto prev = std::prev(it);
                if (prev->first + prev->second.size > address)
                {
                    it = prev; // a region starting before `address` extends into the range
                }
            }

            while (it != this->regions_.end() && it->first < end)
            {
                const uint64_t region_start = it->first;
                const uint64_t region_end = region_start + it->second.size;
                if (region_end <= address)
                {
                    ++it;
                    continue;
                }

                const auto region = it->second;
                it = this->regions_.erase(it);

                if (region_start < address)
                {
                    this->regions_[region_start] = mapped_region{
                        .size = static_cast<size_t>(address - region_start), .permissions = region.permissions, .owned = region.owned};
                }
                if (region_end > end)
                {
                    it = this->regions_
                             .emplace(end, mapped_region{.size = static_cast<size_t>(region_end - end),
                                                         .permissions = region.permissions,
                                                         .owned = region.owned})
                             .first;
                    ++it;
                }
            }
        }

        // Records `permissions` on every regions_ entry intersecting [address, address+size),
        // splitting entries that straddle an edge so only the in-range part changes. A protection
        // change from the guest can target a sub-range of a larger committed region, start mid-entry,
        // or span several entries - all of which a plain regions_.find(address) either misses (mid-
        // entry, no key) or over-applies (sets a larger region's permission for a sub-range write).
        // Since QueryGuestExecutableRange decides executability straight from these recorded
        // permissions, a stale entry makes the JIT reject a legitimately-executable page (a NoExec
        // "wild branch") or execute a page it should not. Gaps in the tiling are preserved (only
        // already-present entries are rewritten - unmapped holes are never fabricated as mapped).
        void set_region_range_permissions(uint64_t address, size_t size, memory_permission permissions)
        {
            const uint64_t end = address + size;

            std::vector<std::pair<uint64_t, mapped_region>> touched;
            auto it = this->regions_.lower_bound(address);
            if (it != this->regions_.begin())
            {
                auto prev = std::prev(it);
                if (prev->first + prev->second.size > address)
                {
                    it = prev; // a region starting before `address` extends into the range
                }
            }
            for (; it != this->regions_.end() && it->first < end; ++it)
            {
                if (it->first + it->second.size > address)
                {
                    touched.emplace_back(it->first, it->second);
                }
            }

            for (const auto& [region_start, region] : touched)
            {
                const uint64_t region_end = region_start + region.size;
                this->regions_.erase(region_start);

                if (region_start < address)
                {
                    this->regions_[region_start] = mapped_region{
                        .size = static_cast<size_t>(address - region_start), .permissions = region.permissions, .owned = region.owned};
                }
                const uint64_t inner_start = std::max(region_start, address);
                const uint64_t inner_end = std::min(region_end, end);
                this->regions_[inner_start] =
                    mapped_region{.size = static_cast<size_t>(inner_end - inner_start), .permissions = permissions, .owned = region.owned};
                if (region_end > end)
                {
                    this->regions_[end] = mapped_region{
                        .size = static_cast<size_t>(region_end - end), .permissions = region.permissions, .owned = region.owned};
                }
            }
        }

        bool is_range_mapped(uint64_t address, size_t size) const
        {
            if (size == 0)
            {
                return true;
            }

            uint64_t cursor = address;
            const uint64_t end = address + size;

            // Walk the (sorted) region map covering [address, end). Regions are page-granular and
            // non-overlapping, so a simple forward walk suffices.
            while (cursor < end)
            {
                auto it = this->regions_.upper_bound(cursor);
                if (it == this->regions_.begin())
                {
                    return false;
                }
                --it;

                const uint64_t region_end = it->first + it->second.size;
                if (cursor < it->first || cursor >= region_end)
                {
                    return false;
                }
                cursor = region_end;
            }

            return true;
        }

#ifdef __APPLE__
        // --[ 16KB-host vs 4KB-guest permission reconciliation (Apple only) ]------------------------
        //
        // Updates the per-4KB shadow for [address, address+size) then re-syncs every 16KB host page
        // it touches. `permissions` is nullopt for unmap (the pages become "never requested" again,
        // which must still fault like reserved-but-uncommitted guest memory - not silently allowed).

        void set_shadow_range_apple(uint64_t address, size_t size, std::optional<memory_permission> permissions)
        {
            for (uint64_t page = address; page < address + size; page += page_size)
            {
                if (permissions.has_value())
                {
                    this->page_shadow_apple_[page] = *permissions;
                }
                else
                {
                    this->page_shadow_apple_.erase(page);
                }
            }
        }

        // Applies the effective host permission for one 16KB-aligned host page, derived from its
        // (up to four) 4KB shadow slots:
        //   - all slots agree (including "all absent") -> apply exactly, the common case.
        //   - slots disagree -> union (most permissive). This also covers the "some slot absent"
        //     case for now: until the Mach exception handler (a later phase) can resolve faults on a
        //     PROT_NONE page, a guard/reserved slot sharing a host page with mapped memory is folded
        //     into the union rather than made to fault - a temporary relaxation, not the final
        //     design; tightening this to genuinely fault (and resolving the resulting "legitimate
        //     access from a stricter neighbor" case) is deferred to that phase.
        // host_page_addr is the guest page address (page_shadow_apple_/mapped_host_pages_apple_ stay
        // keyed by it, unrebased); the real host mmap/mprotect/munmap target is
        // host_page_addr + wow64_guest_rebase in 32-bit mode (see that constant's doc comment) -
        // despite this function's name, it's not the actual host pointer until this rebase is added.
        void sync_host_page_apple(uint64_t host_page_addr)
        {
            memory_permission effective = memory_permission::none;
            bool any_slot_present = false;

            for (uint64_t page = host_page_addr; page < host_page_addr + host_page_size_apple; page += page_size)
            {
                const auto it = this->page_shadow_apple_.find(page);
                if (it == this->page_shadow_apple_.end())
                {
                    continue;
                }
                any_slot_present = true;
                effective = effective | it->second;
            }

            const auto rebase = rebase_for(this->is_wow64_process_, host_page_addr);
            void* host_ptr = reinterpret_cast<void*>(host_page_addr + rebase);
            const bool currently_mapped = this->mapped_host_pages_apple_.contains(host_page_addr);

            if (!any_slot_present)
            {
                if (currently_mapped)
                {
                    ::munmap(host_ptr, host_page_size_apple);
                    this->mapped_host_pages_apple_.erase(host_page_addr);
                }
                return;
            }

            if (!currently_mapped)
            {
                void* result = ::mmap(host_ptr, host_page_size_apple, to_prot_apple(effective),
                                      MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
                if (result == MAP_FAILED || result != host_ptr)
                {
                    throw std::runtime_error("FEX backend failed to map guest memory at requested address");
                }
                this->mapped_host_pages_apple_.insert(host_page_addr);
                return;
            }

            if (::mprotect(host_ptr, host_page_size_apple, to_prot_apple(effective)) != 0)
            {
                throw std::runtime_error("FEX backend failed to change memory protection");
            }
        }

        void sync_host_pages_covering_apple(uint64_t address, size_t size)
        {
            const uint64_t start = host_page_align_down_apple(address);
            const uint64_t end = host_page_align_up_apple(address + size);
            for (uint64_t host_page = start; host_page < end; host_page += host_page_size_apple)
            {
                this->sync_host_page_apple(host_page);
            }
        }
#endif

        // --[ FEX context plumbing ]-----------------------------------------------------------------

        void initialize_context()
        {
            // Without an installed handler, LogMan::Msg::MFmtImpl/LogMan::Throw::MFmt silently
            // discard the formatted message (see LogManager.cpp - `if (Handler) { ... }`) while
            // still unconditionally executing FEX_TRAP_EXECUTION for ASSERT-level messages - meaning
            // every internal FEXCore assertion failure would otherwise crash with zero indication of
            // what actually failed. Install both handlers to print to stderr; this runs from ordinary
            // call context (assertions fire synchronously, not from a signal handler), so plain
            // fprintf is fine here, no async-signal-safety concerns apply.
            LogMan::Msg::InstallHandler([](LogMan::DebugLevels level, const char* message) {
                fprintf(stderr, "[FEXCore LogMan] level=%s: %s\n", LogMan::DebugLevelStr(level), message);
            });
            LogMan::Throw::InstallHandler([](const char* message) { fprintf(stderr, "[FEXCore LogMan THROW] %s\n", message); });

#ifdef __APPLE__
            // Confine every FEXCore-internal host allocation (BlockLinks tree storage, JIT CodeBuffer,
            // dispatcher) to a dedicated arena disjoint from the guest address space, and install the
            // FEXCore::Allocator::mmap/munmap hooks that steer them there. Must happen before the very
            // first internal allocation below (CreateNewContext allocates the CodeBuffer/dispatcher)
            // and before reserved_host_ranges() is first queried, so the whole arena is off-limits to
            // guest allocations. See fex_internal_arena's comment for the full rationale.
            fex_internal_arena::instance().install();

            // Claim the wow64 rebase window here too, unconditionally - not only once bitness is known
            // to be wow64 (notify_process_bitness, which used to be the sole call site). A real
            // regression showed this window can already be occupied by the time notify_process_bitness
            // runs: this process links Cocoa/Metal (sogen's own GPU/window subsystem - see the
            // vulkan-shim work), and Metal's device/heap setup reserves a large (multi-GB) host VA
            // range whose ASLR placement can land inside this window before any target executable
            // (and thus its bitness) is even known - measured directly landing on the heaven's-gate
            // trampoline's rebased target and reproducibly breaking install_wow64_heaven_gate. This is
            // the earliest point in the process (this backend's own construction, before FEXCore's
            // context/CodeBuffer, before any GUI/graphics initialization sogen itself triggers) this
            // code can act, so it gives the reservation the best chance of winning that race. Harmless
            // for a plain 64-bit-only guest: it only ever steers that guest's own allocations away from
            // this one 4GB range, exactly like any other foreign occupant already does.
            this->reserve_wow64_host_window();
#endif

            // libc++abi's default terminate handler prints nothing useful for an uncaught exception
            // by default. Install our own to print the exception's what() plus a real backtrace
            // before aborting - this runs from ordinary call context (std::terminate is not a signal
            // handler), so backtrace()/backtrace_symbols()/fprintf are all safe here.
            std::set_terminate([]() {
                fprintf(stderr, "[FEX backend] std::terminate invoked\n");
                if (auto exc = std::current_exception())
                {
                    try
                    {
                        std::rethrow_exception(exc);
                    }
                    catch (const std::exception& e)
                    {
                        fprintf(stderr, "[FEX backend] uncaught exception: %s\n", e.what());
                    }
                    catch (...)
                    {
                        fprintf(stderr, "[FEX backend] uncaught exception of unknown type\n");
                    }
                }

                void* frames[64]{};
                const int frame_count = ::backtrace(frames, 64);
                char** symbols = ::backtrace_symbols(frames, frame_count);
                fprintf(stderr, "[FEX backend] backtrace (%d frames):\n", frame_count);
                for (int i = 0; i < frame_count; ++i)
                {
                    fprintf(stderr, "  %s\n", symbols ? symbols[i] : "?");
                }
                free(symbols);

                std::abort();
            });

            FEXCore::Config::Initialize();
            FEXCore::Config::Load();

            // Diagnostic tooling: FEXCore's own per-block IR dump. When the env var
            // EMULATOR_FEX_DUMPIR names an existing directory, FEXCore writes one file per
            // translated guest basic block into it, keyed by the block's guest RIP:
            // "<dir>/<rip:x>-pre.ir" (frontend IR straight out of the decoder, BEFOREOPT) and
            // "<dir>/<rip:x>-post.ir" (after all optimization + register allocation, AFTEROPT).
            // This lets a specific guest RVA window be inspected instruction-by-instruction to
            // find a miscompiled/mis-decoded op. It is a pure runtime config (no FEXCore rebuild)
            // and is confirmed not to perturb timing-sensitive JIT bugs, unlike inline hot-path
            // C++ diagnostics. Off (zero overhead) unless the env var is set. See the plan file's
            // "IR-DUMP TOOLING" recipe. PassManagerDumpIR value 3 == BEFOREOPT(1)|AFTEROPT(2).
            if (const char* dumpir_dir = std::getenv("EMULATOR_FEX_DUMPIR"))
            {
                FEXCore::Config::Set(FEXCore::Config::CONFIG_DUMPIR, dumpir_dir);
                FEXCore::Config::Set(FEXCore::Config::CONFIG_PASSMANAGERDUMPIR, "3");
            }

            // context_ is always the 64-bit FEXCore::Context, wow64 process or not - see
            // notify_process_bitness's doc comment. context32_ (ensure_context32(), built lazily
            // once a wow64 process is known) is the one built with CONFIG_IS64BIT_MODE=0.
            FEXCore::Config::Set(FEXCore::Config::CONFIG_IS64BIT_MODE, "1");

            // Piggyback on FEXCore's own GdbServer config flag: its only effect inside FEXCore
            // (ContextImpl::InitCore, Core.cpp) is setting Config.NeedsPendingInterruptFaultCheck,
            // which makes the JIT emit a `str zr, [InterruptFaultPage]` at every block entry
            // (JIT.cpp's EmitSuspendInterruptCheck) - the mechanism request_thread_stop() needs to
            // force a stuck-in-JIT thread to fault so handle_fault_signal gets a chance to run. We
            // don't use FEXCore's actual built-in gdbserver (sogen has its own, separate stub), so
            // this has no other observable effect.
            FEXCore::Config::Set(FEXCore::Config::CONFIG_GDBSERVER, "1");

#ifdef __APPLE__
            const FEXCore::HostFeatures features = fetch_host_features_apple();
#else
            const FEXCore::HostFeatures features{}; // TODO(fex): FEXCore::FetchHostFeatures() on real HW.
#endif
            this->context_ = FEXCore::Context::Context::CreateNewContext(features);

            // Tell context_'s JIT the actual host address offset to use for the wow64 rebase -
            // whatever reserve_wow64_host_window() (called above, before context_ existed) already
            // chose, or the unchanged default if that never ran (e.g. non-Apple platforms). Must
            // happen before the first block compiles (InitCore(), below) - see
            // SetWow64GuestRebaseValue's doc comment (public Context.h).
            this->context_->SetWow64GuestRebaseValue(this->wow64_guest_rebase_);

            // active_context_/active_thread_ track whichever context/thread is currently executing -
            // see their doc comment. Nothing but context_/thread_ ever runs until Phase B's gate-
            // crossing starts flipping this, so initialize it to context_ here, once, right after
            // construction.
            this->active_context_ = this->context_.get();

            this->syscall_handler_ = std::make_unique<fex_syscall_handler>(*this);
            this->context_->SetSyscallHandler(this->syscall_handler_.get());

            // InitCore() requires a non-null SignalDelegator. FEXCore's SetConfig()/GetConfig() (the
            // dispatcher entry-point addresses used by handle_fault_signal below) are concrete, non-
            // virtual methods on the base class, so the plain base satisfies everything InitCore()
            // needs; real fault *delivery* is handled by the host signal handler installed below
            // instead of a SignalDelegator subclass.
            this->signal_delegator_ = std::make_unique<FEXCore::SignalDelegator>();
            this->context_->SetSignalDelegator(this->signal_delegator_.get());

            this->context_->InitCore();

#ifdef __APPLE__
            install_fault_signal_handlers(*this);
#endif
        }

        // Lazily builds context32_, the second, 32-bit (CONFIG_IS64BIT_MODE=0) FEXCore::Context a
        // wow64 process needs. Called from notify_process_bitness() once a wow64 process is known,
        // well before create_thread() seeds context_'s thread - safe to build now since it touches no
        // state create_thread()/context_ depend on. FEXCore::Config is a process-global registry read
        // once per FEXCore::Context::Context at CreateNewContext() time (confirmed: context_ above
        // already captured CONFIG_IS64BIT_MODE=1 by the time this runs), so flipping it to "0" here
        // cannot retroactively affect the already-built context_.
        void ensure_context32()
        {
            if (this->context32_)
            {
                return;
            }

            FEXCore::Config::Set(FEXCore::Config::CONFIG_IS64BIT_MODE, "0");

#ifdef __APPLE__
            const FEXCore::HostFeatures features = fetch_host_features_apple();
#else
            const FEXCore::HostFeatures features{};
#endif
            this->context32_ = FEXCore::Context::Context::CreateNewContext(features);

            // context32_ is a genuinely 32-bit-mode Context (GuestMemoryRebase() applies the rebase
            // unconditionally there, not just when NeedsWow64GuestRebase is set - see its doc
            // comment), so it must agree with context_/wow64_guest_rebase_ on the actual host address
            // offset - whatever reserve_wow64_host_window() already chose for this process by the
            // time a wow64 process's first gate crossing lazily creates this Context.
            this->context32_->SetWow64GuestRebaseValue(this->wow64_guest_rebase_);

            this->syscall_handler32_ = std::make_unique<fex_syscall_handler>(*this);
            this->context32_->SetSyscallHandler(this->syscall_handler32_.get());

            this->signal_delegator32_ = std::make_unique<FEXCore::SignalDelegator>();
            this->context32_->SetSignalDelegator(this->signal_delegator32_.get());

            this->context32_->InitCore();

            // Restore the global back to what context_ (the currently-executing context) actually
            // is, so any later, unrelated CreateNewContext-driving code path (there is none today,
            // but the global is otherwise easy to leave in a surprising state) doesn't silently pick
            // up "0" from this call.
            FEXCore::Config::Set(FEXCore::Config::CONFIG_IS64BIT_MODE, "1");
        }

        // Creates thread32_, the InternalThreadState that actually executes 32-bit guest code on
        // context32_. Called once, from create_thread() (ordinary call context) for a wow64 process
        // - deliberately not left lazy for the first 64->32 gate crossing to create, since that
        // crossing is only ever reached from inside handle_fault_signal (a signal handler), where
        // this function's real heap allocation would be unsafe. The initial CPUState this seeds is
        // irrelevant: the first gate-crossing handler to actually use thread32_ immediately
        // overwrites every GPR/XMM/x87/EFLAGS/RIP/RSP with the marshaled state from whichever
        // context is crossing down, so an all-zero NewThreadState (CreateThread's own default) is
        // fine here.
        void create_thread32()
        {
            this->thread32_ = this->context32_->CreateThread(0, 0, nullptr);

            // Real Windows shares one GDT across both bitnesses of a wow64 process (see load_gdt's
            // doc comment) - point context32_'s segment table at the exact same physical GDT memory
            // sogen's loader wrote for context_. GDT_ADDR (process_context.hpp) sits well above the
            // rebase threshold on Apple Silicon, so rebase_for evaluates to 0 regardless of bitness,
            // but apply it anyway to stay correct if that constant ever changes.
            const auto rebase = rebase_for(this->is_wow64_process_, this->gdt_base_);
            this->thread32_->CurrentFrame->State.segment_arrays[0] =
                reinterpret_cast<FEXCore::Core::CPUState::gdt_segment*>(this->gdt_base_ + rebase);

            // ensure_callret_stack writes into whatever this->active_thread_ currently is (see its
            // doc comment) - temporarily point it at the new thread32_ engine so it gets its own
            // private call-ret stack set up correctly, then restore whatever was active before.
            // thread32_ doesn't actually become the active engine until the gate-crossing handler
            // flips active_thread_/active_context_ itself, right after marshaling state into it.
            auto* const previously_active_thread = this->active_thread_;
            this->active_thread_ = this->thread32_;
            this->ensure_callret_stack(this->thread32_->CurrentFrame->State);
            this->active_thread_ = previously_active_thread;
        }

        // A registered WoW64 bitness mode-switch point (see x86_emulator::register_gate_crossing).
        struct gate_crossing
        {
            uint64_t address = 0;
            size_t size = 0;
            gate_crossing_kind kind = gate_crossing_kind::heaven_gate;
        };

        // Returns the registered gate crossing whose range contains `rip`, or nullptr. Called from
        // handle_fault_signal's synthetic-#PF path with the faulting guest RIP (the address FEXCore
        // refused to compile because QueryGuestExecutableRange reported the gate range non-executable).
        const gate_crossing* find_gate_crossing(uint64_t rip) const
        {
            for (const auto& gate : this->gate_crossings_)
            {
                if (rip >= gate.address && rip < gate.address + gate.size)
                {
                    return &gate;
                }
            }
            return nullptr;
        }

        // Performs a WoW64 bitness gate crossing: marshals the architectural register file out of the
        // currently-active engine into the other-bitness engine, sets the target's entry point/stack/
        // segment selectors per the crossing's calling convention, and flips active_context_/
        // active_thread_ so the next ExecuteThread runs the other engine. Called from
        // handle_fault_signal once the faulting RIP is recognized as a registered gate.
        //
        // Direction is data-driven by the target CS selector, not by which engine is currently active:
        // the same trampoline mechanism is bidirectional (whatever CS you load selects the mode), so
        // reading the target CS is the robust way to decide the destination engine. This makes both
        // the first 64->32 entry into 32-bit code and the 32->64 return (e.g. exception delivery via
        // exception_dispatch.cpp) go through one handler.
        //
        // Returns true if the crossing was performed; false if the gate's convention isn't decodable
        // yet (wow64cpu_dispatch - see below), in which case the caller falls back to the ordinary
        // memory-violation path rather than silently applying the wrong convention.
        // Extracts the 32-bit linear base of a GDT selector from the shared GDT context32_ points at
        // (segment_arrays[0]), matching FEXCore's own UpdatePrefixFromSegment/CalculateGDTBase. Used
        // to resolve the 32-bit segment bases (notably fs -> TEB32) when entering 32-bit mode, since
        // the crossing sets the selectors directly rather than executing the `mov Sreg` that would
        // otherwise populate the cached base.
        static uint32_t gdt_segment_base(FEXCore::Core::CPUState& state, uint16_t selector)
        {
            const auto* segment = FEXCore::Core::CPUState::GetSegmentFromIndex(state, selector);
            return FEXCore::Core::CPUState::CalculateGDTBase(*segment);
        }

        // Performs the real WoW64 forward (64->32) transition by emulating RunSimulatedCode's observable
        // effect: it reads the WoW64 CPU-area register block (an i386 CONTEXT the 64-bit side prepared)
        // and marshals it into the 32-bit context32_, exactly as RunSimulatedCode's own segment-setup +
        // `iretq`/`ljmp 0x23:EIP` tail would, then flips execution to the 32-bit engine. Skipping
        // RunSimulatedCode's body entirely is what avoids ever handing its `mov gs, cx` (unimplemented
        // in FEX's 64-bit JIT) to the compiler. All field offsets are relative to the r13 pointer
        // RunSimulatedCode computes as *(TEB64+0x1488)+0x80; they were decoded from the shipped
        // wow64cpu.dll (an i386 CONTEXT sits at r13-0x60). Returns true on success.
        bool enter_wow64_32bit_from_run_simulated_code(const gate_crossing& gate)
        {
            // Source is always the 64-bit engine: RunSimulatedCode only ever runs under context_.
            const auto& src = this->thread_->CurrentFrame->State;

            // r12 = gs:[0x30] (TEB64 self-pointer); r13 = *(TEB64 + 0x1488) + 0x80. gs_cached is the
            // logical (unrebased) 64-bit GS base; read_memory applies the wow64 rebase as needed.
            uint64_t teb64 = 0;
            if (!this->try_read_memory(src.gs_cached + 0x30, &teb64, sizeof(teb64)) || teb64 == 0)
            {
                return false;
            }
            uint64_t cpu_area = 0;
            if (!this->try_read_memory(teb64 + 0x1488, &cpu_area, sizeof(cpu_area)) || cpu_area == 0)
            {
                return false;
            }
            const uint64_t block = cpu_area + 0x80; // == r13

            const auto read32 = [&](uint64_t offset) -> uint32_t {
                uint32_t value = 0;
                this->try_read_memory(block + offset, &value, sizeof(value));
                return value;
            };

            const uint32_t edi = read32(0x20);
            const uint32_t esi = read32(0x24);
            const uint32_t ebx = read32(0x28);
            const uint32_t edx = read32(0x2c);
            const uint32_t ecx = read32(0x30);
            const uint32_t eax = read32(0x34);
            const uint32_t ebp = read32(0x38);
            const uint32_t eip = read32(0x3c);
            const uint32_t eflags = read32(0x44);
            const uint32_t esp = read32(0x48);

            // thread32_ is built eagerly in create_thread() (ordinary call context), never lazily
            // from here - this runs inside handle_fault_signal's synthetic-#PF path, where
            // create_thread32()'s real heap allocation (CreateThread, SignalDelegator
            // construction, InitCore()) would be unsafe. Null here means create_thread() genuinely
            // never ran for this (wow64) process, which should be unreachable - a process can't
            // execute far enough to reach the heaven's gate before start()/create_thread() has run.
            // Fail the crossing rather than allocate from the signal handler if that invariant is
            // ever wrong; the caller already has a safe fallback for a failed crossing (dispatches
            // an ordinary memory violation instead of resuming into half-marshaled state).
            if (this->thread32_ == nullptr)
            {
                return false;
            }
            auto& dst = this->thread32_->CurrentFrame->State;

            // Carry the genuinely-architectural register file across first, then overwrite the pieces
            // the WoW64 CPU-area block authoritatively defines for the 32-bit entry.
            marshal_architectural_state(src, dst);

            dst.gregs[detail::greg_rax] = eax;
            dst.gregs[detail::greg_rcx] = ecx;
            dst.gregs[detail::greg_rdx] = edx;
            dst.gregs[detail::greg_rbx] = ebx;
            dst.gregs[detail::greg_rsp] = esp;
            dst.gregs[detail::greg_rbp] = ebp;
            dst.gregs[detail::greg_rsi] = esi;
            dst.gregs[detail::greg_rdi] = edi;
            dst.rip = eip;
            set_flags_from_compacted_eflags(dst, eflags);

            // RunSimulatedCode's FULL path restores xmm0..5 from the CPU-area block (0xf0..0x140); the
            // rest of the XMM file is carried from the 64-bit engine by marshal_architectural_state.
            for (int i = 0; i < 6; ++i)
            {
                this->try_read_memory(block + 0xf0 + static_cast<uint64_t>(i) * 0x10, &dst.xmm.avx.data[i][0], 16);
            }

            // The 32-bit compat-mode selector set RunSimulatedCode installs: CS=0x23, SS/DS/ES=0x2b,
            // FS=0x53 (TEB32), GS flat. Resolve the cached bases from the shared GDT (only FS is
            // non-zero - it points at TEB32); leaving them right is what makes 32-bit fs:[...] TEB
            // accesses land correctly.
            dst.cs_idx = 0x23;
            dst.ss_idx = 0x2b;
            dst.ds_idx = 0x2b;
            dst.es_idx = 0x2b;
            dst.fs_idx = 0x53;
            dst.gs_idx = 0;
            dst.cs_cached = gdt_segment_base(dst, 0x23);
            dst.ss_cached = gdt_segment_base(dst, 0x2b);
            dst.ds_cached = gdt_segment_base(dst, 0x2b);
            dst.es_cached = gdt_segment_base(dst, 0x2b);
            dst.fs_cached = gdt_segment_base(dst, 0x53);
            dst.gs_cached = 0;

            // The forward gate intercepts RunSimulatedCode at its true entry (RVA 0x1650 == gate.address),
            // so its prologue never executes. That prologue is (from wow64cpu.dll's on-image UNWIND_INFO):
            //   push r15; push r14; push r13; push r12; push rbx; push rsi; push rdi; push rbp; sub rsp,0x68
            // leaving 8 saved nonvolatiles + a 0x68 local frame, with RunSimulatedCode's own return address
            // (into its caller: BTCpuSimulate, or Wow64KiUserCallbackDispatcher during a kernel callback) at
            // rsp+0xA8. The 64-bit engine is frozen here and later resumed INSIDE RunSimulatedCode's body
            // (the reverse gate resumes it at 0x17af to run Wow64SystemServiceEx), where the on-image
            // UNWIND_INFO assumes the prologue ran. If we leave the frozen rsp at the raw entry level, a
            // later callback-return longjmp (RtlUnwindEx) virtual-unwinds this frame by rsp+0xA8 and reads
            // uninitialized stack (observed as the garbage return address 0x10003) instead of the real
            // caller return address -> RtlpxVirtualUnwind's no-progress leaf guard returns 0xC00000FF ->
            // noncontinuable exception -> STATUS_FATAL_USER_CALLBACK_EXCEPTION. Native (Unicorn) executes
            // the real prologue and unwinds correctly. So emulate the prologue's stack effect now: spill the
            // 8 nonvolatiles into their canonical slots and drop rsp by 0xA8, making the frozen 64-bit frame
            // unwindable exactly as the on-image UNWIND_INFO describes. Only on the true entry - the 0x167f
            // syscall re-entry already runs with rsp prologue-adjusted and must not be double-counted.
            auto& state64 = this->thread_->CurrentFrame->State;
            if (src.rip == gate.address)
            {
                const uint64_t entry_rsp = state64.gregs[detail::greg_rsp];
                const auto spill = [&](uint64_t below_entry, int greg) {
                    const uint64_t value = state64.gregs[greg];
                    this->write_marshal_state(entry_rsp - below_entry, &value, sizeof(value));
                };
                spill(0x08, 15); // r15
                spill(0x10, 14); // r14
                spill(0x18, 13); // r13
                spill(0x20, 12); // r12
                spill(0x28, detail::greg_rbx);
                spill(0x30, detail::greg_rsi);
                spill(0x38, detail::greg_rdi);
                spill(0x40, detail::greg_rbp);
                state64.gregs[detail::greg_rsp] = entry_rsp - 0xA8;
            }

            // RunSimulatedCode's body (which this forward gate skips) loads the WoW64-reserved 64-bit
            // registers before the `jmp` into 32-bit mode (wow64cpu.dll, from RVA 0x1660 onward):
            //   r12 = gs:[0x30] (TEB64 self-pointer)
            //   r13 = *(TEB64+0x1488)+0x80 (the CpuArea i386-CONTEXT block == `block` above)
            //   r14 = the 64-bit rsp captured right before the mode switch (`mov r14, rsp`, i.e. the
            //         frozen RunSimulatedCode frame the thread returns to when re-entering 64-bit)
            //   r15 = wow64cpu!TurboThunkDispatch jump table (image_base + 0x36d0)
            // The 64-bit engine (thread_) stays frozen here while 32-bit code runs, but on a 32-bit
            // fault dispatch_exception must build a 64-bit exception CONTEXT whose R12..R15 hold these
            // reserved values: ntdll!KiUserExceptionDispatcher -> wow64!Wow64PrepareForException
            // derives the 64-bit exception stack directly from CONTEXT.R14 (`mov rsp, <R14-derived>`),
            // and read_raw_register sources r8..r15 for a wow64 32-bit-active capture from thread_ (the
            // 32-bit engine's own r8..r15 are meaningless and get clobbered by the fault's SRA spill).
            // Leaving these stale sent the 64-bit exception dispatcher to a wild host-range stack and
            // crashed exception delivery (rip=4); native's RunSimulatedCode body runs for real and sets
            // them, so it never diverged. Match RunSimulatedCode's `mov r14, rsp` timing: by this point
            // state64.gregs[rsp] holds the final frozen 64-bit frame on both the 0x1650 entry and the
            // 0x167f syscall re-entry (both skip the body).
            state64.gregs[12] = teb64;                                                    // r12 = TEB64
            state64.gregs[13] = block;                                                    // r13 = CpuArea block
            state64.gregs[14] = state64.gregs[detail::greg_rsp];                          // r14 = 64-bit frame
            state64.gregs[15] = (gate.address & ~static_cast<uint64_t>(0xFFFF)) + 0x36d0; // r15 = turbo table

            this->active_context_ = this->context32_.get();
            this->active_thread_ = this->thread32_;
            return true;
        }

        // Performs the real WoW64 reverse (32->64) transition. The 32-bit ntdll syscall stub reaches
        // wow64cpu.dll's WOW64SVC thunk (RVA 0x2010) via `call fs:[0xC0]` (Wow64Transition); the thunk
        // does a far `ljmp 0x33:0x2024` into 64-bit mode, which the fixed-bitness 32-bit Context cannot
        // execute. Instead of the thunk's bitness switch + wow64cpu.dll's own reverse-marshal (0x1779),
        // we marshal the current 32-bit register file into the WoW64 CPU-area CONTEXT block ourselves
        // and resume the 64-bit engine (context_, frozen at RunSimulatedCode's entry by the forward
        // crossing) at TurboDispatchJumpAddressStart (0x17a6). The real 64-bit TurboDispatch +
        // wow64.dll!Wow64SystemServiceEx then run, translating the 32-bit service number to its 64-bit
        // equivalent and issuing a genuine 64-bit `syscall` that sogen's own syscall hook catches (the
        // same path the native backends use, so sogen dispatches by the translated 64-bit number).
        // wow64.dll writes the result into CONTEXT.Eax and `jmp 0x167f`s back into RunSimulatedCode's
        // body - which is inside the forward gate's range, so the forward crossing re-fires and
        // re-enters 32-bit code at the syscall's return point carrying the result. Returns true on
        // success; false only on a genuine memory-read failure (caller then falls through to the
        // ordinary memory-violation path).
        bool enter_wow64_64bit_from_wow64svc_thunk(const gate_crossing& gate)
        {
            // wow64cpu.dll layout: TurboDispatchJumpAddressStart @0x17a6; the r15 turbo-thunk jump
            // table that BTCpuProcessInit builds @0x36d0. This handler serves BOTH reverse-gate bop
            // codes - the WOW64SVC thunk (RVA 0x2010) and the W64SVC turbo bop (RVA 0x6000) - so the
            // image base is recovered by rounding the gate address down to the 64K PE allocation
            // granularity rather than subtracting a single fixed RVA.
            const uint64_t image_base = gate.address & ~static_cast<uint64_t>(0xFFFF);
            // Resume at the GENERIC dispatcher (TurboDispatchJumpAddressEnd @0x17af), NOT the turbo
            // table dispatch @0x17a6. 0x17a6 does `mov ecx,eax; shr ecx,0x10; jmp [r15+8*rcx]`, which
            // for a turbo-encoded service number (eax>>16 != 0, e.g. NtQueryPerformanceCounter) jumps
            // into an inline turbo thunk in wow64cpu.dll whose RETURN to 32-bit is its own bitness-
            // switch `ljmp` (RVA 0x1cd4 et al.) - sogen registers no gate there, so FEX cannot execute
            // that far jump and the turbo thunk corrupts the 32-bit state (the *next* syscall then reads
            // garbage args -> the near-null write that was the wall here). 0x17af instead does
            // `mov ecx,eax; mov rdx,r11; call Wow64SystemServiceEx; mov [r13+0x34],eax; jmp 0x167f`,
            // returning via 0x167f (inside RunSimulatedCode's forward gate) - the only 64->32 return
            // sogen intercepts. Forcing generic for ALL syscalls is correct: Wow64SystemServiceEx
            // derives the service table/index from only the low 14 bits of eax ((eax>>12)&3, eax&0xFFF)
            // and ignores the high-word turbo index, so no masking of eax is needed. table[0] is 0x17af
            // anyway, so non-turbo syscalls are unaffected; turbo syscalls just take the slower (but
            // correct) generic thunk. This is the plan's documented robust fix at a small perf cost.
            //
            // The +0x17af offset is only a fallback: confirmed empirically to be WRONG on at least
            // one real wow64cpu.dll build (the bytes there decode as nonsense, not the documented
            // `mov ecx,eax; ...` sequence, and executing them corrupts guest memory beyond repair).
            // module_manager resolves the real TurboDispatchJumpAddressEnd export address and hands
            // it over via set_wow64_turbo_dispatch_end - always prefer that when it's been set.
            const uint64_t generic_dispatch = this->wow64_turbo_dispatch_end_ != 0 ? this->wow64_turbo_dispatch_end_ : image_base + 0x17af;
            const uint64_t jump_table = image_base + 0x36d0;

            // Source: the 32-bit engine that reached the thunk (SRA already spilled - this is a
            // controlled synthetic #PF). Its live register file is the syscall's argument context.
            const auto& src32 = this->active_thread_->CurrentFrame->State;
            const uint32_t eax = static_cast<uint32_t>(src32.gregs[detail::greg_rax]);
            const uint32_t ecx = static_cast<uint32_t>(src32.gregs[detail::greg_rcx]);
            const uint32_t edx = static_cast<uint32_t>(src32.gregs[detail::greg_rdx]);
            const uint32_t ebx = static_cast<uint32_t>(src32.gregs[detail::greg_rbx]);
            const uint32_t ebp = static_cast<uint32_t>(src32.gregs[detail::greg_rbp]);
            const uint32_t esi = static_cast<uint32_t>(src32.gregs[detail::greg_rsi]);
            const uint32_t edi = static_cast<uint32_t>(src32.gregs[detail::greg_rdi]);
            const uint32_t esp = static_cast<uint32_t>(src32.gregs[detail::greg_rsp]);
            const uint32_t eflags = reconstruct_compacted_eflags(src32);

            // The stub reached the thunk via `call fs:[0xC0]`, so [esp] is the 32-bit return address
            // (the `ret` after that call) and the syscall args follow the caller's own return slot:
            // [esp]=stub-ret, [esp+4]=caller-ret, [esp+8]=arg1. The transition must resume the stub at
            // its return address with esp advanced past it (as if `call fs:[0xC0]` returned normally).
            uint32_t return_eip = 0;
            if (!this->try_read_memory(esp, &return_eip, sizeof(return_eip)))
            {
                return false;
            }

            // Recompute the CpuArea CONTEXT block from the 64-bit engine's TEB64, exactly as the
            // forward crossing does (TEB64/CpuArea are high addresses, so the wow64 rebase is a no-op).
            const auto& state64 = this->thread_->CurrentFrame->State;
            uint64_t teb64 = 0;
            if (!this->try_read_memory(state64.gs_cached + 0x30, &teb64, sizeof(teb64)) || teb64 == 0)
            {
                return false;
            }
            uint64_t cpu_area = 0;
            if (!this->try_read_memory(teb64 + 0x1488, &cpu_area, sizeof(cpu_area)) || cpu_area == 0)
            {
                return false;
            }
            const uint64_t block = cpu_area + 0x80;

            // Reverse-marshal the full 32-bit register file into the CONTEXT block (what wow64cpu.dll's
            // 0x1779 does for the GPRs, plus xmm0..5 so the block is the authoritative 32-bit state
            // across the dispatch - the forward re-entry reads xmm back unconditionally). wow64.dll
            // overwrites CONTEXT.Eax@0x34 with the syscall result before that re-entry.
            const auto write32 = [&](uint64_t offset, uint32_t value) { this->write_marshal_state(block + offset, &value, sizeof(value)); };
            write32(0x20, edi);
            write32(0x24, esi);
            write32(0x28, ebx);
            write32(0x2c, edx);
            write32(0x30, ecx);
            write32(0x34, eax);
            write32(0x38, ebp);
            write32(0x3c, return_eip);
            write32(0x44, eflags);
            write32(0x48, esp + 4);
            for (int i = 0; i < 6; ++i)
            {
                this->write_marshal_state(block + 0xf0 + static_cast<uint64_t>(i) * 0x10, &src32.xmm.avx.data[i][0], 16);
            }

            // Set up the 64-bit engine to resume at the generic dispatcher (0x17af), which does
            // `mov ecx,eax; mov rdx,r11; call Wow64SystemServiceEx; mov [r13+0x34],eax; jmp 0x167f`.
            // Required: eax=service#, r13=CONTEXT block, r11=args pointer. The remaining GPRs are
            // carried live (harmless; the generic thunk reads its args from [r11]). r15 is still set to
            // the turbo jump table for parity even though 0x17af does not consult it. rsp is left frozen
            // at the 64-bit RunSimulatedCode stack (a valid writable stack for the dispatch call frame);
            // the re-entry rebuilds r12/r14 itself.
            auto& dst64 = this->thread_->CurrentFrame->State;
            dst64.rip = generic_dispatch;
            dst64.gregs[detail::greg_rax] = eax;
            dst64.gregs[detail::greg_rcx] = ecx;
            dst64.gregs[detail::greg_rdx] = edx;
            dst64.gregs[detail::greg_rbx] = ebx;
            dst64.gregs[detail::greg_rbp] = ebp;
            dst64.gregs[detail::greg_rsi] = esi;
            dst64.gregs[detail::greg_rdi] = edi;
            dst64.gregs[11] = static_cast<uint64_t>(esp) + 8; // R11 = args pointer (rebased on deref)
            dst64.gregs[13] = block;                          // R13 = CONTEXT block
            dst64.gregs[15] = jump_table;                     // R15 = turbo-thunk jump table

            this->active_context_ = this->context_.get();
            this->active_thread_ = this->thread_;

            return true;
        }

        // Performs a generic bitness-switch crossing given an already-decoded target RIP/RSP/CS:
        // marshals the architectural register file from the currently-active engine into whichever
        // engine target_cs selects, preserving the destination's own segment state, r12-r15, and
        // rax-rbx-rcx-rdx (all of which marshal_architectural_state would otherwise clobber with the
        // source's - see the comments below), then flips active_context_/active_thread_ so the next
        // ExecuteThread runs the destination engine from target_rip. Shared by the heaven's-gate
        // exception-delivery crossing (whose target_rip/rsp/cs come from registers dispatch_exception_
        // pointers set up) and the far-jmp CPU-mode-probe crossing (whose target_rip/cs come from
        // decoding the `jmp far` instruction's own immediate operand instead).
        bool perform_bitness_switch(const uint64_t target_rip, const uint64_t target_rsp, const uint16_t target_cs)
        {
            const auto& src = this->active_thread_->CurrentFrame->State;
            const bool target_is_64bit = (target_cs == wow64_user_code_selector_64bit);

            FEXCore::Context::Context* dst_context = nullptr;
            FEXCore::Core::InternalThreadState* dst_thread = nullptr;
            if (target_is_64bit)
            {
                // thread_ always exists by the time any crossing can fire - a wow64 process starts
                // executing 64-bit code (which created thread_) long before it can reach a gate.
                dst_context = this->context_.get();
                dst_thread = this->thread_;
            }
            else
            {
                // thread32_ is built eagerly in create_thread() (ordinary call context) - see its
                // doc comment for why lazily creating it from here (inside handle_fault_signal's
                // call chain) would be unsafe. Null here should be unreachable; fail the crossing
                // rather than allocate from the signal handler if that invariant is ever wrong.
                if (this->thread32_ == nullptr)
                {
                    return false;
                }
                dst_context = this->context32_.get();
                dst_thread = this->thread32_;
            }

            auto& dst = dst_thread->CurrentFrame->State;

            // A WoW64 bitness crossing must NOT carry the source engine's segment state into the
            // destination: each engine owns mode-appropriate FS/GS bases (the 64-bit engine's GS ->
            // TEB64, the 32-bit engine's FS -> TEB32). marshal_architectural_state copies the whole
            // segment block, which clobbered the 64-bit engine's GS base (TEB64) with the 32-bit
            // engine's (0) on the heaven's-gate exception path, so the 64-bit KiUserExceptionDispatcher
            // read gs:[0x30] against a null base and corrupted itself into a wild jump. Preserve the
            // destination engine's own segment selectors + cached bases across the marshal, mirroring
            // how the reverse gate (enter_wow64_64bit_from_wow64svc_thunk) leaves the 64-bit engine's
            // segments untouched. The destination is a continuously-live engine, so its selectors and
            // bases are already correct for its own mode.
            const auto saved_es_idx = dst.es_idx;
            const auto saved_cs_idx = dst.cs_idx;
            const auto saved_ss_idx = dst.ss_idx;
            const auto saved_ds_idx = dst.ds_idx;
            const auto saved_fs_idx = dst.fs_idx;
            const auto saved_gs_idx = dst.gs_idx;
            const auto saved_es_cached = dst.es_cached;
            const auto saved_cs_cached = dst.cs_cached;
            const auto saved_ss_cached = dst.ss_cached;
            const auto saved_ds_cached = dst.ds_cached;
            const auto saved_fs_cached = dst.fs_cached;
            const auto saved_gs_cached = dst.gs_cached;

            // Same reasoning as the segment preservation above, applied to r12-r15: these are the
            // wow64cpu-reserved registers the forward crossing populates on the 64-bit engine (thread_)
            // - r12/r13 = TEB64/CpuArea block, r14 = the frozen 64-bit exception stack real Windows'
            // Wow64PrepareForException reads via CONTEXT.R14 (see enter_wow64_32bit_from_run_simulated_code's
            // doc comment), r15 = the turbo jump table. marshal_architectural_state copies the FULL
            // register file including r8-r15, so crossing back into the 64-bit engine here (the
            // heaven's-gate exception-delivery path) overwrote thread_'s carefully-set r12-r15 with
            // whatever was in the 32-bit engine's (thread32_) same slots - meaningless SRA-spill garbage,
            // since 32-bit code cannot address r8-r15 at all. That garbage r14 then fed
            // Wow64PrepareForException's real stack-derivation logic, producing a wild address and a
            // second fault inside KiUserExceptionDispatcher itself. Preserve the destination engine's
            // own r12-r15 across the marshal exactly like the segment state above.
            const auto saved_r12 = dst.gregs[12];
            const auto saved_r13 = dst.gregs[13];
            const auto saved_r14 = dst.gregs[14];
            const auto saved_r15 = dst.gregs[15];

            // rax/rbx/rcx/rdx are the trampoline's OWN scratch registers here (see the convention
            // comment above: dispatch_exception_pointers stuffs rax=target RIP, rbx=target RSP,
            // rcx=target CS selector, rdx=target SS selector into the SOURCE (pre-crossing) engine
            // purely so the trampoline's iretq can consume them). They were never meant to be live
            // architectural state - target_rip/target_rsp are already captured above from src, and
            // target_cs was only needed to pick the destination engine. marshal_architectural_state
            // copies the whole GPR file though, so without this the destination engine's genuine
            // rax/rbx/rcx/rdx (whatever the guest was last doing with them) get clobbered by these
            // selector/address scratch values instead - the same class of leak the segment and
            // r12-r15 preservation above already guards against. Preserve them the same way.
            const auto saved_rax = dst.gregs[detail::greg_rax];
            const auto saved_rbx = dst.gregs[detail::greg_rbx];
            const auto saved_rcx = dst.gregs[detail::greg_rcx];
            const auto saved_rdx = dst.gregs[detail::greg_rdx];

            marshal_architectural_state(src, dst);

            dst.es_idx = saved_es_idx;
            dst.cs_idx = saved_cs_idx;
            dst.ss_idx = saved_ss_idx;
            dst.ds_idx = saved_ds_idx;
            dst.fs_idx = saved_fs_idx;
            dst.gs_idx = saved_gs_idx;
            dst.es_cached = saved_es_cached;
            dst.cs_cached = saved_cs_cached;
            dst.ss_cached = saved_ss_cached;
            dst.ds_cached = saved_ds_cached;
            dst.fs_cached = saved_fs_cached;
            dst.gs_cached = saved_gs_cached;
            dst.gregs[12] = saved_r12;
            dst.gregs[13] = saved_r13;
            dst.gregs[14] = saved_r14;
            dst.gregs[15] = saved_r15;
            dst.gregs[detail::greg_rax] = saved_rax;
            dst.gregs[detail::greg_rbx] = saved_rbx;
            dst.gregs[detail::greg_rcx] = saved_rcx;
            dst.gregs[detail::greg_rdx] = saved_rdx;

            dst.rip = target_rip;
            dst.gregs[detail::greg_rsp] = target_rsp;

            this->active_context_ = dst_context;
            this->active_thread_ = dst_thread;
            return true;
        }

        // gate.address here is a `jmp far 0x33:<target>` (opcode 0xEA - see gate_crossing_kind::
        // far_jmp_bitness_switch's doc comment). It was originally taken for a standalone, one-time
        // "can the CPU switch to 64-bit mode" hardware check, unrelated to the real syscall
        // dispatch path - but live traces prove otherwise: it's reached via the EXACT SAME route as
        // wow64cpu_dispatch's WOW64SVC thunk (the 32-bit syscall stub's `call fs:[0xC0]` /
        // Wow64Transition indirection lands here directly, with eax/edx already holding the
        // syscall number and the stub's own return address still on the stack, un-pushed-to by
        // anything in between). This IS wow64cpu.dll's real Wow64Transition entry point for this
        // build - a `jmp far` into 64-bit mode is simply how it happens to be implemented here,
        // rather than the turbo-bop mechanism wow64cpu_dispatch's gates model. Treat it exactly
        // like reaching the WOW64SVC thunk: reverse-marshal the 32-bit register file and resume at
        // the generic dispatcher, using the already-proven-correct logic verbatim.
        bool enter_bitness_switch_from_far_jmp(const gate_crossing& gate)
        {
            return this->enter_wow64_64bit_from_wow64svc_thunk(gate);
        }

        bool perform_gate_crossing(const gate_crossing& gate)
        {
            if (gate.kind == gate_crossing_kind::wow64_run_simulated_code)
            {
                return this->enter_wow64_32bit_from_run_simulated_code(gate);
            }

            if (gate.kind == gate_crossing_kind::wow64cpu_dispatch)
            {
                return this->enter_wow64_64bit_from_wow64svc_thunk(gate);
            }

            if (gate.kind == gate_crossing_kind::far_jmp_bitness_switch)
            {
                return this->enter_bitness_switch_from_far_jmp(gate);
            }

            // gate_crossing_kind::heaven_gate: confirmed trampoline convention (wow64_heaven_gate.hpp,
            // cross-checked against exception_dispatch.cpp which drives it programmatically) - the
            // trampoline's final iretq consumes RIP<-RAX, CS<-RCX, RFLAGS<-(pushfq), RSP<-RBX, SS<-RDX,
            // leaving the GPRs otherwise intact.
            const auto& src = this->active_thread_->CurrentFrame->State;
            return this->perform_bitness_switch(src.gregs[detail::greg_rax], src.gregs[detail::greg_rbx],
                                                static_cast<uint16_t>(src.gregs[detail::greg_rcx]));
        }

        // Returns the host address offset to add to `address` if it needs the wow64 rebase applied -
        // see wow64_guest_rebase_default's doc comment for why this is a per-instance member
        // (wow64_guest_rebase_) rather than a fixed constant. See wow64_guest_address_space_size's
        // doc comment for why is_32bit_mode alone isn't the gate - the address itself must also be
        // below that boundary. Unconditional (not Apple-only): this backend is shared with Linux
        // ARM64, which also needs it (wow64_guest_rebase_ just stays at its default there, since
        // reserve_wow64_host_window - the only thing that ever changes it - is Apple-only).
        uint64_t rebase_for(bool is_32bit_mode, uint64_t address) const
        {
            return (is_32bit_mode && address < wow64_guest_address_space_size) ? this->wow64_guest_rebase_ : 0ULL;
        }

        // Un-rebases a real hardware fault address (info->si_addr) back to the guest address space
        // when it falls in the wow64-rebased range a 32-bit context's guest memory actually lives in
        // (see rebase_for's doc comment) - a no-op in 64-bit mode. Needed anywhere a fault address is
        // compared against or dispatched to guest-address-keyed structures (mmio_regions_,
        // page_shadow_apple_, memory_violation_hooks_), as opposed to used directly as a real host
        // pointer (e.g. handle_misaligned_atomic_fault's memcpy), which must keep the original,
        // rebased address.
        uint64_t unrebase_fault_addr(uint64_t fault_addr) const
        {
            if (this->is_wow64_process_ && fault_addr >= this->wow64_guest_rebase_ &&
                fault_addr < this->wow64_guest_rebase_ + wow64_guest_address_space_size)
            {
                return fault_addr - this->wow64_guest_rebase_;
            }
            return fault_addr;
        }

#ifdef __APPLE__
      public:
        // Called (via the free-function signal handler below) for SIGSEGV/SIGBUS/SIGILL. Returns true
        // if this was FEXCore's own deliberate guest-exception trampoline (see JIT/MiscOps.cpp's
        // DEF_OP(Break) and Dispatcher.cpp's GuestSignal_SIG* labels) and has been handled; false for
        // anything else (a real host bug, or a genuine guest memory access violation the caller should
        // fall back to default disposition for - guest memory violations arrive as separate, later
        // work since they need real fault-address reconstruction, unlike this compile-time-known-
        // vector path).
        // Services a fault whose address falls inside a registered mmio_region (see its doc comment -
        // the region is deliberately left unmapped, so every access faults here). Decodes the single
        // faulting load instruction, satisfies it via the region's read callback, writes the result
        // into the destination register, and advances PC past it (fixed 4-byte width) to resume.
        // Applies a decode_arm64_load result once its data has been fetched (from an mmio_region's
        // read_cb, or a plain memcpy off real guest memory - see handle_mmio_fault and
        // handle_misaligned_atomic_fault) - writes the (possibly extended) value into the destination
        // register and advances PC past the single decoded instruction.
        void complete_decoded_load(ucontext_t* uctx, const decoded_arm64_load& decoded, const void* data, uint64_t pc)
        {
            if (decoded.is_vector)
            {
                __uint128_t value{};
                std::memcpy(&value, data, sizeof(value));
                auto* fprs = reinterpret_cast<__uint128_t*>(&uctx->uc_mcontext->__ns.__v[0]);
                fprs[decoded.rt] = value;
                arm_thread_state64_set_pc_fptr(uctx->uc_mcontext->__ss, reinterpret_cast<void*>(pc + 4));
                return;
            }

            uint64_t raw_value = 0;
            std::memcpy(&raw_value, data, decoded.size);

            uint64_t result = 0;
            switch (decoded.size)
            {
            case 1:
                result = decoded.sign_extend ? static_cast<uint64_t>(static_cast<int64_t>(static_cast<int8_t>(raw_value)))
                                             : (raw_value & 0xFFULL);
                break;
            case 2:
                result = decoded.sign_extend ? static_cast<uint64_t>(static_cast<int64_t>(static_cast<int16_t>(raw_value)))
                                             : (raw_value & 0xFFFFULL);
                break;
            case 4:
                result = decoded.sign_extend ? static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(raw_value)))
                                             : (raw_value & 0xFFFFFFFFULL);
                break;
            default:
                result = raw_value;
                break;
            }

            if (!decoded.dest_is_64bit)
            {
                // Writing Wt always zeroes bits 63:32 of the aliased Xt (AArch64 register semantics).
                result &= 0xFFFFFFFFULL;
            }

            if (decoded.rt <= 28)
            {
                uctx->uc_mcontext->__ss.__x[decoded.rt] = result;
            }
            else if (decoded.rt == 29)
            {
                uctx->uc_mcontext->__ss.__fp = result;
            }
            else if (decoded.rt == 30)
            {
                uctx->uc_mcontext->__ss.__lr = result;
            }
            // rt == 31 is XZR/WZR: the load's result is discarded, nothing to write back.

            arm_thread_state64_set_pc_fptr(uctx->uc_mcontext->__ss, reinterpret_cast<void*>(pc + 4));
        }

        bool handle_mmio_fault(ucontext_t* uctx, const mmio_region& region, uint64_t fault_addr)
        {
            const uint64_t pc = arm_thread_state64_get_pc(uctx->uc_mcontext->__ss);
            const auto insn = *reinterpret_cast<const uint32_t*>(pc);
            const auto decoded = decode_arm64_load(insn);
            if (!decoded)
            {
                // fprintf/stdio is not async-signal-safe (internal buffering/locking) - this runs
                // inside a real signal handler, so use snprintf into a fixed stack buffer followed by
                // a single write(2) instead, the standard pragmatic idiom for signal-handler-safe
                // formatted output (see jit_write_protect_retry_count_for's doc comment for the fuller
                // async-signal-safety rationale that motivated this).
                char buf[128];
                const int len = snprintf(buf, sizeof(buf), "[MMIO] unrecognized instruction 0x%08x at pc=%p for fault_addr=0x%llx\n", insn,
                                         reinterpret_cast<void*>(pc), static_cast<unsigned long long>(fault_addr));
                if (len > 0)
                {
                    const auto write_len = static_cast<size_t>(len) < sizeof(buf) ? static_cast<size_t>(len) : sizeof(buf);
                    ::write(STDERR_FILENO, buf, write_len);
                }
                return false;
            }

            alignas(16) std::byte buffer[16]{};
            region.read_cb(fault_addr - region.address, buffer, decoded->size);
            this->complete_decoded_load(uctx, *decoded, buffer, pc);
            return true;
        }

        // Real hardware LDAR/LDAPR/STLR (load-acquire/store-release) instructions require natural
        // alignment, unlike plain LDR/STR - but x86 permits unaligned accesses freely, and FEX uses
        // this family to model x86's stronger memory ordering on ARM's weaker one, so an ordinary
        // unaligned guest access to otherwise legitimately mapped memory can fault here (Darwin
        // reports it as SIGBUS/BUS_ADRALN). sogen runs every guest thread of a process cooperatively
        // on a single host thread (see windows_emulator.cpp's central loop), so there is no real
        // concurrent host-thread race for these instructions to order against here - downgrading to a
        // plain, non-atomic access is therefore correctness-preserving, not just a workaround.
        bool handle_misaligned_atomic_fault(ucontext_t* uctx, uint64_t fault_addr)
        {
            const uint64_t pc = arm_thread_state64_get_pc(uctx->uc_mcontext->__ss);
            const auto insn = *reinterpret_cast<const uint32_t*>(pc);

            if (const auto load = decode_arm64_load(insn))
            {
                this->complete_decoded_load(uctx, *load, reinterpret_cast<const void*>(fault_addr), pc);
                return true;
            }

            if (const auto store = decode_arm64_store(insn))
            {
                uint64_t value = 0;
                if (store->rt <= 28)
                {
                    value = uctx->uc_mcontext->__ss.__x[store->rt];
                }
                else if (store->rt == 29)
                {
                    value = uctx->uc_mcontext->__ss.__fp;
                }
                else if (store->rt == 30)
                {
                    value = uctx->uc_mcontext->__ss.__lr;
                }
                // rt == 31 is XZR: stores zero, matching the default-initialized value above.

                std::memcpy(reinterpret_cast<void*>(fault_addr), &value, store->size);
                arm_thread_state64_set_pc_fptr(uctx->uc_mcontext->__ss, reinterpret_cast<void*>(pc + 4));
                return true;
            }

            return false;
        }

        // memory_violation_hooks_/interrupt_hooks_ callbacks are shared, backend-agnostic
        // windows-emulator code (dispatch_exception and friends) that allocates, logs, and mutates
        // STL containers freely - safe when invoked from normal call context (as KVM/Unicorn do, after
        // a blocking syscall or interpreter callback returns), but NOT safe to call directly from
        // inside handle_fault_signal, a real kernel-delivered SIGSEGV/SIGBUS/SIGILL handler that can
        // interrupt an unrelated malloc()/free() or STL mutation already in progress on this thread -
        // confirmed to be a real, ASLR-timing-dependent heap-corruption hazard (see
        // jit_write_protect_retry_count_for's doc comment for the same class of bug at smaller scale).
        // Instead of calling hooks in-handler, stash what's needed here (plain data, no allocation) and
        // force ExecuteThread to unwind back to start() (via ThreadStopHandlerAddress, exactly like a
        // real stop - but without touching stop_requested_), which then dispatches the hook in normal
        // context and resumes guest execution by simply re-entering ExecuteThread: it always starts
        // fresh from CurrentFrame->State.rip, which is exactly what AbsoluteLoopTopAddressFillSRA
        // already re-derived SRA from, so this is behaviorally identical to the old in-handler resume.
        enum class pending_fault_kind
        {
            none,
            memory_violation,
            interrupt,
            // A WoW64 gate crossing already performed the state marshal + active_context_/
            // active_thread_ flip inside handle_fault_signal; this only tells start()'s loop to
            // resume (re-enter ExecuteThread on the now-active engine) rather than break. No hook runs.
            gate_crossing,
        };

        struct pending_fault_dispatch
        {
            pending_fault_kind kind = pending_fault_kind::none;
            uint64_t address = 0;
            size_t size = 0;
            memory_operation operation{};
            memory_violation_type type{};
            int vector = 0;
        };

        // Called only from start(), in normal call context, right after ExecuteThread returns - see
        // pending_fault_dispatch_'s doc comment. Returns true if a hook was actually dispatched (i.e.
        // ExecuteThread returned because handle_fault_signal deferred a hook, not because of a genuine
        // stop_requested_ - start()'s loop uses the return value to decide whether to resume).
        bool dispatch_pending_hook_if_any()
        {
            const pending_fault_dispatch dispatch = this->pending_fault_dispatch_;
            this->pending_fault_dispatch_.kind = pending_fault_kind::none;

            switch (dispatch.kind)
            {
            case pending_fault_kind::memory_violation:
                for (auto& [_, hook] : this->memory_violation_hooks_)
                {
                    hook(*this, dispatch.address, dispatch.size, dispatch.operation, dispatch.type);
                }
                return true;
            case pending_fault_kind::interrupt:
                for (auto& [_, hook] : this->interrupt_hooks_)
                {
                    hook(*this, dispatch.vector);
                }
                return true;
            case pending_fault_kind::gate_crossing:
                // The crossing itself already happened in-handler; nothing to dispatch. Return true
                // so start()'s loop re-enters ExecuteThread on the freshly-flipped active engine
                // (which resumes from its CurrentFrame->State.rip, set by perform_gate_crossing).
                return true;
            case pending_fault_kind::none:
            default:
                return false;
            }
        }

        // Called only from within handle_fault_signal (real signal-handler context) whenever a hook
        // needs to run. See pending_fault_dispatch_'s doc comment for why hooks can't be called
        // directly from here: stash the (plain-data, non-allocating) dispatch request and force
        // ExecuteThread to unwind back to start(), which dispatches it safely in normal call context
        // and then simply resumes by re-entering ExecuteThread - it always starts fresh from
        // CurrentFrame->State.rip, which the hook is free to redirect (e.g. into the guest's own
        // exception dispatcher), exactly as it could before when resumed via
        // AbsoluteLoopTopAddressFillSRA directly from here.
        //
        // sra_already_spilled distinguishes the two unwind entry points the dispatcher provides
        // (Dispatcher.cpp: ThreadStopHandlerAddressSpillSRA falls through SpillStaticRegs into
        // ThreadStopHandlerAddress's plain PopCalleeSavedRegisters+ret) - callers whose fault happened
        // via FEXCore's own controlled synthetic-exception path (vector==14/interrupt dispatch, where
        // SRA is already spilled to CpuStateFrame by the time this C++ code runs) must pass true;
        // callers interrupting arbitrary, uncontrolled points in live guest-translated JIT code (a
        // real hardware fault directly on translated code, see handle_general_memory_violation) must
        // pass false, since SRA is still live only in host registers there and skipping the spill
        // left stale/inconsistent state for the next ExecuteThread entry to read.
        void defer_hook_dispatch(ucontext_t* uctx, const pending_fault_dispatch& dispatch, bool sra_already_spilled)
        {
            this->pending_fault_dispatch_ = dispatch;
            const auto& cfg = this->signal_delegator_->GetConfig();
            const auto target = sra_already_spilled ? cfg.ThreadStopHandlerAddress : cfg.ThreadStopHandlerAddressSpillSRA;
            arm_thread_state64_set_pc_fptr(uctx->uc_mcontext->__ss, reinterpret_cast<void*>(target));
        }

        // Real (non-synthetic) guest memory violations: FEXCore's own vector-14 synthetic #PF
        // (NoExecOp, see handle_fault_signal) is handled separately, but an ordinary guest
        // load/store/instruction-fetch that directly faults - a real Windows PAGE_GUARD page,
        // genuinely unmapped memory, or a Category-3 shadow-table page (page_shadow_apple_'s doc
        // comment: mprotect'd to PROT_NONE because some 4KB guest slot within its host page is
        // guard/unmapped while another slot is legitimately mapped) - has no path to
        // memory_violation_hooks_ otherwise. Consult the shadow table for the *specific* 4KB guest
        // page the fault address falls in: if the requested operation exceeds what's declared there,
        // this is a real violation - classify it and defer_hook_dispatch (mirroring the existing
        // vector-14 branch). Otherwise the access is genuinely legitimate per the shadow (a false
        // fault from a stricter neighbor sharing the host page) - decode-and-emulate it exactly like
        // handle_misaligned_atomic_fault already does for a different fault kind (same technique,
        // reused directly).
        // True if a host PC lies inside either FEXCore Context's dispatcher trampoline. The dispatcher
        // is host MAP_JIT code (like a CodeBuffer) but IsAddressInCodeBuffer does not recognize it, so
        // the CodeBuffer-gated W^X retries in handle_fault_signal never fire for a dispatcher fault.
        // Both Contexts' dispatchers live in the same MAP_JIT arena and are covered by this thread's
        // single per-thread write-protect bit, so a dispatcher fault from either must be checked.
        bool host_pc_in_any_dispatcher(uint64_t pc) const
        {
            for (const auto* delegator : {this->signal_delegator_.get(), this->signal_delegator32_.get()})
            {
                if (delegator == nullptr)
                {
                    continue;
                }
                const auto& cfg = delegator->GetConfig();
                if (pc >= cfg.DispatcherBegin && pc < cfg.DispatcherEnd)
                {
                    return true;
                }
            }
            return false;
        }

        // FEXCore's call-ret shadow stack (REG_CALLRET_SP == x25, ensure_callret_buffer) is a return-
        // address predictor bracketed by a guard page on each side. A deep guest call chain - or a
        // guest stack pivot / longjmp that abandons already-pushed frames, as steam_api.dll's RLD DRM
        // does - legitimately underflows (or overflows) it past the committed region into a guard page.
        // Upstream FEX treats this as expected and recovers by resetting REG_CALLRET_SP to the buffer's
        // default location: Linux SyscallHandler::HandleSegfault (LinuxSyscalls/SyscallsSMCTracking.cpp)
        // and Windows FEX::Windows::CallRetStack::HandleAccessViolation (Source/Windows/Common/
        // CallRetStack.h, called from WOW64/Module.cpp) do exactly this. sogen's macOS backend set up
        // the buffer and its default location (matching GetCallRetStackInfo) but never ported the guard-
        // page fault recovery, so a guard-page hit fell through to handle_general_memory_violation,
        // which mis-read the host callret-stack address as a bogus guest access violation and crashed
        // marshaling a synthetic exception with it. Classify by shape (fault address inside the active
        // engine's callret allocation, guard pages included) rather than si_code - Darwin reports a
        // PROT_NONE guard-page hit as SEGV_ACCERR/SEGV_MAPERR/BUS_ADRALN interchangeably (see the
        // CodeBuffer-race comments) - and reset x25 to the default location, mirroring GetCallRetStackInfo
        // exactly (Base +- host_page guard, DefaultLocation = Base + CALLRET_STACK_SIZE/4). Strict no-op
        // for any fault outside the callret allocation.
        bool handle_callret_stack_fault(ucontext_t* uctx, uint64_t fault_addr) const
        {
            if (this->active_thread_ == nullptr || this->active_thread_->CallRetStackBase == nullptr)
            {
                return false;
            }
            const auto base = reinterpret_cast<uint64_t>(this->active_thread_->CallRetStackBase);
            const auto host_page = static_cast<uint64_t>(::getpagesize());
            constexpr uint64_t callret_stack_size = FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE;
            if (fault_addr < base - host_page || fault_addr >= base + callret_stack_size + host_page)
            {
                return false;
            }
            uctx->uc_mcontext->__ss.__x[25] = base + callret_stack_size / 4;
            return true;
        }

        bool handle_general_memory_violation(ucontext_t* uctx, uint64_t fault_addr)
        {
            const uint64_t pc = arm_thread_state64_get_pc(uctx->uc_mcontext->__ss);
            const auto guest_fault_addr = this->unrebase_fault_addr(fault_addr);
            const auto guest_page = guest_fault_addr & ~(page_size - 1);
            const auto shadow_it = this->page_shadow_apple_.find(guest_page);
            const auto declared = (shadow_it != this->page_shadow_apple_.end()) ? shadow_it->second : memory_permission::none;

            memory_operation operation = memory_operation::exec;
            if (fault_addr != pc)
            {
                const auto insn = *reinterpret_cast<const uint32_t*>(pc);
                operation = decode_arm64_store(insn) ? memory_operation::write : memory_operation::read;
            }

            if ((declared & operation) == operation)
            {
                return this->handle_misaligned_atomic_fault(uctx, fault_addr);
            }

            const auto type = (declared == memory_permission::none) ? memory_violation_type::unmapped : memory_violation_type::protection;

            // This fault interrupted live guest-translated JIT code at an arbitrary point. FEX's call-ret
            // block-chaining (directly-linked blocks and callret RET fast-paths) advances execution
            // WITHOUT rewriting CurrentFrame->State.rip - it holds whatever was last written to it (e.g. a
            // prior syscall's fallthrough or a gate resume PC), so it is frequently STALE here. The
            // memory-violation hook (and the synthetic exception record it dispatches to the guest) reads
            // State.rip as the faulting instruction pointer, so without this it reports a misleading PC -
            // unlike Unicorn/native, which are instruction-precise and report the true faulting insn.
            // Reconstruct the real guest rip from the live host PC (the same mechanism FEX's own
            // suspend-time ReconstructThreadState and the InterruptFaultPage cooperative-stop path use);
            // the host PC is squarely inside a compiled block here, so this resolves accurately. Guard on
            // a non-zero result so a failed reconstruction never zeroes a usable stale rip.
            if (const uint64_t recon_rip = this->active_context_->RestoreRIPFromHostPC(this->active_thread_, pc))
            {
                this->active_thread_->CurrentFrame->State.rip = recon_rip;
            }

            pending_fault_dispatch dispatch{};
            dispatch.kind = pending_fault_kind::memory_violation;
            dispatch.address = guest_fault_addr;
            dispatch.size = 1;
            dispatch.operation = operation;
            dispatch.type = type;

            // SRA is still live only in host registers here - this fault interrupted guest-translated
            // JIT code at an arbitrary point, not FEXCore's own controlled synthetic-exception path.
            this->defer_hook_dispatch(uctx, dispatch, /*sra_already_spilled=*/false);
            return true;
        }

        bool handle_fault_signal(int sig, siginfo_t* info, void* raw_ucontext)
        {
            if (this->active_thread_ == nullptr)
            {
                return false;
            }

            auto* uctx = static_cast<ucontext_t*>(raw_ucontext);

            if (sig == SIGSEGV || sig == SIGBUS)
            {
                const auto fault_addr = reinterpret_cast<uint64_t>(info->si_addr);

                // ROOT CAUSE FIX (confirmed via extensive bisection - a stack canary in
                // windows_emulator::start() caught precise corruption of the main thread's own
                // native stack, correlated exactly with request_thread_stop()'s mprotect() call and
                // with a real fault actually occurring on InterruptFaultPage, but NOT with which of
                // the two paths below (redirect vs skip) ends up handling it - which pointed at this
                // check running too late): this used to live further down, reached only via
                // SEGV_ACCERR/SEGV_MAPERR falling through the BUS_ADRALN-CodeBuffer check below. But
                // just like that CodeBuffer race (see the BUS_ADRALN branch's own comment), Darwin can
                // report this exact same PROT_NONE violation as BUS_ADRALN instead of the expected
                // SEGV_ACCERR/SEGV_MAPERR. Since InterruptFaultPage's address is never inside the
                // CodeBuffer, a misclassified BUS_ADRALN fault here used to fall past that check
                // straight into handle_general_memory_violation, which unconditionally treats
                // fault_addr as a *guest* address (unrebase_fault_addr/page_shadow_apple_ lookup) and
                // dispatches a synthetic guest memory-violation exception with that bogus "guest
                // address" (really just this backend's own internal heap pointer) - corrupting
                // whatever the resulting nonsense exception dispatch touches downstream. Checking this
                // first, before any signal/si_code-specific branch, means every InterruptFaultPage
                // fault is caught here regardless of how Darwin classifies it.
                const auto interrupt_page_addr = reinterpret_cast<uint64_t>(this->active_thread_->InterruptFaultPage);
                if (fault_addr >= interrupt_page_addr &&
                    fault_addr < interrupt_page_addr + sizeof(this->active_thread_->InterruptFaultPage))
                {
                    const auto fault_pc = arm_thread_state64_get_pc(uctx->uc_mcontext->__ss);
                    const bool is_dispatch_code =
                        this->active_context_ && this->active_context_->IsAddressInCodeBuffer(this->active_thread_, fault_pc);

                    // ExitFunctionLinkerAddress's OWN epilogue (EmitSignalGuardedRegion's closing
                    // sequence, Dispatcher.cpp) also writes to InterruptFaultPage from inside the
                    // CodeBuffer - via a `strb`, functionally identical to the DeferredSignalRefCount
                    // Guard host-C++ destructor below, just JIT-emitted. IsAddressInCodeBuffer alone
                    // can't tell this apart from a genuine per-block-entry interrupt check
                    // (EmitSuspendInterruptCheck's 64-bit `str`/128-bit vector `str`, JIT.cpp) - both
                    // are "inside the CodeBuffer". Redirecting to ThreadStopHandlerAddress while
                    // actually mid-trampoline-epilogue pops the dispatcher's own frame at the wrong
                    // stack depth. Distinguish via the raw instruction word: STRB (unsigned-offset
                    // immediate) always encodes with size=00,V=0,opc=00 - genuinely distinct from both
                    // of EmitSuspendInterruptCheck's forms (64-bit `str` has size=11; 128-bit vector
                    // `str` has V=1) - so this mask catches only the epilogue's strb, never either
                    // genuine block-entry form.
                    const bool is_strb_epilogue_write = (*reinterpret_cast<const uint32_t*>(fault_pc) & 0xFFC00000u) == 0x39000000u;

                    if (is_dispatch_code && !is_strb_epilogue_write)
                    {
                        // ROOT CAUSE FIX (Wall A / the long-standing intermittent import-snap NoExec).
                        // This cooperative stop is being taken at a genuine JIT block-entry / loop
                        // back-edge interrupt check (EmitSuspendInterruptCheck, JIT.cpp), triggered by
                        // the quantum-timer thread's async mprotect of InterruptFaultPage. At such a
                        // point the LIVE guest state is in host registers (SRA) and CPUState.rip holds
                        // whatever was last written to it - which is frequently STALE: a completed
                        // syscall leaves rip at its fallthrough (HandleSyscall sets rip = syscall+2),
                        // and execution then runs on through directly-linked blocks / callret RET
                        // fast-paths that never rewrite CPUState.rip. The previous redirect jumped
                        // straight to the NON-spill ThreadStopHandlerAddress, so on resume ExecuteThread
                        // re-entered from that stale rip with stale gregs - re-executing an already-
                        // retired instruction. Observed failure: an NtProtectVirtualMemory stub `retn`
                        // re-run after a later, shallower RtlpImageDirectoryEntryToDataEx push had
                        // already reused/overwritten its return slot, so the re-run popped garbage ->
                        // wild branch -> "NoExec instruction in entry block". Fix: reconstruct the real
                        // guest rip from the faulting host PC and redirect through the SpillSRA stop
                        // handler so the live SRA GPRs/FPRs/flags are written back to CPUState before
                        // ExecuteThread returns. This mirrors FEX's own suspend-time reconstruction
                        // (Source/Windows/WOW64/Module.cpp ReconstructThreadState, which does exactly
                        // RestoreRIPFromHostPC + SRA spill). Both halves are required: without the rip
                        // reconstruction resume lands on the stale instruction; without the SRA spill it
                        // resumes with stale registers.
                        this->active_thread_->CurrentFrame->State.rip =
                            this->active_context_->RestoreRIPFromHostPC(this->active_thread_, fault_pc);
                        const auto& stop_cfg = this->signal_delegator_->GetConfig();
                        arm_thread_state64_set_pc_fptr(uctx->uc_mcontext->__ss,
                                                       reinterpret_cast<void*>(stop_cfg.ThreadStopHandlerAddressSpillSRA));
                        return true;
                    }

                    // Not a genuine block-entry check - either FEXCore's own
                    // DeferredSignalRefCountGuard destructor (SignalScopeGuards.h, host C++ code) or
                    // ExitFunctionLinkerAddress's own JIT-emitted epilogue strb (both write to this
                    // same page as ordinary bookkeeping, coincidentally racing with a stop request
                    // from another thread, e.g. the quantum timer, that just mprotect'd the page).
                    // Redirecting to ThreadStopHandlerAddress here would be wrong in either case -
                    // that entry point expects to unwind a live JIT dispatcher stack frame, not
                    // whatever is actually executing at the moment of the race (this was in fact the
                    // actual root cause of a long-standing intermittent pc==lr==0 crash - see task
                    // #14's investigation, for the host-C++-side instance of this same class of bug).
                    // The store's actual value is inconsequential - only the page's protection state
                    // drives the cooperative-stop mechanism - so just skip the single faulting store
                    // instruction; the next real JIT block entry will still see the page protected
                    // and stop correctly.
                    arm_thread_state64_set_pc_fptr(uctx->uc_mcontext->__ss, reinterpret_cast<void*>(fault_pc + 4));
                    return true;
                }

                // A W^X (write-XOR-execute) instruction-fetch fault on FEXCore's own dispatcher
                // trampoline. The dispatcher is host MAP_JIT code, just like a CodeBuffer, but
                // IsAddressInCodeBuffer does not recognize it, so the CodeBuffer-gated W^X retries
                // below never fire for it. This only surfaces once a second FEXCore Context exists (the
                // 32-bit wow64 context32_): its dispatcher/blocks are lazily compiled from inside the
                // gate-crossing signal handler, which can leave this thread's per-thread JIT write-
                // protect in write mode, so re-entering either Context's dispatcher then faults on the
                // instruction fetch (fault address == pc). Darwin reports this as SIGSEGV or SIGBUS with
                // any of SEGV_ACCERR/SEGV_MAPERR/BUS_ADRALN (see the CodeBuffer-race comments below for
                // the same si_code ambiguity), so classify by shape - an instruction fetch (fault_addr
                // == pc) inside a known dispatcher range - rather than by si_code, and toggle execute
                // mode and retry the identical instruction.
                //
                // This is deliberately NOT bounded by the per-address retry budget the CodeBuffer cases
                // use. A host pc inside the dispatcher is unambiguously FEXCore's own code executing
                // (never a wild guest branch - guest addresses rebase elsewhere), and the dispatcher is
                // genuine RWX-capable MAP_JIT, so toggling execute mode ALWAYS lets the fetch succeed and
                // execution proceeds - it can never spin. A WoW64 syscall round-trip compiles many blocks
                // back-to-back, each leaving the thread in write mode, so the very same dispatcher entry
                // legitimately faults far more than a handful of times in rapid succession (never letting
                // the 100ms reset window fire); a small budget spuriously exhausted here, dropping the
                // fault through to handle_general_memory_violation which mis-read the host dispatcher
                // address as a bogus guest access violation and crashed marshaling it to the guest stack.
                {
                    const auto host_pc = arm_thread_state64_get_pc(uctx->uc_mcontext->__ss);
                    if (fault_addr == host_pc && this->host_pc_in_any_dispatcher(host_pc))
                    {
                        ::pthread_jit_write_protect_np(1);
                        return true;
                    }
                }

                // A call-ret shadow-stack guard-page hit (underflow/overflow) - reset REG_CALLRET_SP to
                // the buffer's default location and resume, exactly as upstream FEX does. Checked early,
                // by shape, before the general-violation routing that would otherwise mis-dispatch this
                // host arena address as a bogus guest access violation (see the helper's doc comment).
                if (this->handle_callret_stack_fault(uctx, fault_addr))
                {
                    return true;
                }

                // An instruction-fetch fault reports si_addr == the faulting pc itself (a real MMIO
                // data access from a mapped mmio_region's guest address never coincides with a live
                // code address, so this is never a false negative for a genuine MMIO hit). Excluding
                // it here matters: if the underlying root cause is a bad branch to a garbage/null pc
                // (root-caused elsewhere, not by this backend's fault handling), that garbage address
                // can coincidentally fall inside some registered mmio_region's range purely by chance -
                // routing it into handle_mmio_fault would then try to decode "the instruction at pc"
                // from that same garbage/unmapped address and crash again there instead, which is a
                // confusing secondary symptom of the real bug, not a new one. Let it fall through to
                // the ordinary unhandled-signal report untouched.
                const auto pc_for_mmio_check = arm_thread_state64_get_pc(uctx->uc_mcontext->__ss);
                if (fault_addr != pc_for_mmio_check)
                {
                    // mmio_regions_ is keyed by guest address (see unrebase_fault_addr's doc
                    // comment) - a 32-bit guest's real host access lands at guest_addr +
                    // wow64_guest_rebase, so it must be un-rebased before matching here.
                    const auto guest_fault_addr = this->unrebase_fault_addr(fault_addr);
                    for (const auto& region : this->mmio_regions_)
                    {
                        if (guest_fault_addr >= region.address && guest_fault_addr < region.address + region.size)
                        {
                            return this->handle_mmio_fault(uctx, region, guest_fault_addr);
                        }
                    }
                }

                // ROOT CAUSE FIX (confirmed via the fault_event ring buffer + a raw instruction-word
                // capture): a BUS_ADRALN fault whose *address* falls inside the live JIT CodeBuffer,
                // but whose *PC* is FEXCore's own host C++ code (e.g. ExitFunctionLink's self-
                // modifying-write path), decodes to a perfectly ordinary, 4-byte-aligned 32-bit `str`
                // (confirmed by capturing the raw instruction word: 0xb900032a = `str w10, [x25]`, no
                // offset, size=32-bit, not an exclusive/ordered form at all) - i.e. this is NOT a real
                // alignment fault (plain STR never requires alignment on ARM64, and this address is
                // aligned anyway). This is the JIT write-XOR-execute race - the CodeBuffer is currently
                // execute-only - which Darwin evidently sometimes reports via BUS_ADRALN instead of the
                // expected SEGV_ACCERR/SEGV_MAPERR, mirroring the already-documented SEGV_MAPERR-
                // instead-of-SEGV_ACCERR quirk for the exact same underlying mechanism (see the
                // JITGuardPage comment below). Before this fix, such a fault fell into the BUS_ADRALN
                // branch further down unconditionally, which routes to handle_general_memory_violation
                // - designed for genuine guest memory accesses, it unwinds via
                // defer_hook_dispatch/ThreadStopHandlerAddress as if interrupting the JIT dispatcher's
                // own call frame. That's wrong here: execution is several real C++ call frames deep
                // inside FEXCore's own code (dispatcher trampoline -> embedder wrapper ->
                // ExitFunctionLink), so popping "the dispatcher's" callee-saved registers off the stack
                // pops whatever's actually there instead - corrupting STATE (x28) and other SRA
                // registers with stack garbage, which then crashes on the *next* unlinked call with an
                // unrelated-looking null-Frame dereference. Confirmed directly: the ring buffer's very
                // first captured fault was exactly this, immediately preceding the corrupted-STATE
                // crash this investigation chased for two sessions.
                //
                // Fix: treat it exactly like the SEGV_ACCERR/SEGV_MAPERR write-protect race just below
                // - toggle write access on and retry the identical instruction, bounded by the same
                // per-address retry counter (a genuinely different bug at this exact address, rather
                // than an unresolvable race, would still eventually surface as unhandled after
                // max_write_protect_retries, not spin forever).
                if (sig == SIGBUS && info->si_code == BUS_ADRALN && this->active_context_ &&
                    this->active_context_->IsAddressInCodeBuffer(this->active_thread_, fault_addr))
                {
                    auto& retry_count = jit_write_protect_retry_count_for(fault_addr);
                    constexpr int max_write_protect_retries = 4;
                    if (retry_count < max_write_protect_retries)
                    {
                        ++retry_count;
                        ::pthread_jit_write_protect_np(0);
                        return true;
                    }
                }

                // See handle_misaligned_atomic_fault's doc comment: BUS_ADRALN is Darwin's alignment-
                // fault si_code, specific enough that this is never confused with a real access
                // violation (SEGV_ACCERR/SEGV_MAPERR, handled separately below). Routed through
                // handle_general_memory_violation rather than calling handle_misaligned_atomic_fault
                // directly: that doc comment's "otherwise legitimately mapped memory" assumption isn't
                // actually guaranteed - a guest instruction can compute a genuinely garbage/unmapped
                // address (a real access violation that merely happens to also be unaligned), and
                // blindly memcpy-ing to/from it would fault a second time inside the signal handler
                // itself, surfacing as an unhandled crash instead of a normal guest exception. Going
                // through the shadow-table-validated path first means a genuinely bad address gets
                // correctly classified and raised via memory_violation_hooks_ instead. The CodeBuffer
                // case above is handled first and returns early, so by this point fault_addr is known
                // not to be a CodeBuffer address - this is a genuine guest-memory BUS_ADRALN.
                if (sig == SIGBUS && info->si_code == BUS_ADRALN && this->handle_general_memory_violation(uctx, fault_addr))
                {
                    return true;
                }
            }

            // JIT code-buffer overflow guard: FEXCore protects the last host page of each CodeBuffer
            // (CPUBackend.cpp's CodeBuffer constructor) and deliberately writes into it mid-compile to
            // detect running out of space. Not a real bug - resume via the jump-buffer FEXCore already
            // set up before compiling started (mirrors SignalDelegator.cpp's
            // HandleFrontendSIGSEGV/ManuallyLoadJumpBuf). Darwin can report this access violation as
            // either SIGSEGV or SIGBUS depending on the exact protection-fault kind, unlike Linux's
            // single SIGSEGV, so both are checked here. Empirically, Darwin also sometimes reports
            // this exact MAP_JIT write-XOR-execute violation as SEGV_MAPERR (si_code=1, normally
            // "not mapped at all") rather than SEGV_ACCERR (si_code=2, normally "mapped, wrong
            // permission") - confirmed by querying mach_vm_region for the fault address from within
            // this handler and finding it fully mapped RWX (region_prot=7) despite the si_code=1
            // report, so both si_codes are treated the same way below.
            if ((sig == SIGSEGV || sig == SIGBUS) && (info->si_code == SEGV_ACCERR || info->si_code == SEGV_MAPERR))
            {
                const auto guard_page = this->active_thread_->JITGuardPage;
                const auto fault_addr = reinterpret_cast<uintptr_t>(info->si_addr);
                if (guard_page != 0 && fault_addr >= guard_page && fault_addr < guard_page + FEXCore::Utils::FEX_HOST_PAGE_SIZE)
                {
                    auto* gprs = reinterpret_cast<uint64_t*>(&uctx->uc_mcontext->__ss);
                    auto* fprs = reinterpret_cast<__uint128_t*>(&uctx->uc_mcontext->__ns.__v[0]);
                    auto* pc_ptr = reinterpret_cast<uint64_t*>(&uctx->uc_mcontext->__ss.__pc);
                    FEXCore::UncheckedLongJump::ManuallyLoadJumpBuf(this->active_thread_->RestartJump,
                                                                    this->active_thread_->JITGuardOverflowArgument, gprs, fprs, pc_ptr);
                    return true;
                }

                // See g_jit_write_protect_retry_counts' doc comment: FEXCore's own code-patching paths
                // (ExitFunctionLink, block delinkers) don't reliably leave this thread's JIT write-
                // protect state correct for the duration of their self-modifying writes into a
                // CodeBuffer. Set it to whatever the faulting access actually needs and retry the
                // exact same faulting instruction (PC/registers otherwise untouched) rather than
                // treating this as fatal - bounded per fault address so a genuinely different bug
                // can't spin forever. An instruction-fetch fault (PC == fault address) needs execute
                // mode (1); a data write needs write mode (0) - guessing the wrong direction here
                // would just re-fault immediately and consume a retry harmlessly.
                //
                // Gated on IsAddressInCodeBuffer(fault_addr) - this branch previously fired for *any*
                // SEGV_ACCERR/SEGV_MAPERR regardless of the fault address, silently wasting up to
                // max_write_protect_retries toggling W^X for faults that were never a CodeBuffer
                // write-protect race at all (confirmed via the fault_event ring buffer: a genuine
                // branch-to-null, pc==fault_addr==0, was retried 4 times this way before falling
                // through as unhandled - toggling JIT write-protection has nothing to do with a null
                // pointer, and doing so pointlessly here is itself a needless, easily-avoided risk).
                if (this->active_context_ && this->active_context_->IsAddressInCodeBuffer(this->active_thread_, fault_addr))
                {
                    const auto fault_addr_u64 = reinterpret_cast<uint64_t>(info->si_addr);
                    auto& retry_count = jit_write_protect_retry_count_for(fault_addr_u64);
                    constexpr int max_write_protect_retries = 4;
                    if (retry_count < max_write_protect_retries)
                    {
                        ++retry_count;
                        const uint64_t faulting_pc = arm_thread_state64_get_pc(uctx->uc_mcontext->__ss);
                        const bool is_instruction_fetch = (faulting_pc == fault_addr_u64);
                        ::pthread_jit_write_protect_np(is_instruction_fetch ? 1 : 0);
                        return true;
                    }
                }
            }

            const uint64_t pc = arm_thread_state64_get_pc(uctx->uc_mcontext->__ss);

            // FEXCore's guest-exception trampoline always re-enters within a dispatcher range. In a
            // WoW64 process there are TWO dispatchers (the 64-bit context_ and the 32-bit context32_),
            // and a synthetic #PF raised by 32-bit guest code (e.g. the reverse WoW64SVC gate's NoExecOp
            // BreakOp) re-enters the 32-bit dispatcher - which lies OUTSIDE the 64-bit delegator's
            // [DispatcherBegin,DispatcherEnd). Checking only the 64-bit range here rejected every
            // 32-bit-originated synthetic fault as "not the trampoline", so the reverse gate crossing
            // was never performed. Accept a re-entry into EITHER dispatcher.
            if (!this->host_pc_in_any_dispatcher(pc))
            {
                // Not FEXCore's own guest-exception trampoline (that always re-enters within the
                // dispatcher range). The common case: real, translated guest code faulted directly -
                // see handle_general_memory_violation. Only attempt that once we've confirmed pc is
                // genuinely inside a live JIT code buffer (the public IsAddressInCodeBuffer API) -
                // otherwise this is a real host bug elsewhere that we have no business trying to
                // interpret as guest state; the signal handler wrapper below logs and re-raises it.
                if ((sig == SIGSEGV || sig == SIGBUS) && this->active_context_ &&
                    this->active_context_->IsAddressInCodeBuffer(this->active_thread_, pc) &&
                    this->handle_general_memory_violation(uctx, reinterpret_cast<uint64_t>(info->si_addr)))
                {
                    return true;
                }

                return false;
            }

            auto* frame = this->active_thread_->CurrentFrame;
            if (!frame->SynchronousFaultData.FaultToTopAndGeneratedException)
            {
                return false;
            }

            // FEXCore's IR "Break" op raises this both for x86 conditions with a compile-time-known
            // trap vector (HLT/UD2/INT3/INT1/INTO/unhandled INT N) and for its own synthetic #PF
            // (X86_TRAPNO_PF, e.g. NoExecOp when QueryGuestExecutableRange reports an address isn't
            // executable). Vector 14 is therefore a real memory-access-violation-shaped event and
            // needs the fault address; everything else is a plain CPU exception vector. Mirrors the
            // KVM backend's #PF vs. other-vector split in handle_exception() (kvm_x86_64_emulator.cpp).
            auto vector = static_cast<int>(frame->SynchronousFaultData.TrapNo);

            // sogen has no guest IDT (this is a user-mode-only emulator - there's no kernel to
            // populate one), so a guest `INT N` FEXCore can't dispatch directly synthesizes a real
            // #GP(13) whose error code names the referenced IDT selector (bit1 set, selector index in
            // bits[15:3]) - the same effect real hardware produces for an unprivileged/absent IDT gate.
            // Unicorn/KVM don't model IDT lookups at all and report `INT N` as vector N directly, so
            // remap FEX's more architecturally faithful #GP back to the plain vector windows_emulator.cpp's
            // shared interrupt dispatch already expects - otherwise e.g. a CFG/__fastfail `int 0x29`
            // re-faults on the same instruction forever instead of reaching fast-fail dispatch.
            constexpr int gp_fault_vector = 13;
            constexpr uint32_t idt_reference_bit = 0x2;
            if (vector == gp_fault_vector && (frame->SynchronousFaultData.err_code & idt_reference_bit) != 0)
            {
                vector = static_cast<int>(frame->SynchronousFaultData.err_code >> 3);
            }

            // Must be reset before deferring the hook dispatch (not after) - it gates re-entry into
            // this branch (see the check above), and the hook may not actually run until start() gets
            // around to it in normal context; the next real fault of this shape must not be swallowed
            // in the meantime.
            frame->SynchronousFaultData.FaultToTopAndGeneratedException = false;

            pending_fault_dispatch dispatch{};
            if (vector == 14)
            {
                // A registered WoW64 gate crossing surfaces here as a synthetic #PF (its range is
                // reported non-executable by QueryGuestExecutableRange, so FEXCore refuses to compile
                // the mode-switch bytes and Break-ops instead). Perform the actual bitness switch
                // rather than dispatching a memory violation.
                if (const auto* gate = this->find_gate_crossing(frame->State.rip))
                {
                    // Capture the source (currently-active, pre-crossing) engine's dispatcher stop
                    // handler *before* perform_gate_crossing flips active_context_: the in-flight
                    // ExecuteThread that must unwind belongs to the source Context, so it has to
                    // return through that Context's own ThreadStopHandlerAddress. Using the
                    // destination's would re-enter the wrong dispatcher.
                    auto* const source_signal_delegator =
                        (this->active_context_ == this->context32_.get()) ? this->signal_delegator32_.get() : this->signal_delegator_.get();

                    if (this->perform_gate_crossing(*gate))
                    {
                        this->pending_fault_dispatch_.kind = pending_fault_kind::gate_crossing;
                        // SRA is already spilled here (FEXCore's controlled synthetic-#PF/Break-op
                        // path), so use the plain ThreadStopHandlerAddress - same rationale as the
                        // sra_already_spilled=true memory-violation dispatch below.
                        const auto& stop_cfg = source_signal_delegator->GetConfig();
                        arm_thread_state64_set_pc_fptr(uctx->uc_mcontext->__ss, reinterpret_cast<void*>(stop_cfg.ThreadStopHandlerAddress));
                        return true;
                    }
                    // A gate handler that returns false (a genuine memory-read failure while
                    // marshaling) falls through to the ordinary memory-violation dispatch rather than
                    // resuming into half-marshaled register state.
                }

                const auto err_code = frame->SynchronousFaultData.err_code;
                // NoExecOp (the only current producer of a synthetic #PF) always faults on the
                // instruction fetch at the current guest RIP - there is no separate stored fault
                // address (real x86 would use CR2), so RIP is the only correct source for now.
                const bool is_write = (err_code & 0x2) != 0;
                const bool is_instr_fetch = (err_code & 0x10) != 0;
                dispatch.kind = pending_fault_kind::memory_violation;
                dispatch.address = frame->State.rip;
                dispatch.size = 1;
                dispatch.operation = is_instr_fetch ? memory_operation::exec : is_write ? memory_operation::write : memory_operation::read;
                dispatch.type = (err_code & 0x1) ? memory_violation_type::protection : memory_violation_type::unmapped;
            }
            else
            {
                dispatch.kind = pending_fault_kind::interrupt;
                dispatch.vector = vector;
            }

            // See defer_hook_dispatch's doc comment: the hook runs later, in normal call context, once
            // start() dispatches it - it may call this->stop() synchronously (e.g. the fast-fail path),
            // which start()'s loop checks for after dispatching, matching this function's old behavior
            // of redirecting into ThreadStopHandlerAddress instead of resuming when that happens. SRA
            // is already spilled here (this is FEXCore's own controlled synthetic-exception/Break-op
            // path, not an arbitrary interruption of live JIT code), matching this function's own old
            // (pre-hook-deferral) comment justifying the non-spilling ThreadStopHandlerAddress variant.
            this->defer_hook_dispatch(uctx, dispatch, /*sra_already_spilled=*/true);
            return true;
        }
#endif

      private:
        void create_thread()
        {
            // Seed the FEX thread from the staged CPUState the loader populated before the first start().
            this->thread_ =
                this->context_->CreateThread(this->staged_state_.rip, this->staged_state_.gregs[detail::greg_rsp], &this->staged_state_);
            // active_context_/active_thread_ start out equal to context_/thread_ - see their doc
            // comment - and this is the only thing that ever runs until Phase B's gate-crossing
            // exists, so reflect the newly-created thread as the active one right away.
            this->active_thread_ = this->thread_;

            // FEXCore's core does not set up the "call-ret stack" (its own dedicated shadow stack for
            // x86 CALL/RET emulation, SRA-mapped to callret_sp) - on Linux this is embedder glue
            // (ThreadManager::CreateThread, Source/Tools/LinuxEmulation/LinuxSyscalls/ThreadManager.cpp)
            // that has to be replicated here: without it, the very first x86 CALL in JIT-compiled code
            // dereferences a null callret_sp and crashes. See ensure_callret_stack's doc comment for
            // why each logical guest thread needs its own, not just this first one.
            this->ensure_callret_stack(this->thread_->CurrentFrame->State);

#ifdef __APPLE__
            // See exit_function_link_jit_write_wrapper's doc comment: intercept the plain function-
            // pointer slot JIT-compiled code calls through to patch call sites, so the write into the
            // (MAP_JIT) code buffer happens with this thread's JIT write-protection disabled.
            g_original_exit_function_link = this->thread_->CurrentFrame->Pointers.ExitFunctionLink;
            this->thread_->CurrentFrame->Pointers.ExitFunctionLink = reinterpret_cast<uint64_t>(&exit_function_link_jit_write_wrapper);
#endif

            // Build thread32_ here too, in this ordinary call context, rather than leaving it to be
            // lazily created on the process's first gate crossing - that crossing is only ever
            // reached from inside handle_fault_signal (a signal handler), and create_thread32()
            // does real heap allocation (FEXCore::Context::CreateThread, SignalDelegator
            // construction, InitCore()). By the time create_thread() runs (called from start(),
            // never from a signal handler), is_wow64_process_ and gdt_base_ are both already set
            // (notify_process_bitness() and load_gdt() both run before start()), so there's nothing
            // create_thread32() needs that isn't ready yet.
            if (this->is_wow64_process_ && this->thread32_ == nullptr)
            {
                this->create_thread32();
            }
        }

        // FEXCore's call-ret shadow stack (callret_sp, see CoreState.h) has no notion of "logical
        // guest thread" - it's just a raw pointer into whatever host buffer this sets up. sogen models
        // multiple logical guest threads as CPUState-sized snapshots swapped in and out of this one
        // FEXCore thread (see save_registers/restore_registers); if every logical thread's callret_sp
        // pointed at the same buffer, a thread suspended mid-call-chain (e.g. blocked in a syscall,
        // with pending pushed return addresses) would have those frames corrupted the moment a
        // different logical thread starts pushing its own calls from the same default position. Give
        // each logical thread its own private buffer instead, identified by state._pad1 (otherwise-
        // unused CPUState padding immediately after callret_sp) doubling as a marker: 0 means this
        // exact snapshot has never been assigned one (true for the very first snapshot any logical
        // thread starts from - captured before any thread/buffer existed - and for a thread that
        // hasn't made its first CALL yet), non-zero is that buffer's base pointer, safe to trust and
        // reuse verbatim since it round-trips with the rest of this logical thread's own snapshot
        // (save_registers/restore_registers memcpy the whole CPUState, _pad1 included). Also keeps
        // Thread->CallRetStackBase in sync - FEXCore's own code-invalidation path
        // (Core.cpp/JIT.cpp's `Allocator::VirtualDontNeed(Thread->CallRetStackBase, ...)`) resets
        // whatever buffer that field currently names, so it must always point at the logical thread
        // that's actually active right now.
        // Allocates this logical thread's private call-ret shadow-stack buffer on first use (state._pad1
        // == 0), recording it in state._pad1 (round-tripped by save/restore). Does NOT touch any
        // InternalThreadState::CallRetStackBase - callers point the right engine's field at the buffer
        // themselves (ensure_callret_stack for the active engine; restore_state_into per restored engine).
        void ensure_callret_buffer(FEXCore::Core::CPUState& state)
        {
            if (state._pad1 == 0)
            {
                // Guard pages on both sides, sized to the real host page (getpagesize(), not the
                // guest's fixed 4KB) so mprotect can't spill onto the guard.
                const size_t host_page = static_cast<size_t>(::getpagesize());
                constexpr size_t callret_stack_size = FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE;
                const size_t callret_alloc_size = callret_stack_size + 2 * host_page;

                // Route the reservation through FEXCore::Allocator::mmap, not raw ::mmap. On Apple
                // (guest VA == host VA) this is the fex_internal_arena hook installed by install(),
                // so the call-ret shadow stack is placed inside the guest-excluded arena instead of
                // at an unconstrained kernel-chosen address. FEXCore's JIT consumes callret_sp as a
                // plain host pointer (REG_CALLRET_SP stp/ldp push/pop in BranchOps.cpp); if that
                // buffer aliased the live guest stack, a host-side callret push/pop would scribble
                // guest memory with no guest instruction involved - the same host/guest aliasing
                // hazard fix #5 solved for FEXCore's other internal buffers, which this embedder-side
                // allocation was overlooked by. On Linux this pointer defaults to a raw ::mmap, so
                // behavior there is unchanged.
                void* alloc_base = FEXCore::Allocator::mmap(nullptr, callret_alloc_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                if (alloc_base == MAP_FAILED)
                {
                    throw std::runtime_error("FEX backend failed to allocate the call-ret stack");
                }

                auto* callret_stack_base = static_cast<uint8_t*>(alloc_base) + host_page;
                if (::mprotect(callret_stack_base, callret_stack_size, PROT_READ | PROT_WRITE) != 0)
                {
                    throw std::runtime_error("FEX backend failed to make the call-ret stack writable");
                }

                state._pad1 = reinterpret_cast<uint64_t>(callret_stack_base);
                // Leave headroom for underflows without hitting the guard page immediately, matching
                // ThreadManager::GetCallRetStackInfo's DefaultLocation (Base + size/4).
                state.callret_sp = reinterpret_cast<uint64_t>(callret_stack_base) + callret_stack_size / 4;

                this->callret_buffers_.emplace_back(alloc_base, callret_alloc_size);
            }
        }

        // Ensures the active engine's call-ret buffer exists and points CallRetStackBase at it. Kept for
        // create_thread/create_thread32 and any path that sets up the currently-active engine.
        void ensure_callret_stack(FEXCore::Core::CPUState& state)
        {
            this->ensure_callret_buffer(state);
            this->active_thread_->CallRetStackBase = reinterpret_cast<void*>(state._pad1);
        }

        // CPUState is owned by the thread frame once a thread exists. Before the thread is created we
        // stage register accesses in a local CPUState so the Windows loader can set up the initial
        // context; create_thread() seeds the real thread from it.
        FEXCore::Core::CPUState& cpu_state()
        {
            if (this->active_thread_ != nullptr)
            {
                return this->active_thread_->CurrentFrame->State; // TODO(fex): confirm field path for the FEX version.
            }
            return this->staged_state_;
        }

        const FEXCore::Core::CPUState& cpu_state() const
        {
            if (this->active_thread_ != nullptr)
            {
                return this->active_thread_->CurrentFrame->State;
            }
            return this->staged_state_;
        }

        uint64_t read_rflags() const
        {
            // FEXCore's ReconstructCompactedEFLAGS requires a live thread (it dereferences Thread to
            // reach CurrentFrame->State); before create_thread(), fall back to the local
            // reimplementation operating on the staged CPUState directly (see reconstruct_compacted_eflags).
            if (this->active_thread_ != nullptr)
            {
                // At rest (not in JIT) WasInJIT=false and the host GPR/PSTATE inputs are unused.
                return this->active_context_->ReconstructCompactedEFLAGS(this->active_thread_, /*WasInJIT=*/false, nullptr, 0);
            }
            return reconstruct_compacted_eflags(this->staged_state_);
        }

        void write_rflags(uint64_t rflags)
        {
            if (this->active_thread_ != nullptr)
            {
                this->active_context_->SetFlagsFromCompactedEFLAGS(this->active_thread_, static_cast<uint32_t>(rflags));
                return;
            }
            set_flags_from_compacted_eflags(this->staged_state_, static_cast<uint32_t>(rflags));
        }

        uint16_t segment_selector(int index) const
        {
            const auto& state = this->cpu_state();
            switch (index)
            {
            case 0:
                return state.es_idx;
            case 1:
                return state.cs_idx;
            case 2:
                return state.ss_idx;
            case 3:
                return state.ds_idx;
            case 4:
                return state.fs_idx;
            case 5:
                return state.gs_idx;
            default:
                return 0;
            }
        }

        void set_segment_selector(int index, const void* value, size_t size)
        {
            uint16_t selector = 0;
            std::memcpy(&selector, value, (std::min)(size, sizeof(selector)));
            auto& state = this->cpu_state();
            switch (index)
            {
            case 0:
                state.es_idx = selector;
                break;
            case 1:
                state.cs_idx = selector;
                break;
            case 2:
                state.ss_idx = selector;
                break;
            case 3:
                state.ds_idx = selector;
                break;
            case 4:
                state.fs_idx = selector;
                break;
            case 5:
                state.gs_idx = selector;
                break;
            default:
                break;
            }
        }

        void mark_executable_range(uint64_t address, size_t size, memory_permission permissions)
        {
            if (this->active_thread_ != nullptr && (permissions & memory_permission::exec) != memory_permission::none)
            {
                this->syscall_handler_->MarkGuestExecutableRange(this->active_thread_, address, size);
            }
        }

        void invalidate_code_range_in(FEXCore::Context::Context* context, FEXCore::Core::InternalThreadState* thread, uint64_t address,
                                      size_t size) const
        {
            if (context == nullptr)
            {
                return;
            }

            // InvalidateCodeBuffersCodeRange/InvalidateThreadCachedCodeRange both require the caller
            // to already hold GetCodeInvalidationMutex() exclusively (see FEXCore's own
            // ThreadManager::InvalidateGuestCodeRange, the canonical caller on Linux) - without it,
            // CompileBlock's shared lock deadlocks permanently the first time this runs.
            std::unique_lock lock(context->GetCodeInvalidationMutex());

#ifdef __APPLE__
            // Invalidating a range can synchronously delink already-linked call sites (see
            // AddBlockLink's delinker callbacks in JIT.cpp), writing directly into a MAP_JIT code
            // buffer - same per-thread JIT-write-protect requirement as
            // exit_function_link_jit_write_wrapper, but for a call site we make ourselves rather than
            // one JIT-compiled code makes through a function-pointer slot.
            ::pthread_jit_write_protect_np(0);
#endif
            context->InvalidateCodeBuffersCodeRange(address, size);
            if (thread != nullptr)
            {
                context->InvalidateThreadCachedCodeRange(thread, address, size);
            }
#ifdef __APPLE__
            ::pthread_jit_write_protect_np(1);
#endif
        }

        // include_inactive_contexts additionally drops the range from the *other* (currently-inactive)
        // FEXCore context's translation cache. This is needed only when guest code is actually removed
        // from an address (an unmap), not on ordinary protection changes: see the WoW64 note below.
        void invalidate_code_range(uint64_t address, size_t size, bool include_inactive_contexts = false) const
        {
            if (!this->active_context_)
            {
                return;
            }

            // Invalidate the currently-active context exactly as before.
            this->invalidate_code_range_in(this->active_context_, this->active_thread_, address, size);

            // A WoW64 process runs two independent FEXCore contexts - context_ (64-bit) and context32_
            // (32-bit) - each with its own translation cache and code buffers. An unmap of 32-bit guest
            // code is serviced while active_context_ is the 64-bit context (a 32-bit guest syscall
            // crosses through the heaven's gate to 64-bit mode before reaching sogen's syscall handler),
            // so invalidating only active_context_ leaves stale *32-bit* translations behind. That is
            // exactly what let a block compiled from clbcatq.dll (mapped at guest base 0xa220000, image
            // 0x86000) survive after clbcatq was unmapped and mmdevapi.dll (a smaller 0x75000 image) was
            // mapped at the SAME base: the guest later reached that guest RIP, FEX ran the phantom
            // clbcatq translation instead of recompiling mmdevapi's current bytes, and it stored to a
            // clbcatq global past mmdevapi's image end (0xa29ae80) - now unmapped - faulting with
            // C0000005. Only unmaps request this - doing it on every protection change would repeatedly
            // delink the live 32-bit context's blocks from the inactive side and livelock it.
            if (include_inactive_contexts && this->context32_.get() != nullptr && this->context32_.get() != this->active_context_ &&
                this->thread32_ != nullptr)
            {
                this->invalidate_code_range_in(this->context32_.get(), this->thread32_, address, size);
            }
        }

        void request_thread_stop()
        {
            // Forces the in-flight ExecuteThread to return, whether called from the same thread
            // (synchronously, e.g. from within a syscall hook) or a different one (e.g. a quantum
            // timer thread). FEXCore's JIT emits a `str zr, [InterruptFaultPage]` at every translated
            // block's entry when Config.NeedsPendingInterruptFaultCheck is set (see
            // initialize_context's CONFIG_GDBSERVER comment) - protecting that page makes the next
            // block entry fault, landing in handle_fault_signal, which sees stop_requested_ and
            // redirects into FEXCore's own ThreadStopHandlerAddress instead of resuming.
            if (this->active_thread_ == nullptr)
            {
                return;
            }

            ::mprotect(this->active_thread_->InterruptFaultPage, sizeof(this->active_thread_->InterruptFaultPage), PROT_NONE);
        }

        emulator_hook* make_hook()
        {
            return reinterpret_cast<emulator_hook*>(this->next_hook_id_++);
        }

        // --[ state ]--------------------------------------------------------------------------------

        // The always-64-bit FEXCore::Context - see notify_process_bitness's doc comment. Everything
        // in this class outside of ensure_context32()/the destructor references this pair; context32_/
        // thread32_ below are not yet wired into execution (Phase B, in progress - see the WoW64 plan).
        fextl::unique_ptr<FEXCore::Context::Context> context_{};
        FEXCore::Core::InternalThreadState* thread_ = nullptr;

        // Whichever context/thread is *currently executing* - starts out equal to context_/thread_
        // (the only thing that ever runs, until Phase B's gate-crossing creates thread32_ and starts
        // flipping this) and is what every JIT-operation call site below actually uses. context_/
        // thread_ and context32_/thread32_ (declared further below) are the two fixed, named
        // instances; active_context_/active_thread_ is which *one* of them is live right now.
        FEXCore::Context::Context* active_context_ = nullptr;
        FEXCore::Core::InternalThreadState* active_thread_ = nullptr;
        // Set once via notify_process_bitness(), before any thread is created (see that override's
        // doc comment) - gates every guest-memory-touching method's wow64_guest_rebase application
        // below (needed for the 32-bit executable/ntdll32 modules regardless of which FEXCore::Context
        // is currently executing) and whether ensure_context32() builds context32_ at all. No longer
        // selects context_'s own bitness - context_ is always the 64-bit Context now.
        bool is_wow64_process_ = false;
        // The actual host address offset added to sub-4GB guest addresses (see rebase_for). Starts
        // at wow64_guest_rebase_default and, on Apple, is overwritten by reserve_wow64_host_window()
        // once it finds a genuinely free candidate window - see that method's doc comment. Stays at
        // the default on every other platform (this backend is shared, not Apple-exclusive), which
        // is also FEXCore::Context::Config.Wow64GuestRebaseValue's own default, so both sides agree
        // without sogen ever needing to call SetWow64GuestRebaseValue there at all.
        uint64_t wow64_guest_rebase_ = wow64_guest_rebase_default;
#ifdef __APPLE__
        // Set by reserve_wow64_host_window() iff it actually claimed [wow64_guest_rebase_,
        // wow64_guest_rebase_ + wow64_guest_address_space_size) up front - see its doc comment. Gates
        // the skip checks in reserved_host_ranges()/reserved_host_ranges_in() below; if the
        // reservation attempt was skipped or failed (logged loudly either way), this stays false and
        // both functions behave exactly as before - detect-and-report, not silently assume-safe.
        // Apple-only: reserve_wow64_host_window() and its two call sites below are both
        // __APPLE__-gated (the whole mechanism is a macOS/mach_vm_region-specific fix), so this
        // member is unused - and -Werror,-Wunused-private-field - on other platforms without this.
        bool wow64_host_window_reserved_ = false;
#endif
        // wow64cpu.dll's real TurboDispatchJumpAddressEnd export address, set via
        // set_wow64_turbo_dispatch_end once module_manager resolves it - see that method's doc
        // comment for why this can't just be a fixed offset from the image base. Stays 0 until
        // then; enter_wow64_64bit_from_wow64svc_thunk falls back to the old (best-effort) fixed-
        // offset computation if it's never been set, rather than crashing outright.
        uint64_t wow64_turbo_dispatch_end_ = 0;

        void set_wow64_turbo_dispatch_end(pointer_type address) override
        {
            this->wow64_turbo_dispatch_end_ = address;
        }

        std::unique_ptr<fex_syscall_handler> syscall_handler_{};
        // Placeholder: FEXCore::SignalDelegator has no pure virtuals, so InitCore() can be satisfied
        // with the plain base class for now. This does no actual fault handling - a real Mach-
        // exception-based subclass replaces it once cooperative stop()/memory-violation delivery are
        // implemented.
        std::unique_ptr<FEXCore::SignalDelegator> signal_delegator_{};
        FEXCore::Core::CPUState staged_state_{};

        // The second, 32-bit FEXCore::Context a wow64 process needs (see ensure_context32's doc
        // comment). thread32_ is never created yet - Phase B (the heaven's-gate crossing that will
        // actually create it, marshal state into it, and switch execution to it) is not wired up.
        fextl::unique_ptr<FEXCore::Context::Context> context32_{};
        FEXCore::Core::InternalThreadState* thread32_ = nullptr;
        std::unique_ptr<fex_syscall_handler> syscall_handler32_{};
        std::unique_ptr<FEXCore::SignalDelegator> signal_delegator32_{};

        uint64_t gdt_base_ = 0;
        uint32_t gdt_limit_ = 0;

        std::atomic<bool> stop_requested_{false};
        uintptr_t next_hook_id_ = 1;

#ifdef __APPLE__
        // See pending_fault_kind's doc comment (declared earlier in this class, near
        // defer_hook_dispatch/dispatch_pending_hook_if_any).
        pending_fault_dispatch pending_fault_dispatch_{};
#endif

        std::map<uint64_t, mapped_region> regions_;
        std::vector<mmio_region> mmio_regions_;

        // Every per-logical-thread call-ret buffer ever allocated by ensure_callret_buffer, so the
        // destructor can release them - these live outside regions_ (host allocator space, not the
        // guest address space) and outlive any individual CPUState snapshot they were allocated for.
        std::vector<std::pair<void*, size_t>> callret_buffers_;

        // See x86_emulator::mark_guest_range_permanently_non_executable's doc comment - consulted by
        // fex_syscall_handler::QueryGuestExecutableRange, which is what actually makes FEXCore treat
        // these ranges as non-executable regardless of their real page permissions.
        std::vector<std::pair<uint64_t, size_t>> permanently_non_executable_ranges_;

        // See x86_emulator::register_gate_crossing / the gate_crossing struct (declared above, near
        // find_gate_crossing). Each entry is a WoW64 mode-switch point; reaching one (recognized in
        // handle_fault_signal by matching the faulting RIP) marshals CPU state between context_/
        // context32_ and flips active_context_/active_thread_ instead of raising a memory violation.
        // Consulted by QueryGuestExecutableRange (so the range is non-executable to the JIT, forcing
        // the synthetic #PF) and by perform_gate_crossing.
        std::vector<gate_crossing> gate_crossings_;

#ifdef __APPLE__
        // Apple Silicon's host page (16KB, see host_page_size_apple) is coarser than the guest's
        // architectural page (4KB, `page_size` above), so a single host mprotect/mmap can't always
        // express what the guest requested independently per 4KB page - e.g. a PE image's .text
        // (RX) directly followed by .data (RW) land in the same host page. page_shadow_apple_ is
        // the source of truth per guest 4KB page (absent = never requested/unmapped, which must
        // still fault like reserved-but-uncommitted memory); mapped_host_pages_apple_ tracks which
        // 16KB-aligned host pages currently have a live mmap, so sync_host_page_apple can tell a
        // first-time mmap from a protection change on an existing one.
        std::map<uint64_t, memory_permission> page_shadow_apple_;
        std::set<uint64_t> mapped_host_pages_apple_;
#endif

        hook_entry* syscall_hook_ = nullptr;
        std::unordered_map<emulator_hook*, hook_entry> instruction_hooks_;
        std::unordered_map<emulator_hook*, interrupt_hook_callback> interrupt_hooks_;
        std::unordered_map<emulator_hook*, memory_access_hook_callback> memory_read_hooks_;
        std::unordered_map<emulator_hook*, memory_access_hook_callback> memory_write_hooks_;
        std::unordered_map<emulator_hook*, memory_execution_hook_callback> memory_execution_hooks_;
        std::unordered_map<emulator_hook*, memory_violation_hook_callback> memory_violation_hooks_;
        std::unordered_map<emulator_hook*, basic_block_hook_callback> basic_block_hooks_;
    };

#ifdef __APPLE__
    namespace
    {
        void fault_signal_handler(int sig, siginfo_t* info, void* raw_ucontext)
        {
            auto* uctx = static_cast<ucontext_t*>(raw_ucontext);

            const bool handled = g_active_emulator != nullptr && g_active_emulator->handle_fault_signal(sig, info, raw_ucontext);
            if (handled)
            {
                return;
            }

            // See jit_write_protect_retry_count_for's doc comment: fprintf/stdio is not async-signal-
            // safe. snprintf into a fixed stack buffer + a single write(2) is the standard pragmatic
            // idiom for signal-handler-safe formatted output.
            char buf[160];
            const uint64_t pc = arm_thread_state64_get_pc(uctx->uc_mcontext->__ss);
            const int len = snprintf(buf, sizeof(buf), "[FEX backend] unhandled signal %d si_code=%d at pc=0x%llx fault_addr=%p\n", sig,
                                     info->si_code, static_cast<unsigned long long>(pc), info->si_addr);
            if (len > 0)
            {
                const auto write_len = static_cast<size_t>(len) < sizeof(buf) ? static_cast<size_t>(len) : sizeof(buf);
                ::write(STDERR_FILENO, buf, write_len);
            }

            struct sigaction default_action = {};
            default_action.sa_handler = SIG_DFL;
            ::sigaction(sig, &default_action, nullptr);
            ::raise(sig);
        }
    } // namespace
#endif

    // -----------------------------------------------------------------------------------------------
    // fex_syscall_handler method bodies (fex_x86_64_emulator is now complete).
    // -----------------------------------------------------------------------------------------------

    uint64_t fex_syscall_handler::HandleSyscall(FEXCore::Core::CpuStateFrame* /*frame*/, FEXCore::HLE::SyscallArguments* /*args*/)
    {
        // SyscallOp (deps/FEX OpcodeDispatcher.cpp) stores CPUState.rip = the address of the `syscall`
        // instruction ITSELF (GetRelocatedPC(Op, -Op->InstSize)) before invoking us, so during the hook the
        // guest rip points AT the syscall - exactly the convention sogen's shared syscall layer expects (see
        // syscall_utils.hpp write_syscall_result). That layer leaves CPUState.rip so that advancing it by the
        // 2-byte syscall length yields the intended next rip, in EVERY case: a non-redirecting syscall leaves
        // rip AT the syscall (advance -> the following instruction), while a redirecting syscall (NtContinue,
        // exception/APC returns, retriggers) sets rip = (target - 2) precisely so this advance lands on the
        // real target. On non-_WIN32 hosts `syscall` is not FLAGS_BLOCK_END (X86Tables.h) so the JIT would
        // otherwise fall through and either re-execute the syscall on a block re-entry or, for a redirect,
        // branch to (target - 2) - the mid-instruction landing that produced the RtlUserThreadStart-2 `int
        // 0xAC` fault storm during loader init. Mirror the other backends (Unicorn's skip_instruction skips
        // exactly the syscall's length unconditionally): always advance rip past the 2-byte syscall. The
        // SyscallOp block-split's CondJump then compares this against the fallthrough to pick continue-in-line
        // vs ExitFunction(redirect target), both now landing on a real instruction boundary.
        auto* hook = this->emulator_.syscall_hook_;
        if (hook != nullptr && hook->callback)
        {
            // The Windows syscall layer reads/writes guest registers itself through the emulator, so the
            // hook needs no data argument here. It places the NT status in RAX before returning.
            hook->callback(this->emulator_, 0);
        }

        this->emulator_.cpu_state().rip += 2;

        if (this->emulator_.stop_requested_)
        {
            this->emulator_.request_thread_stop();
        }

        // FEX writes our return value into guest RAX; hand back whatever the hook already set so the
        // value is preserved.
        return this->emulator_.cpu_state().gregs[detail::greg_rax];
    }

    FEXCore::HLE::ExecutableRangeInfo fex_syscall_handler::QueryGuestExecutableRange(FEXCore::Core::InternalThreadState* /*thread*/,
                                                                                     uint64_t address)
    {
        // FEXCore checks this before compiling/executing a guest address (see the decoder's use of
        // QueryGuestExecutableRange) and synthesizes a #PF (IR "Break" op, TrapNo=X86_TRAPNO_PF) if the
        // address isn't reported as executable here - so returning the default-constructed {} for
        // every address (as this stub previously did unconditionally) made every guest instruction
        // fetch look like a DEP violation to FEXCore.
        // See x86_emulator::mark_guest_range_permanently_non_executable's doc comment - checked
        // first, ahead of the region's own real permissions, so this always wins regardless of how
        // the range is actually mapped (sogen's own WoW64 heaven's-gate trampoline is deliberately
        // mapped read|exec for KVM/Unicorn's benefit, but must never look executable here).
        for (const auto& [non_exec_address, non_exec_size] : this->emulator_.permanently_non_executable_ranges_)
        {
            if (address >= non_exec_address && address < non_exec_address + non_exec_size)
            {
                return {};
            }
        }

        // A registered WoW64 gate crossing is likewise non-executable to the JIT - reaching it must
        // raise a synthetic #PF (which perform_gate_crossing then services) rather than compile the
        // real mode-switch bytes there, which the fixed-bitness JIT can't execute anyway.
        for (const auto& gate : this->emulator_.gate_crossings_)
        {
            if (address >= gate.address && address < gate.address + gate.size)
            {
                return {};
            }
        }

        auto& regions = this->emulator_.regions_;
        auto it = regions.upper_bound(address);
        if (it == regions.begin())
        {
            return {};
        }
        --it;
        const uint64_t region_end = it->first + it->second.size;
        if (address < it->first || address >= region_end)
        {
            return {};
        }

        const auto perms = it->second.permissions;
        if ((perms & memory_permission::exec) == memory_permission::none)
        {
            return {};
        }

        // FEXCore caches the returned [Base, Base+Size) span and skips re-querying any address inside
        // it (Frontend.cpp CheckRangeExecutable), so a gate crossing that lives *inside* an otherwise-
        // executable region (e.g. RunSimulatedCode inside wow64cpu.dll's .text, unlike the heaven's-
        // gate trampoline which is its own standalone mapping) would never be consulted: the region
        // query at some earlier address caches the whole span as executable, spanning right across the
        // gate. Clamp the reported span so it stops at the nearest gate boundary on either side of the
        // queried address - the gate itself then falls outside the cache, forcing a fresh query (which
        // returns {} above) the moment execution reaches it.
        uint64_t base = it->first;
        uint64_t end = region_end;
        for (const auto& gate : this->emulator_.gate_crossings_)
        {
            const uint64_t gate_end = gate.address + gate.size;
            if (gate_end <= address)
            {
                base = (std::max)(base, gate_end);
            }
            else if (gate.address > address)
            {
                end = (std::min)(end, gate.address);
            }
        }

        return {
            .Base = base,
            .Size = end - base,
            .Writable = (perms & memory_permission::write) != memory_permission::none,
        };
    }

    std::optional<FEXCore::ExecutableFileSectionInfo> fex_syscall_handler::LookupExecutableFileSection(
        FEXCore::Core::InternalThreadState* /*thread*/, uint64_t /*guest_addr*/)
    {
        // We do not back guest code by host file sections, so there is nothing to look up.
        return std::nullopt;
    }

    std::unique_ptr<x86_64_emulator> create_x86_64_emulator()
    {
        return std::make_unique<fex_x86_64_emulator>();
    }
} // namespace sogen::fex
