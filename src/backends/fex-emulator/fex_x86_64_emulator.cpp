// _GNU_SOURCE is required for mremap() (used to alias host memory into the guest address space).
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#define FEX_EMULATOR_IMPL
#include "fex_x86_64_emulator.hpp"
#include "fex_x86_64_common.hpp"

// FEX (https://fex-emu.com) is an in-process x86-64 -> AArch64 binary translator. Unlike the
// Unicorn/Icicle/KVM backends it does not manage a sandboxed guest address space: the translated
// guest executes inside the host process with guest VA == host VA. So map_memory() is a real
// mmap(MAP_FIXED) at the guest address and read/write_memory() a direct host memcpy, and - as with
// KVM - there is no per-access or per-instruction instrumentation point, so memory/execution/block
// hooks are accepted for API compatibility but never fire. Guest `syscall` instructions come back
// through a FEXCore::HLE::SyscallHandler that invokes the registered syscall instruction hook.
//
// The functional target is Darwin on Apple Silicon; the signal handlers, MMIO fault emulation and
// 16KB/4KB page reconciliation below are all Darwin-only. The AArch64 Linux path only has build
// coverage, and Android is excluded by the CMake gate.

#include <cstdlib>
#include <sys/mman.h>
#include <unistd.h>

// Used unconditionally by the std::terminate handler in initialize_context(). Both platforms
// SOGEN_ENABLE_FEX gates this backend to provide it, so the include need not be Apple-only.
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

#ifdef __APPLE__
        // FEXCore's JITWriteScope (FEXCore/Utils/AllocatorHooks.h) is a plain ctor/dtor toggle around
        // pthread_jit_write_protect_np, not a nesting counter. Arm64JITCore::ExitFunctionLink and its
        // delinker helpers patch already-compiled call sites without a scope of their own (upstream FEX
        // targets Linux, which has no per-thread MAP_JIT W^X). Bracketing our own call to
        // Pointers.ExitFunctionLink is not enough: its "not yet compiled" path calls CompileBlock, whose
        // nested JITWriteScope re-enables write protection before control returns to the still-writing
        // outer call, faulting on the self-modifying store. Interposing pthread_jit_write_protect_np to
        // make it reentrant does not work either - both ways of reaching the real implementation from
        // inside the interposer (dlsym(RTLD_NEXT, ...) and dlopen(RTLD_NOLOAD)+dlsym-by-handle) resolve
        // back to the replacement.
        //
        // So the fault is reacted to instead: see handle_fault_signal's SEGV_ACCERR branch, which
        // disables write protection and retries when the faulting PC lies in FEXCore's own code rather
        // than in MAP_JIT memory. Retries are bounded per fault address so a different bug cannot spin.
        //
        // The bound is tracked in a fixed array rather than an unordered_map because it is updated from
        // a real hardware signal handler that can interrupt an unrelated malloc()/free(); operator[] can
        // insert or rehash and is not async-signal-safe. Eight slots suffice: guest execution is
        // cooperative, so only one address is ever mid-retry, and a healthy call site resolves in <= 1
        // retry, so eviction cannot take budget from an address that is.
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

        // A gap at least this long since an address last faulted means the site is being reused
        // healthily rather than spinning, so its retry budget is reset.
        constexpr uint64_t jit_write_protect_retry_reset_window_ns = 100'000'000; // 100ms

        uint64_t monotonic_now_ns()
        {
            struct timespec ts{};
            ::clock_gettime(CLOCK_MONOTONIC, &ts);
            return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL + static_cast<uint64_t>(ts.tv_nsec);
        }

        // Resets the counter when the address has not faulted within the window: otherwise a site that
        // legitimately resolves this race many times over a long run would exhaust its budget and turn a
        // benign race into a hard crash. Async-signal-safe: fixed-array scan, no allocation, and
        // clock_gettime(CLOCK_MONOTONIC) is vDSO-backed.
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

        // Decodes only what an mmio_region fault needs: destination register, transfer size and
        // extension. The addressing mode is deliberately not decoded - the effective address is already
        // known, being the fault address itself under guest VA == host VA - so only the fields the
        // "Load/store register" class keeps at fixed bit positions across every sub-form are read
        // (size/opc at [31:30]/[23:22], Rt at [4:0]; AArch64 ISA C4.1.3). Stores are not decoded: this
        // backend's only MMIO consumer is read-only.
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

        // Store-release forms (STLR/STLRB/STLRH), the counterpart to decode_arm64_load's LDAR/LDAPR
        // handling. Used only by handle_fault_signal's misaligned-atomic fallback, never for
        // mmio_region, whose only consumer here is read-only.
        //
        // Deliberately narrow: broadening this table to plain STR/STUR for
        // handle_general_memory_violation's Category-3 false-fault case causes a reproducible hang
        // isolated to this table (decode_arm64_load's equivalent plain-load coverage is safe), with a
        // root cause that is not yet understood.
        //
        // Known consequence, tracked separately: handle_general_memory_violation also uses this decoder
        // to classify a fault as read vs. write on the genuine protection-violation path, so a plain STR
        // to read-only memory is misclassified as a read, routed into false-fault recovery, and surfaces
        // as an unhandled host signal instead of a guest STATUS_ACCESS_VIOLATION - which any guest that
        // writes to read-only memory and catches the exception (packers, DRM) will hit. A plain store to
        // unmapped memory does reach the guest, but with ExceptionInformation[0] wrongly saying read.
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
        // Apple Silicon's kernel refuses simultaneous write+exec on any non-MAP_JIT mapping (mprotect
        // fails with EACCES), unlike Linux where guest W^X is advisory. Real PE loaders hit this
        // routinely: map .text RWX to apply relocations, then narrow to RX before executing. Favoring
        // write handles that sequence; it would be wrong for a page genuinely written and executed in
        // the same window without an intervening apply_memory_protection call, which PE loading is not.
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

        // The guest runs natively with guest VA == host VA, so there is no per-instruction hook to
        // intercept a specific address. The only MMIO consumer here is KUSER_SHARED_DATA at 0x7ffe0000,
        // which falls inside Darwin's mandatory __PAGEZERO segment and cannot be backed by real memory
        // (confirmed via both mmap(MAP_FIXED) and mach_vm_allocate), so the region is left unmapped and
        // every access faults into handle_mmio_fault. Read-only, matching real Windows: a guest write is
        // left to surface as a normal access violation. Not a general MMIO implementation.
        struct mmio_region
        {
            uint64_t address = 0;
            size_t size = 0;
            mmio_read_callback read_cb;
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

        // Replaces FEXCore's own FetchHostFeatures(), which is Linux-only: it reads MIDR_EL1, neither
        // EL0-readable nor trap-emulated on Darwin. Anything not confirmed present in
        // hw.optional.arm.*, or without a clear ARM-feature mapping, is left false - that costs codegen
        // quality, never correctness.
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

            // TPIDRRO_EL0 is not confirmed to carry a CPU index on Darwin, and DEF_OP(ProcessorID)
            // treats the non-TPIDRRO fallback as unsupported (matching the Windows/wine precedent), so
            // leaving this false makes a guest RDTSCP/RDPID hard-error rather than read garbage.
            features.SupportsCPUIndexInTPIDRRO = false;

            // FEXCore's CPUID brand-string leaves (0x80000002-4) index PerCPUData, sized from CPUMIDRs;
            // an empty CPUMIDRs null-derefs the leaf's ProductName. Linux populates it from per-core
            // MIDR_EL1, unavailable from EL0 on Darwin, so the host logical-CPU count is reported with a
            // placeholder MIDR of 0: M-series parts are absent from FEXCore's MIDR table anyway, so it
            // resolves to FEXCore's own "Unknown ARM CPU" fallback while keeping the guest-visible core
            // count realistic.
            uint32_t logical_cpus = 1;
            size_t logical_cpus_len = sizeof(logical_cpus);
            if (::sysctlbyname("hw.logicalcpu", &logical_cpus, &logical_cpus_len, nullptr, 0) != 0 || logical_cpus == 0)
            {
                logical_cpus = 1;
            }
            features.CPUMIDRs.assign(logical_cpus, 0u);

            return features;
        }

        // sogen runs one FEX-backed guest thread per process, so a single active-instance pointer is
        // enough for the signal handler below to reach the emulator's hook tables and thread state.
        // Signal handlers cannot be non-static member functions, hence the indirection.
        fex_x86_64_emulator* g_active_emulator = nullptr;

        void fault_signal_handler(int sig, siginfo_t* info, void* raw_ucontext);

        void install_fault_signal_handlers(fex_x86_64_emulator& emulator)
        {
            // Fault routing is process-global (one sigaction handler set, one active-instance
            // pointer). A second live instance would silently receive the first one's faults, so
            // refuse it outright - reachable e.g. via the Python bindings constructing two emulators.
            if (g_active_emulator != nullptr)
            {
                throw std::runtime_error("Only one FEX emulator instance can be active per process");
            }

            g_active_emulator = &emulator;

            // A dedicated alternate stack, so a second signal arriving while this handler is already
            // running does not nest on the faulting thread's potentially near-exhausted stack.
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

            // FEXCore's IR "Break" op models x86 HLT/UD2/INT3/INT1/INTO uniformly via distinct native
            // traps chosen per Dispatcher.cpp's GuestSignal_SIG* stubs: HLT/UDF raise SIGILL, BRK raises
            // SIGTRAP. INT3 (DebugBreak()) goes through the SIGTRAP stub, so without a handler here that
            // BRK is an unhandled hardware trap terminating the process, instead of reaching the vector
            // dispatch below, which handles vector 3 correctly.
            ::sigaction(SIGTRAP, &action, nullptr);
        }

        // Arm64JITCore::ExitFunctionLink (JIT.cpp) patches an already-compiled call site once its target
        // block is known, writing straight into a MAP_JIT code buffer without a JITWriteScope, because
        // it is written for Linux, which has no per-thread W^X state. Pointers.ExitFunctionLink is a
        // plain function-pointer slot JIT code calls through (JIT.cpp's InitThreadPointers), so it can
        // be wrapped here instead of patching deps/FEX.
        uint64_t g_original_exit_function_link = 0;

        uint64_t exit_function_link_jit_write_wrapper(FEXCore::Core::CpuStateFrame* frame, void* record)
        {
            using exit_function_link_fn = uint64_t (*)(FEXCore::Core::CpuStateFrame*, void*);
            const auto real = reinterpret_cast<exit_function_link_fn>(g_original_exit_function_link);

            // pthread_jit_write_protect_np is per-thread and exclusive with execute permission on this
            // thread's MAP_JIT pages: leaving it disabled past this call would fault the next guest
            // instruction fetch on this thread, not just widen an otherwise-harmless window.
            ::pthread_jit_write_protect_np(0);
            const uint64_t result = real(frame, record);
            ::pthread_jit_write_protect_np(1);
            return result;
        }

        // Under guest VA == host VA, any host allocation the kernel places freely can land inside the
        // guest's own address space. FEXCore obtains all of its internal buffers - the BlockLinks
        // red-black-tree storage, the JIT CodeBuffer, the dispatcher - through
        // FEXCore::Allocator::mmap(nullptr, ...), a raw ::mmap(NULL, ...) with no coordination with
        // sogen's bookkeeping, so an ordinary guest store into a buffer placed at a guest-owned address
        // silently corrupts FEXCore's own state (the long-standing __tree_balance_after_insert /
        // AddBlockLink corruption).
        //
        // So one large host arena is reserved up-front and registered as a reserved_host_range, giving
        // a two-way exclusion, and every FEXCore::Allocator::mmap(nullptr, ...) is satisfied from inside
        // it. Non-executable requests are placed with MAP_FIXED. Executable (MAP_JIT) ones cannot be -
        // Apple rejects MAP_JIT|MAP_FIXED with EINVAL - so the sub-region is unmapped first and MAP_JIT
        // gets the hole's address as a non-fixed hint, which the kernel honors reliably since nothing
        // else competes for space inside the arena. The result is verified to land inside the arena and
        // fails loudly rather than silently falling back to an unconstrained mapping that would
        // reintroduce the aliasing hazard.
        class fex_internal_arena
        {
          public:
            // Pure VA reservation (PROT_NONE) until sub-regions are committed, sized to exceed any
            // realistic FEXCore-internal need for a full application workload.
            static constexpr size_t arena_size = 0x1'0000'0000ULL; // 4 GiB

            static fex_internal_arena& instance()
            {
                static fex_internal_arena arena;
                return arena;
            }

            // Must run before the first FEXCore-internal allocation (context/CodeBuffer creation) and
            // before the memory manager first queries reserved_host_ranges(). Idempotent.
            void install()
            {
                if (this->base_ != 0)
                {
                    return;
                }

                void* base = ::mmap(nullptr, arena_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
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

            // Returns 0 on exhaustion. Caller holds lock_.
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
                    // FEXCore's CodeBuffer sizes its request to include a trailing guard page
                    // (CPUBackend.h's UsableSize(): AllocatedSize - FEX_HOST_PAGE_SIZE) and
                    // mprotect(PROT_NONE)s that page itself. That always fails on Apple Silicon with
                    // EACCES - MAP_JIT protection is fixed at mmap() time and cannot be adjusted
                    // afterwards - which is the "Failed to mprotect last page of code buffer" diagnostic
                    // CPUBackend.cpp logs. A real guard is provided instead: only the leading portion
                    // becomes the executable MAP_JIT mapping and the trailing page stays part of the
                    // arena's permanent PROT_NONE reservation, faulting on any access exactly where
                    // UsableSize() expects the buffer to end. Skipped at or below one host page so a
                    // small allocation (the Dispatcher's guardless buffer) is not shrunk into uselessness.
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

    // Bridges FEX's guest `syscall` exits to the registered instruction hook. Method bodies are out of
    // line, after fex_x86_64_emulator is complete, since they touch its internals.
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

            // Release everything we mmap'd into the (host == guest) address space.
#ifdef __APPLE__
            // All host mappings this backend owns are tracked as 16KB host pages (see
            // mapped_host_pages_apple_), including PROT_NONE reservation claims that no regions_
            // entry covers; map_host_memory aliases (owned=false) are never in this set. Darwin's
            // munmap also requires host-page alignment, which regions_ entries don't guarantee.
            for (const auto host_page : this->mapped_host_pages_apple_)
            {
                ::munmap(reinterpret_cast<void*>(host_page), host_page_size_apple);
            }

            if (g_active_emulator == this)
            {
                g_active_emulator = nullptr;
            }
#else
            for (const auto& [address, region] : this->regions_)
            {
                if (region.owned)
                {
                    ::munmap(reinterpret_cast<void*>(address), region.size);
                }
            }
#endif

            // Per-logical-thread call-ret buffers (see ensure_callret_buffer) live in host allocator
            // space, not regions_, and are never freed as individual guest threads exit - release them
            // all here so they don't outlive this emulator instance.
            for (const auto& [alloc_base, alloc_size] : this->callret_buffers_)
            {
                FEXCore::Allocator::munmap(alloc_base, alloc_size);
            }
        }

        // cpu_interface

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
            if (count != 0)
            {
                // FEX has CompileRIPCount() for bounded execution, but wiring exact instruction counts
                // through the JIT exit path is non-trivial; match the KVM backend and refuse for now.
                throw std::runtime_error("FEX backend does not support exact instruction counts yet");
            }

            if (this->thread_ == nullptr)
            {
                this->create_thread();
            }

            this->stop_requested_ = false;
            // Re-arm InterruptFaultPage for this quantum - see request_thread_stop's doc comment; a
            // prior stop() may have left it protected to force the last quantum's ExecuteThread to
            // return, and it must be writable again before the JIT's per-block-entry store runs.
            ::mprotect(this->thread_->InterruptFaultPage, sizeof(this->thread_->InterruptFaultPage), PROT_READ | PROT_WRITE);

            // ExecuteThread runs the translated guest until the thread is asked to stop (which the
            // syscall bridge does when a hook calls stop()), or the guest faults/exits.
#ifdef __APPLE__
            // Here it can also return early because handle_fault_signal deferred a hook dispatch (see
            // pending_fault_dispatch_) rather than genuinely stopping - dispatch it in normal call
            // context, where that is safe, then resume by calling ExecuteThread again; it always
            // restarts from CurrentFrame->State.rip, which the hook is free to have redirected.
            for (;;)
            {
                this->context_->ExecuteThread(this->thread_);

                const bool hook_dispatched = this->dispatch_pending_hook_if_any();
                const bool interrupt_page_unwind = this->interrupt_page_unwind_.exchange(false);

                // An InterruptFaultPage unwind with no stop pending is the quantum timer racing this
                // quantum's own entry: stop() sets stop_requested_ then protects the page, but a
                // concurrently-entered start() has already cleared the flag and the timer's mprotect
                // only lands after the re-arm above, so the first block-entry check faults with nothing
                // requested. Treating that as a real stop makes windows_emulator::vcpu_worker read it as
                // a fatal wind-down, so re-arm and resume instead; the timer's pending switch_thread
                // request is honored at the next genuine stop.
                if (this->stop_requested_ || (!hook_dispatched && !interrupt_page_unwind))
                {
                    break;
                }

                if (interrupt_page_unwind)
                {
                    ::mprotect(this->thread_->InterruptFaultPage, sizeof(this->thread_->InterruptFaultPage), PROT_READ | PROT_WRITE);
                }
            }
#else
            this->context_->ExecuteThread(this->thread_);
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
                uint64_t raw = state.gregs[mapping.gpr.index] >> (mapping.gpr.byte_offset * 8);
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

        std::vector<std::byte> save_registers() const override
        {
            // The whole architectural state lives in a single CPUState struct; snapshot it verbatim.
            const auto& state = this->cpu_state();
            std::vector<std::byte> data(sizeof(FEXCore::Core::CPUState));
            std::memcpy(data.data(), &state, sizeof(state));
            return data;
        }

        // Preserves the fields that are per-FEXCore-thread-global rather than per-logical-guest-thread:
        // FEXCore rewrites L1Pointer/L1Mask itself whenever the JIT lookup cache reallocates, so the
        // live values are always the correct ones and a stale snapshot must not clobber them.
        // callret_sp/_pad1 are handled by ensure_callret_buffer.
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
            if (register_data.size() != sizeof(FEXCore::Core::CPUState))
            {
                throw std::runtime_error("FEX register snapshot has unexpected size");
            }

            if (this->thread_ == nullptr)
            {
                // No thread yet: writing into staged_state_, which create_thread() will seed the
                // real thread from (including installing L1Pointer/L1Mask/callret_sp correctly
                // itself afterward) - a verbatim copy here is fine.
                std::memcpy(&this->staged_state_, register_data.data(), sizeof(FEXCore::Core::CPUState));
                return;
            }

            this->restore_state_into(this->thread_, register_data.data());
        }

        bool has_violation() const override
        {
            return false;
        }

        bool supports_instruction_counting() const override
        {
            return false;
        }

        // request_thread_stop() mprotects InterruptFaultPage to PROT_NONE, which is safe to call from
        // any host thread - the software-quantum watchdog thread in windows_emulator::start() relies on
        // exactly this (supports_instruction_counting() is false, so that path is the only time-slicing
        // mechanism available). Matches KVM's reasoning for the same accessor.
        bool is_stop_thread_safe() const override
        {
            return true;
        }

        // emulator

        std::string get_name() const override
        {
            return "FEX";
        }

        bool supports_multiple_vcpus() const override
        {
            // sogen multiplexes logical guest threads onto a single FEXCore engine, cooperatively
            // scheduled on one host thread - no multi-vCPU support.
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

        // x86_emulator

        void set_segment_base(x86_register base, pointer_type value) override
        {
            auto& state = this->cpu_state();
            if (base == x86_register::fs || base == x86_register::fs_base)
            {
                state.fs_cached = value;
            }
            else if (base == x86_register::gs || base == x86_register::gs_base)
            {
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

        void notify_process_bitness(bool is_wow64_process) override
        {
            if (is_wow64_process)
            {
                throw std::runtime_error("FEX backend does not support WoW64 (32-bit) processes yet");
            }
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
            this->cpu_state().segment_arrays[0] = reinterpret_cast<FEXCore::Core::CPUState::gdt_segment*>(address);
        }

        // memory_interface (public)

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
            // memmove, not memcpy: callers (e.g. the PE loader's section copy) can hand this a source
            // buffer that overlaps the guest destination range - overlapping memcpy is undefined
            // behaviour. This is a generic guest memory-copy primitive with no non-overlap contract.
            std::memmove(data, reinterpret_cast<const void*>(address), size);
            return true;
        }

        void write_memory(uint64_t address, const void* data, size_t size) override
        {
            if (!this->try_write_memory(address, data, size))
            {
                char buf[128];
                snprintf(buf, sizeof(buf), "Failed to write FEX guest memory at 0x%llx size=0x%zx",
                         static_cast<unsigned long long>(address), size);
                throw std::runtime_error(buf);
            }
        }

        bool try_write_memory(uint64_t address, const void* data, size_t size) override
        {
            if (!this->is_range_mapped(address, size))
            {
                return false;
            }

            // sogen's loader writes guest memory it has already declared read-only (a PE section's raw
            // file bytes, regardless of the section's final protection). Unlike Unicorn's uc_mem_write,
            // this backend's guest VA == host VA is backed by real host mprotect state, so the write
            // needs a temporary permission bump. Every page must be checked, not just the first: a write
            // straddling into a read-only region would otherwise fault mid-memmove.
            const bool needs_temporary_write = !this->range_is_writable(address, size);

            if (needs_temporary_write)
            {
                this->set_temporary_write_access(address, size, true);
            }

            // memmove, not memcpy: see try_read_memory - the source may overlap the guest destination.
            std::memmove(reinterpret_cast<void*>(address), data, size);

            if (needs_temporary_write)
            {
                this->set_temporary_write_access(address, size, false);
            }

            // Writing to a mapped region may overwrite already-translated code; drop FEX's cache for it.
            this->invalidate_code_range(address, size);
            return true;
        }

        // hook_interface
        //
        // As with KVM, the guest runs natively, so fine-grained memory/execution/basic-block hooks
        // cannot fire; they are accepted and tracked (so delete_hook works) purely for API
        // compatibility. Only `syscall` instruction hooks are actually wired.

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
            // Enumerates everything currently mapped in this process via the Mach VM region API, the
            // Darwin equivalent of walking /proc/self/maps.
            std::vector<host_reserved_range> ranges;

            // Every 64-bit Mach-O executable reserves a __PAGEZERO segment spanning at least [0, 4GB),
            // an OS/linker convention enforced at the mmap syscall level rather than a listed VM region
            // mach_vm_region reports: its first real hit starts well above 4GB (ASLR-dependent), yet
            // mapping guest memory anywhere in the gap below still fails. So the whole gap up to the
            // scan's first real region is reserved rather than guessing a fixed size.
            //
            // The FEXCore-internal arena would otherwise be reported as many separate sub-regions
            // (PROT_NONE reservation, committed BlockLinks buffers, the MAP_JIT CodeBuffer, freed
            // holes). Registering it as one range instead covers sub-regions allocated lazily after this
            // one-shot snapshot and any transient holes, with no dependence on the scan's timing.
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

                if (first_region)
                {
                    if (address > 0)
                    {
                        ranges.push_back({.address = 0, .size = static_cast<size_t>(address)});
                    }
                    first_region = false;
                }

                // AddressSanitizer's sparse shadow map spans tens of GB up to multiple TB per region,
                // placed far above where the guest ever allocates. Feeding those to the memory manager
                // bloats reserved_regions_ into the thousands and stalls process setup, so they are
                // skipped - in instrumented builds only.
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
            // mach_vm_region's start-address parameter lets the kernel skip straight to the first region
            // at or above the window, so this visits only regions actually inside it (usually none),
            // instead of re-walking every region in the process - a count that grows unbounded over a
            // long session. The arena and the __PAGEZERO gap are captured by the first full scan at
            // startup and never released, so the only thing a rescan of an otherwise-free window adds is
            // a foreign mapping in a gap an earlier guest unmap returned to the OS.
            std::vector<host_reserved_range> ranges;

            const mach_vm_address_t window_start = address;
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

                const uint64_t hit_start = std::max<uint64_t>(region_addr, window_start);
                const uint64_t hit_end = std::min<uint64_t>(region_addr + region_size, window_end);
                ranges.push_back({.address = hit_start, .size = static_cast<size_t>(hit_end - hit_start)});

                probe = region_addr + region_size;
            }

            return ranges;
        }
#endif

      private:
        friend class fex_syscall_handler;

        // memory_interface (private)

        void map_mmio(uint64_t address, size_t size, mmio_read_callback read_cb, mmio_write_callback /*write_cb*/) override
        {
            // See mmio_region's doc comment - the region stays unmapped so accesses fault and emulate.
            if (!is_page_aligned(address) || !is_page_aligned(size))
            {
                throw std::runtime_error("FEX MMIO mappings must be page aligned");
            }

            this->mmio_regions_.emplace_back(mmio_region{.address = address, .size = size, .read_cb = std::move(read_cb)});
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
            // Pages already ours (from this or an adjacent guest region sharing the host page) keep
            // their mapping; sync_host_page_apple/map_memory applies the real permission on commit.
            // Everything else is claimed one mmap per contiguous run: whole module image reservations
            // come through here, and a per-page syscall makes process startup crawl.
            const uint64_t start = host_page_align_down_apple(address);
            const uint64_t end = host_page_align_up_apple(address + size);
            for (uint64_t host_page = start; host_page < end;)
            {
                if (this->mapped_host_pages_apple_.contains(host_page))
                {
                    host_page += host_page_size_apple;
                    continue;
                }

                uint64_t run_end = host_page + host_page_size_apple;
                while (run_end < end && !this->mapped_host_pages_apple_.contains(run_end))
                {
                    run_end += host_page_size_apple;
                }

                void* result = ::mmap(reinterpret_cast<void*>(host_page), static_cast<size_t>(run_end - host_page), PROT_NONE,
                                      MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
                if (result == MAP_FAILED || result != reinterpret_cast<void*>(host_page))
                {
                    throw std::runtime_error("FEX backend failed to reserve guest address range at the host level");
                }

                for (uint64_t claimed = host_page; claimed < run_end; claimed += host_page_size_apple)
                {
                    this->mapped_host_pages_apple_.insert(claimed);
                }

                host_page = run_end;
            }
        }

        void release_guest_address_range(uint64_t address, size_t size) override
        {
            // The caller guarantees the range holds no reserved guest ranges (see memory_interface), so
            // every host page wholly inside it is a stale claim and can go back to the OS. Boundary
            // pages are kept: their outside part may belong to a neighboring, still-live reservation.
            const uint64_t start = host_page_align_up_apple(address);
            const uint64_t end = host_page_align_down_apple(address + size);

            auto it = this->mapped_host_pages_apple_.lower_bound(start);
            while (it != this->mapped_host_pages_apple_.end() && *it + host_page_size_apple <= end)
            {
                ::munmap(reinterpret_cast<void*>(*it), host_page_size_apple);
                it = this->mapped_host_pages_apple_.erase(it);
            }
        }
#endif

        void map_host_memory(uint64_t address, size_t size, void* host_pointer, memory_permission permissions) override
        {
            if (!is_page_aligned(address) || !is_page_aligned(size))
            {
                throw std::runtime_error("FEX host memory mappings must be page aligned");
            }

            const uint64_t host_address = address;

#ifdef __APPLE__
            // Darwin has no mremap(); mach_vm_remap() is the Mach equivalent. VM_FLAGS_OVERWRITE
            // replaces whatever reservation the memory manager already put there, matching
            // mmap(MAP_FIXED); copy=FALSE aliases rather than duplicates, matching MREMAP_MAYMOVE.
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
            // Apple Silicon's unified memory makes CPU/GPU coherency for the Metal buffers behind
            // MoltenVK's Vulkan buffers likely, but it is not guaranteed across every Metal storage
            // mode this bridge might use. An unnecessary flush is a harmless no-op, whereas wrongly
            // claiming coherence surfaces as rendering corruption.
            return false;
        }

        void flush_host_memory_cache(const void* host_pointer, size_t size) override
        {
            if (host_pointer == nullptr || size == 0)
            {
                return;
            }

            // The ARM64 equivalent of the KVM backend's clflushopt+sfence pair: evict the CPU data cache
            // out to memory so a GPU reading the same physical pages non-coherently sees guest writes.
#ifdef __APPLE__
            // Darwin's public API for exactly this ("useful when dealing with cache incoherent devices
            // or DMA" - OSCacheControl.h), preferred over hand-rolled `dc civac`, since EL0 access to
            // cache-maintenance instructions is not something an embedder should assume is permitted.
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
            // MMIO regions (see mmio_region's doc comment) were never really mapped at the host level.
            if (std::erase_if(this->mmio_regions_, [address](const mmio_region& region) { return region.address == address; }))
            {
                return;
            }

#ifdef __APPLE__
            this->set_shadow_range_apple(address, size, std::nullopt);
            this->sync_host_pages_covering_apple(address, size);
#else
            ::munmap(reinterpret_cast<void*>(address), size);
#endif
            this->invalidate_code_range(address, size);
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

        // region bookkeeping

        // Callers are expected to have checked is_range_mapped already.
        bool range_is_writable(uint64_t address, size_t size) const
        {
            uint64_t cursor = address;
            const uint64_t end = address + size;

            while (cursor < end)
            {
                auto it = this->regions_.upper_bound(cursor);
                if (it == this->regions_.begin())
                {
                    return false;
                }
                --it;

                const uint64_t region_end = it->first + it->second.size;
                if (cursor < it->first || cursor >= region_end ||
                    (it->second.permissions & memory_permission::write) == memory_permission::none)
                {
                    return false;
                }
                cursor = region_end;
            }

            return true;
        }

        // For loader-privileged writes: sogen itself writing guest memory it declared read-only, such as
        // a PE section's initial file content before its final permission is locked in. Needed because
        // this backend enforces the declared permission through real host protection, unlike Unicorn's
        // uc_mem_write, which operates on emulated memory independent of any host mprotect state.
        void set_temporary_write_access(uint64_t address, size_t size, bool enable)
        {
#ifdef __APPLE__
            const uint64_t start = host_page_align_down_apple(address);
            const uint64_t end = host_page_align_up_apple(address + size);
            for (uint64_t host_page = start; host_page < end; host_page += host_page_size_apple)
            {
                if (!enable)
                {
                    // Restores via sync_host_page_apple, which re-derives the declared permissions
                    // from the shadow table.
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
                ::mprotect(reinterpret_cast<void*>(host_page), host_page_size_apple, to_prot_apple(effective | memory_permission::write));
            }
#else
            // The range can span several regions_ entries with different declared permissions, so
            // both the bump and the restore work per intersecting entry.
            const uint64_t end = address + size;

            auto it = this->regions_.upper_bound(address);
            if (it != this->regions_.begin())
            {
                --it;
            }

            for (; it != this->regions_.end() && it->first < end; ++it)
            {
                const uint64_t region_end = it->first + it->second.size;
                if (region_end <= address)
                {
                    continue;
                }

                const uint64_t sub_start = std::max(it->first, address & ~(page_size - 1));
                const uint64_t sub_end = std::min(region_end, (end + page_size - 1) & ~(page_size - 1));
                const memory_permission perm = enable ? (it->second.permissions | memory_permission::write) : it->second.permissions;
                ::mprotect(reinterpret_cast<void*>(sub_start), sub_end - sub_start, to_prot(perm));
            }
#endif
        }

        // Keeps regions_ non-overlapping, the invariant is_range_mapped/range_is_writable rely on. The
        // guest memory manager commits a reserved region in gap-filling sub-ranges (several map_memory
        // calls, so several entries tile one committed region) but decommits it in one call, so an
        // unmap range spans several entries and does not start at each entry's key. A plain
        // regions_.erase(address) would orphan the rest, and is_range_mapped's --upper_bound walk would
        // later land on a stale inner entry and wrongly report "not mapped". Entries straddling an edge
        // are trimmed or split, since the host-level unmap only touches [address, address+size).
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

        // Splits entries straddling an edge so only the in-range part changes. A guest protection change
        // can target a sub-range of a larger committed region, start mid-entry, or span several entries,
        // all of which a plain regions_.find(address) either misses or over-applies. Since
        // QueryGuestExecutableRange decides executability straight from these recorded permissions, a
        // stale entry makes the JIT reject a legitimately-executable page or execute one it should not.
        // Gaps in the tiling are preserved: unmapped holes are never fabricated as mapped.
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
        // 16KB-host vs 4KB-guest permission reconciliation. nullopt permissions means unmap: the pages
        // become "never requested" again, which must still fault like reserved-but-uncommitted guest
        // memory rather than being silently allowed.
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

        // Derives one 16KB host page's permission from its up to four 4KB shadow slots, taking their
        // union when they disagree. The union also swallows the "some slot absent" case: a
        // guard/reserved slot sharing a host page with mapped memory is folded in rather than made to
        // fault, a deliberate relaxation until a Mach exception handler can resolve faults on a
        // PROT_NONE page without breaking a legitimate access from a stricter neighbor.
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

            void* host_ptr = reinterpret_cast<void*>(host_page_addr);
            const bool currently_mapped = this->mapped_host_pages_apple_.contains(host_page_addr);

            if (!any_slot_present)
            {
                if (currently_mapped)
                {
                    // The guest range may merely be decommitted while still MEM_RESERVE'd, so the page
                    // must stay claimed: munmapping it would let a foreign host allocation land here
                    // and be clobbered by a later recommit's MAP_FIXED. Replacing the mapping rather
                    // than mprotect'ing it discards the old contents, so a recommit sees the zeroed
                    // pages MEM_COMMIT requires. release_guest_address_range drops the claim for good.
                    void* result =
                        ::mmap(host_ptr, host_page_size_apple, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
                    if (result != host_ptr)
                    {
                        throw std::runtime_error("FEX backend failed to re-reserve decommitted guest memory");
                    }
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

        // FEX context plumbing

        void initialize_context()
        {
            // Without an installed handler, LogMan silently discards the formatted message (LogManager.
            // cpp's `if (Handler)`) while still executing FEX_TRAP_EXECUTION for ASSERT-level messages,
            // so every internal FEXCore assertion failure crashes with no indication of what failed.
            LogMan::Msg::InstallHandler([](LogMan::DebugLevels level, const char* message) {
                fprintf(stderr, "[FEXCore LogMan] level=%s: %s\n", LogMan::DebugLevelStr(level), message);
            });
            LogMan::Throw::InstallHandler([](const char* message) { fprintf(stderr, "[FEXCore LogMan THROW] %s\n", message); });

#ifdef __APPLE__
            // Must happen before the first FEXCore-internal allocation (CreateNewContext allocates the
            // CodeBuffer and dispatcher) and before reserved_host_ranges() is first queried, so the
            // whole arena is off-limits to guest allocations.
            fex_internal_arena::instance().install();
#endif

            // libc++abi's default terminate handler prints nothing useful for an uncaught exception.
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

            // With EMULATOR_FEX_DUMPIR naming an existing directory, FEXCore writes one file per
            // translated guest basic block, keyed by guest RIP: "<rip:x>-pre.ir" straight out of the
            // decoder and "<rip:x>-post.ir" after optimization and register allocation. Unlike inline
            // hot-path diagnostics, this is confirmed not to perturb timing-sensitive JIT bugs.
            // PassManagerDumpIR value 3 == BEFOREOPT(1)|AFTEROPT(2).
            if (const char* dumpir_dir = std::getenv("EMULATOR_FEX_DUMPIR"))
            {
                FEXCore::Config::Set(FEXCore::Config::CONFIG_DUMPIR, dumpir_dir);
                FEXCore::Config::Set(FEXCore::Config::CONFIG_PASSMANAGERDUMPIR, "3");
            }

            FEXCore::Config::Set(FEXCore::Config::CONFIG_IS64BIT_MODE, "1");

            // Piggybacks on FEXCore's GdbServer flag, whose only effect inside FEXCore (ContextImpl::
            // InitCore) is setting Config.NeedsPendingInterruptFaultCheck, making the JIT emit a
            // `str zr, [InterruptFaultPage]` at every block entry - the mechanism request_thread_stop()
            // needs to force a stuck-in-JIT thread to fault. FEXCore's own gdbserver is unused (sogen
            // has its own stub), so there is no other observable effect.
            FEXCore::Config::Set(FEXCore::Config::CONFIG_GDBSERVER, "1");

#ifdef __APPLE__
            const FEXCore::HostFeatures features = fetch_host_features_apple();
#else
            const FEXCore::HostFeatures features{}; // TODO(fex): FEXCore::FetchHostFeatures() on real HW.
#endif
            this->context_ = FEXCore::Context::Context::CreateNewContext(features);

            this->syscall_handler_ = std::make_unique<fex_syscall_handler>(*this);
            this->context_->SetSyscallHandler(this->syscall_handler_.get());

            // InitCore() requires a non-null SignalDelegator, and the dispatcher entry-point addresses
            // handle_fault_signal needs come from non-virtual base-class methods, so the plain base
            // suffices - fault delivery is handled by the host signal handler installed below.
            this->signal_delegator_ = std::make_unique<FEXCore::SignalDelegator>();
            this->context_->SetSignalDelegator(this->signal_delegator_.get());

            this->context_->InitCore();

#ifdef __APPLE__
            install_fault_signal_handlers(*this);
#endif
        }

#ifdef __APPLE__
      public:
        // Applies a decode_arm64_load result once its data has been fetched, writing the possibly
        // extended value into the destination register and advancing PC past the decoded instruction.
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
                // stdio is not async-signal-safe and this runs inside a real signal handler, hence
                // snprintf into a fixed stack buffer followed by a single write(2).
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

        // LDAR/LDAPR/STLR require natural alignment on real hardware, unlike plain LDR/STR, but x86
        // permits unaligned accesses freely and FEX uses this family to model x86's stronger memory
        // ordering, so an ordinary unaligned guest access to legitimately mapped memory faults here
        // (Darwin reports SIGBUS/BUS_ADRALN). sogen runs every guest thread of a process cooperatively
        // on one host thread, so there is no concurrent host-thread race for these instructions to
        // order against, making the downgrade to a plain access correctness-preserving.
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

        // memory_violation_hooks_/interrupt_hooks_ callbacks are backend-agnostic windows-emulator code
        // that allocates, logs and mutates STL containers freely. That is fine from normal call context
        // (where KVM/Unicorn invoke them) but not from inside handle_fault_signal, which can interrupt
        // an unrelated malloc()/free() on this thread - a confirmed, ASLR-timing-dependent heap
        // corruption hazard. So the handler stashes plain data here instead and forces ExecuteThread to
        // unwind back to start() via ThreadStopHandlerAddress, without touching stop_requested_; start()
        // dispatches the hook in normal context and resumes by re-entering ExecuteThread, which always
        // restarts from CurrentFrame->State.rip.
        enum class pending_fault_kind
        {
            none,
            memory_violation,
            interrupt,
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

        // Called only from start(), in normal call context, right after ExecuteThread returns. The
        // return value tells start()'s loop whether ExecuteThread came back because a hook was deferred
        // or because of a genuine stop.
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
            case pending_fault_kind::none:
            default:
                return false;
            }
        }

        // Called only from signal-handler context; see pending_fault_kind for why hooks cannot run
        // directly there.
        //
        // sra_already_spilled picks between the dispatcher's two unwind entry points
        // (ThreadStopHandlerAddressSpillSRA falls through SpillStaticRegs into ThreadStopHandlerAddress).
        // A fault taken via FEXCore's own synthetic-exception path (vector 14) has SRA already spilled to
        // CpuStateFrame and passes true; a real hardware fault interrupting live translated code passes
        // false, since SRA is still only in host registers there and skipping the spill would leave the
        // next ExecuteThread entry reading stale state.
        void defer_hook_dispatch(ucontext_t* uctx, const pending_fault_dispatch& dispatch, bool sra_already_spilled)
        {
            this->pending_fault_dispatch_ = dispatch;
            const auto& cfg = this->signal_delegator_->GetConfig();
            const auto target = sra_already_spilled ? cfg.ThreadStopHandlerAddress : cfg.ThreadStopHandlerAddressSpillSRA;
            arm_thread_state64_set_pc_fptr(uctx->uc_mcontext->__ss, reinterpret_cast<void*>(target));
        }

        // The dispatcher is host MAP_JIT code like a CodeBuffer, but IsAddressInCodeBuffer does not
        // recognize it, so the CodeBuffer-gated W^X retries never fire for a dispatcher fault.
        bool host_pc_in_dispatcher(uint64_t pc) const
        {
            if (this->signal_delegator_ == nullptr)
            {
                return false;
            }
            const auto& cfg = this->signal_delegator_->GetConfig();
            return pc >= cfg.DispatcherBegin && pc < cfg.DispatcherEnd;
        }

        // FEXCore's call-ret shadow stack (x25, see ensure_callret_buffer) is a return-address predictor
        // bracketed by a guard page on each side. A deep guest call chain, or a stack pivot/longjmp
        // abandoning already-pushed frames as steam_api.dll's RLD DRM does, legitimately runs past the
        // committed region into a guard page. Upstream FEX treats that as expected and recovers by
        // resetting the register to the buffer's default location (Linux
        // SyscallHandler::HandleSegfault, Windows FEX::Windows::CallRetStack::HandleAccessViolation),
        // which this mirrors: Base +- host_page guard, DefaultLocation = Base + CALLRET_STACK_SIZE/4,
        // matching GetCallRetStackInfo. Classified by shape rather than si_code, since Darwin reports a
        // PROT_NONE guard-page hit as SEGV_ACCERR/SEGV_MAPERR/BUS_ADRALN interchangeably.
        bool handle_callret_stack_fault(ucontext_t* uctx, uint64_t fault_addr) const
        {
            if (this->thread_ == nullptr || this->thread_->CallRetStackBase == nullptr)
            {
                return false;
            }
            const auto base = reinterpret_cast<uint64_t>(this->thread_->CallRetStackBase);
            const auto host_page = static_cast<uint64_t>(::getpagesize());
            constexpr uint64_t callret_stack_size = FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE;
            if (fault_addr < base - host_page || fault_addr >= base + callret_stack_size + host_page)
            {
                return false;
            }
            uctx->uc_mcontext->__ss.__x[25] = base + callret_stack_size / 4;
            return true;
        }

        // Real (non-synthetic) guest memory violations, which have no other path to
        // memory_violation_hooks_: a Windows PAGE_GUARD page, genuinely unmapped memory, or a host page
        // mprotect'd to PROT_NONE because one 4KB guest slot inside it is guard/unmapped while another
        // is legitimately mapped. The shadow table for the specific 4KB page decides: an operation
        // exceeding what is declared there is a real violation and gets deferred to the hooks, otherwise
        // the access is a false fault from a stricter neighbor sharing the host page and is
        // decode-and-emulated the same way handle_misaligned_atomic_fault does.
        bool handle_general_memory_violation(ucontext_t* uctx, uint64_t fault_addr)
        {
            const uint64_t pc = arm_thread_state64_get_pc(uctx->uc_mcontext->__ss);
            const auto guest_fault_addr = fault_addr;
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

            // FEX's block chaining (directly-linked blocks and callret RET fast-paths) advances execution
            // without rewriting CurrentFrame->State.rip, so it holds whatever was last written to it and
            // is frequently stale here. The memory-violation hook reads State.rip as the faulting
            // instruction pointer, so it must be reconstructed from the live host PC - the same mechanism
            // FEX's own ReconstructThreadState uses. Guarded on a non-zero result so a failed
            // reconstruction never zeroes a usable stale rip.
            if (const uint64_t recon_rip = this->context_->RestoreRIPFromHostPC(this->thread_, pc))
            {
                this->thread_->CurrentFrame->State.rip = recon_rip;
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

        // Returns false for anything it does not recognize, which the wrapper below reports as an
        // unhandled host fault and re-raises with default disposition.
        bool handle_fault_signal(int sig, siginfo_t* info, void* raw_ucontext)
        {
            if (this->thread_ == nullptr)
            {
                return false;
            }

            auto* uctx = static_cast<ucontext_t*>(raw_ucontext);

            if (sig == SIGSEGV || sig == SIGBUS)
            {
                const auto fault_addr = reinterpret_cast<uint64_t>(info->si_addr);

                // Must run before any si_code-specific branch below, because Darwin can report this
                // PROT_NONE violation as BUS_ADRALN instead of SEGV_ACCERR/SEGV_MAPERR. Since
                // InterruptFaultPage is never inside the CodeBuffer, a misclassified fault would
                // otherwise fall through to handle_general_memory_violation, which treats fault_addr as
                // a guest address and would dispatch a synthetic guest exception built from this
                // backend's own internal pointer.
                const auto interrupt_page_addr = reinterpret_cast<uint64_t>(this->thread_->InterruptFaultPage);
                if (fault_addr >= interrupt_page_addr && fault_addr < interrupt_page_addr + sizeof(this->thread_->InterruptFaultPage))
                {
                    const auto fault_pc = arm_thread_state64_get_pc(uctx->uc_mcontext->__ss);
                    const bool is_dispatch_code = this->context_ && this->context_->IsAddressInCodeBuffer(this->thread_, fault_pc);

                    // ExitFunctionLinkerAddress's own epilogue (EmitSignalGuardedRegion's closing
                    // sequence) also writes InterruptFaultPage from inside the CodeBuffer, via a
                    // JIT-emitted `strb`, so IsAddressInCodeBuffer cannot tell it apart from a genuine
                    // per-block-entry interrupt check. Redirecting to ThreadStopHandlerAddress while
                    // mid-epilogue pops the dispatcher's frame at the wrong stack depth. The raw
                    // instruction word distinguishes them: STRB (unsigned-offset immediate) encodes
                    // size=00,V=0,opc=00, distinct from both of EmitSuspendInterruptCheck's forms
                    // (64-bit `str` has size=11, the 128-bit vector `str` has V=1).
                    const bool is_strb_epilogue_write = (*reinterpret_cast<const uint32_t*>(fault_pc) & 0xFFC00000u) == 0x39000000u;

                    if (is_dispatch_code && !is_strb_epilogue_write)
                    {
                        // A genuine cooperative stop at a JIT block-entry interrupt check, triggered by
                        // the quantum timer's async mprotect. Live guest state is in host registers
                        // (SRA) and CPUState.rip is frequently stale, since a completed syscall leaves
                        // it at its fallthrough and execution then runs through directly-linked blocks
                        // and callret RET fast-paths that never rewrite it. Resuming from that stale rip
                        // with stale registers re-executes an already-retired instruction - a `retn`
                        // whose return slot was reused pops garbage and branches wild. Both halves are
                        // therefore required: reconstruct the rip from the faulting host PC, and unwind
                        // through the SpillSRA handler so the live SRA registers reach CPUState before
                        // ExecuteThread returns. This mirrors FEX's own ReconstructThreadState.
                        this->thread_->CurrentFrame->State.rip = this->context_->RestoreRIPFromHostPC(this->thread_, fault_pc);
                        this->interrupt_page_unwind_ = true;
                        const auto& stop_cfg = this->signal_delegator_->GetConfig();
                        arm_thread_state64_set_pc_fptr(uctx->uc_mcontext->__ss,
                                                       reinterpret_cast<void*>(stop_cfg.ThreadStopHandlerAddressSpillSRA));
                        return true;
                    }

                    // Not a block-entry check but ordinary bookkeeping racing a stop request from another
                    // thread: either FEXCore's DeferredSignalRefCountGuard destructor or
                    // ExitFunctionLinkerAddress's JIT-emitted epilogue strb. ThreadStopHandlerAddress
                    // expects to unwind a live JIT dispatcher frame, so taking it from the host-C++ side
                    // corrupts the stack into a pc==lr==0 crash. Only the page's protection state drives
                    // the stop mechanism, not the stored value, so skip the faulting store; the next real
                    // block entry still sees the page protected and stops correctly.
                    arm_thread_state64_set_pc_fptr(uctx->uc_mcontext->__ss, reinterpret_cast<void*>(fault_pc + 4));
                    return true;
                }

                // A W^X instruction-fetch fault on FEXCore's dispatcher trampoline: it is host MAP_JIT
                // code like a CodeBuffer, but IsAddressInCodeBuffer does not recognize it, so the
                // CodeBuffer-gated retries below never fire for it, and this handler's other retry paths
                // can leave the thread's JIT write-protect in write mode. Classified by shape - an
                // instruction fetch inside the dispatcher range - rather than si_code, since Darwin
                // reports this as any of SEGV_ACCERR/SEGV_MAPERR/BUS_ADRALN.
                //
                // Deliberately unbounded, unlike the CodeBuffer cases: a host pc inside the dispatcher
                // is unambiguously FEXCore's own code on genuine RWX-capable MAP_JIT, so toggling
                // execute mode always lets the fetch succeed and can never spin. A spuriously exhausted
                // budget would instead drop the fault into handle_general_memory_violation, which would
                // mis-read the host dispatcher address as a guest access violation.
                {
                    const auto host_pc = arm_thread_state64_get_pc(uctx->uc_mcontext->__ss);
                    if (fault_addr == host_pc && this->host_pc_in_dispatcher(host_pc))
                    {
                        ::pthread_jit_write_protect_np(1);
                        return true;
                    }
                }

                // Checked early, before the general-violation routing that would otherwise mis-dispatch
                // this host arena address as a guest access violation.
                if (this->handle_callret_stack_fault(uctx, fault_addr))
                {
                    return true;
                }

                // An instruction-fetch fault reports si_addr == pc, which a genuine MMIO data access
                // never does, so excluding it costs no real MMIO hits. It matters because a bad branch
                // to a garbage pc can land inside a registered mmio_region by chance; handle_mmio_fault
                // would then try to decode the instruction at that same garbage address and crash there
                // instead, masking the real bug behind a confusing secondary symptom.
                const auto pc_for_mmio_check = arm_thread_state64_get_pc(uctx->uc_mcontext->__ss);
                if (fault_addr != pc_for_mmio_check)
                {
                    for (const auto& region : this->mmio_regions_)
                    {
                        if (fault_addr >= region.address && fault_addr < region.address + region.size)
                        {
                            return this->handle_mmio_fault(uctx, region, fault_addr);
                        }
                    }
                }

                // A BUS_ADRALN whose address is inside the live CodeBuffer but whose PC is FEXCore's own
                // host C++ code (ExitFunctionLink's self-modifying write) is not a real alignment fault
                // at all - it decodes to an ordinary aligned 32-bit `str`. It is the JIT W^X race, which
                // Darwin sometimes reports as BUS_ADRALN instead of SEGV_ACCERR/SEGV_MAPERR, the same
                // si_code ambiguity documented at the JITGuardPage branch below. Falling through to the
                // BUS_ADRALN branch would route it into handle_general_memory_violation, which unwinds
                // via ThreadStopHandlerAddress as if interrupting the JIT dispatcher's own frame -
                // wrong here, since execution is several real C++ frames deep inside FEXCore, so popping
                // "the dispatcher's" callee-saved registers corrupts STATE (x28) and other SRA registers
                // with stack garbage and crashes later on an unrelated-looking null-Frame dereference.
                // Treated instead like the write-protect race below, with the same per-address bound.
                if (sig == SIGBUS && info->si_code == BUS_ADRALN && this->context_ &&
                    this->context_->IsAddressInCodeBuffer(this->thread_, fault_addr))
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

                // A genuine guest-memory alignment fault (the CodeBuffer case above returns early).
                // Routed through handle_general_memory_violation rather than calling
                // handle_misaligned_atomic_fault directly, because that helper's "legitimately mapped
                // memory" premise is not guaranteed: a guest instruction can compute a garbage address
                // that merely happens to also be unaligned, and memcpy-ing to it would fault a second
                // time inside the signal handler. The shadow-table-validated path classifies such an
                // address correctly and raises it via memory_violation_hooks_ instead.
                if (sig == SIGBUS && info->si_code == BUS_ADRALN && this->handle_general_memory_violation(uctx, fault_addr))
                {
                    return true;
                }
            }

            // FEXCore protects the last host page of each CodeBuffer and deliberately writes into it
            // mid-compile to detect running out of space, so this is expected: resume via the jump
            // buffer it set up beforehand, mirroring SignalDelegator.cpp's HandleFrontendSIGSEGV.
            // Unlike Linux's single SIGSEGV, Darwin reports these as either SIGSEGV or SIGBUS, and
            // sometimes as SEGV_MAPERR ("not mapped") rather than SEGV_ACCERR ("wrong permission") -
            // confirmed by querying mach_vm_region from inside this handler and finding the address
            // fully mapped RWX despite the si_code=1 report - so both si_codes are treated alike.
            if ((sig == SIGSEGV || sig == SIGBUS) && (info->si_code == SEGV_ACCERR || info->si_code == SEGV_MAPERR))
            {
                const auto guard_page = this->thread_->JITGuardPage;
                const auto fault_addr = reinterpret_cast<uintptr_t>(info->si_addr);
                if (guard_page != 0 && fault_addr >= guard_page && fault_addr < guard_page + FEXCore::Utils::FEX_HOST_PAGE_SIZE)
                {
                    auto* gprs = reinterpret_cast<uint64_t*>(&uctx->uc_mcontext->__ss);
                    auto* fprs = reinterpret_cast<__uint128_t*>(&uctx->uc_mcontext->__ns.__v[0]);
                    auto* pc_ptr = reinterpret_cast<uint64_t*>(&uctx->uc_mcontext->__ss.__pc);
                    FEXCore::UncheckedLongJump::ManuallyLoadJumpBuf(this->thread_->RestartJump, this->thread_->JITGuardOverflowArgument,
                                                                    gprs, fprs, pc_ptr);
                    return true;
                }

                // FEXCore's code-patching paths (ExitFunctionLink, block delinkers) do not reliably
                // leave this thread's JIT write-protect state correct across their self-modifying
                // writes, so set whatever the faulting access needs and retry the same instruction.
                // An instruction fetch (PC == fault address) needs execute mode, a data write needs
                // write mode; guessing wrong just re-faults and consumes a retry harmlessly.
                //
                // The IsAddressInCodeBuffer gate matters: without it the branch fires for any
                // SEGV_ACCERR/SEGV_MAPERR, burning retries toggling W^X on faults that were never a
                // CodeBuffer race - a branch-to-null would be retried this way before falling through.
                if (this->context_ && this->context_->IsAddressInCodeBuffer(this->thread_, fault_addr))
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

            // FEXCore's guest-exception trampoline always re-enters within the dispatcher range.
            if (!this->host_pc_in_dispatcher(pc))
            {
                // The common case: translated guest code faulted directly. Only attempted once pc is
                // confirmed inside a live JIT code buffer - anything else is a host bug elsewhere that
                // must not be interpreted as guest state, and the wrapper below logs and re-raises it.
                if ((sig == SIGSEGV || sig == SIGBUS) && this->context_ && this->context_->IsAddressInCodeBuffer(this->thread_, pc) &&
                    this->handle_general_memory_violation(uctx, reinterpret_cast<uint64_t>(info->si_addr)))
                {
                    return true;
                }

                return false;
            }

            auto* frame = this->thread_->CurrentFrame;
            if (!frame->SynchronousFaultData.FaultToTopAndGeneratedException)
            {
                return false;
            }

            // FEXCore's IR "Break" op raises this both for x86 conditions with a compile-time-known trap
            // vector (HLT/UD2/INT3/INT1/INTO/unhandled INT N) and for its own synthetic #PF (NoExecOp,
            // when QueryGuestExecutableRange reports an address is not executable), so vector 14 needs
            // the fault address while everything else is a plain exception vector. Mirrors the KVM
            // backend's #PF vs. other-vector split in handle_exception().
            auto vector = static_cast<int>(frame->SynchronousFaultData.TrapNo);

            // sogen has no guest IDT, so a guest `INT N` FEXCore cannot dispatch synthesizes a #GP(13)
            // whose error code names the referenced IDT selector (bit 1 set, index in bits[15:3]), just
            // as real hardware does for an absent IDT gate. Unicorn/KVM model no IDT lookup and report
            // `INT N` as vector N, so FEX's more faithful #GP is remapped back to the plain vector the
            // shared interrupt dispatch expects - otherwise a __fastfail `int 0x29` re-faults forever.
            constexpr int gp_fault_vector = 13;
            constexpr uint32_t idt_reference_bit = 0x2;
            if (vector == gp_fault_vector && (frame->SynchronousFaultData.err_code & idt_reference_bit) != 0)
            {
                vector = static_cast<int>(frame->SynchronousFaultData.err_code >> 3);
            }

            // Must be reset before deferring, not after: it gates re-entry into this branch, and the
            // hook does not run until start() reaches it, so the next real fault of this shape would
            // otherwise be swallowed in the meantime.
            frame->SynchronousFaultData.FaultToTopAndGeneratedException = false;

            pending_fault_dispatch dispatch{};
            if (vector == 14)
            {
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
                // FEXCore's INT3 translation stores the post-instruction RIP (INTOp's SetRIPToNext),
                // while NT and the KVM/WHP backends report #BP at the 0xCC itself. Only the direct #BP
                // needs normalizing: a GP-remapped `INT N` (e.g. the `CD 2D` debug service trap, whose
                // bytes exception_dispatch.cpp expects to find at RIP) already stores its own address.
                if (frame->SynchronousFaultData.TrapNo == FEXCore::X86State::X86_TRAPNO_BP)
                {
                    frame->State.rip -= 1;
                }

                dispatch.kind = pending_fault_kind::interrupt;
                dispatch.vector = vector;
            }

            // SRA is already spilled: this is FEXCore's own controlled synthetic-exception/Break-op
            // path, not an arbitrary interruption of live JIT code.
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

            // FEXCore's core does not set up the call-ret shadow stack; on Linux that is embedder glue
            // in ThreadManager::CreateThread, replicated here. Without it the first x86 CALL in compiled
            // code dereferences a null callret_sp and crashes.
            this->ensure_callret_stack(this->thread_->CurrentFrame->State);

#ifdef __APPLE__
            // See exit_function_link_jit_write_wrapper: the call-site patch must happen with this
            // thread's JIT write-protection disabled.
            g_original_exit_function_link = this->thread_->CurrentFrame->Pointers.ExitFunctionLink;
            this->thread_->CurrentFrame->Pointers.ExitFunctionLink = reinterpret_cast<uint64_t>(&exit_function_link_jit_write_wrapper);
#endif
        }

        // FEXCore's call-ret shadow stack has no notion of "logical guest thread" - callret_sp is just a
        // raw pointer into whatever host buffer this sets up. sogen models logical guest threads as
        // CPUState snapshots swapped in and out of one FEXCore thread, so a shared buffer would let one
        // logical thread's pushes corrupt the pending frames of another suspended mid-call-chain. Each
        // gets its own, identified by state._pad1, the unused CPUState padding right after callret_sp,
        // doubling as a marker: 0 means this snapshot has never been assigned one, non-zero is the
        // buffer's base, safe to reuse since it round-trips with the rest of the snapshot. Callers point
        // InternalThreadState::CallRetStackBase at it themselves.
        void ensure_callret_buffer(FEXCore::Core::CPUState& state)
        {
            if (state._pad1 == 0)
            {
                // Guard pages on both sides, sized to the real host page (getpagesize(), not the
                // guest's fixed 4KB) so mprotect can't spill onto the guard.
                const size_t host_page = static_cast<size_t>(::getpagesize());
                constexpr size_t callret_stack_size = FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE;
                const size_t callret_alloc_size = callret_stack_size + 2 * host_page;

                // Routed through FEXCore::Allocator::mmap rather than raw ::mmap so that on Apple it
                // hits the fex_internal_arena hook and lands inside the guest-excluded arena. The JIT
                // consumes callret_sp as a plain host pointer, so a buffer aliasing the live guest stack
                // would let a host-side push/pop scribble guest memory with no guest instruction
                // involved. On Linux the hook defaults to raw ::mmap, so behavior there is unchanged.
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

        // Ensures the thread's call-ret buffer exists and points CallRetStackBase at it.
        void ensure_callret_stack(FEXCore::Core::CPUState& state)
        {
            this->ensure_callret_buffer(state);
            this->thread_->CallRetStackBase = reinterpret_cast<void*>(state._pad1);
        }

        // CPUState is owned by the thread frame once a thread exists. Before the thread is created we
        // stage register accesses in a local CPUState so the Windows loader can set up the initial
        // context; create_thread() seeds the real thread from it.
        FEXCore::Core::CPUState& cpu_state()
        {
            if (this->thread_ != nullptr)
            {
                return this->thread_->CurrentFrame->State; // TODO(fex): confirm field path for the FEX version.
            }
            return this->staged_state_;
        }

        const FEXCore::Core::CPUState& cpu_state() const
        {
            if (this->thread_ != nullptr)
            {
                return this->thread_->CurrentFrame->State;
            }
            return this->staged_state_;
        }

        uint64_t read_rflags() const
        {
            // FEXCore's ReconstructCompactedEFLAGS requires a live thread (it dereferences Thread to
            // reach CurrentFrame->State); before create_thread(), fall back to the local
            // reimplementation operating on the staged CPUState directly (see reconstruct_compacted_eflags).
            if (this->thread_ != nullptr)
            {
                // At rest (not in JIT) WasInJIT=false and the host GPR/PSTATE inputs are unused.
                return this->context_->ReconstructCompactedEFLAGS(this->thread_, /*WasInJIT=*/false, nullptr, 0);
            }
            return reconstruct_compacted_eflags(this->staged_state_);
        }

        void write_rflags(uint64_t rflags)
        {
            if (this->thread_ != nullptr)
            {
                this->context_->SetFlagsFromCompactedEFLAGS(this->thread_, static_cast<uint32_t>(rflags));
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
            if (this->thread_ != nullptr && (permissions & memory_permission::exec) != memory_permission::none)
            {
                this->syscall_handler_->MarkGuestExecutableRange(this->thread_, address, size);
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
            // Invalidating a range can synchronously delink already-linked call sites (AddBlockLink's
            // delinker callbacks), writing into a MAP_JIT buffer - the same per-thread write-protect
            // requirement as exit_function_link_jit_write_wrapper.
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

        void invalidate_code_range(uint64_t address, size_t size) const
        {
            this->invalidate_code_range_in(this->context_.get(), this->thread_, address, size);
        }

        void request_thread_stop()
        {
            // Forces the in-flight ExecuteThread to return, from this thread or another. With
            // Config.NeedsPendingInterruptFaultCheck set (see initialize_context's CONFIG_GDBSERVER
            // comment) the JIT emits a `str zr, [InterruptFaultPage]` at every block entry, so
            // protecting that page makes the next entry fault into handle_fault_signal, which redirects
            // it to ThreadStopHandlerAddress without consulting stop_requested_.
            if (this->thread_ == nullptr)
            {
                return;
            }

            ::mprotect(this->thread_->InterruptFaultPage, sizeof(this->thread_->InterruptFaultPage), PROT_NONE);
        }

        emulator_hook* make_hook()
        {
            return reinterpret_cast<emulator_hook*>(this->next_hook_id_++);
        }

        // state

        fextl::unique_ptr<FEXCore::Context::Context> context_{};
        FEXCore::Core::InternalThreadState* thread_ = nullptr;

        std::unique_ptr<fex_syscall_handler> syscall_handler_{};
        // Does no fault handling; the plain base only carries the dispatcher config
        // (ThreadStopHandlerAddress, DispatcherBegin/End) that handle_fault_signal reads.
        std::unique_ptr<FEXCore::SignalDelegator> signal_delegator_{};
        FEXCore::Core::CPUState staged_state_{};

        uint64_t gdt_base_ = 0;
        uint32_t gdt_limit_ = 0;

        std::atomic<bool> stop_requested_{false};
        uintptr_t next_hook_id_ = 1;

#ifdef __APPLE__
        pending_fault_dispatch pending_fault_dispatch_{};
        // Set by handle_fault_signal on an InterruptFaultPage unwind, consumed by start()'s loop to tell
        // it apart from any other clean return. Atomic even though both ends are the same thread: the
        // C++ abstract machine has no signal-delivery control-flow edge, so an optimizer may treat this
        // as unmodified across the opaque ExecuteThread() call and cache a stale read.
        std::atomic<bool> interrupt_page_unwind_{false};
#endif

        std::map<uint64_t, mapped_region> regions_;
        std::vector<mmio_region> mmio_regions_;

        // Every per-logical-thread call-ret buffer ever allocated by ensure_callret_buffer, so the
        // destructor can release them - these live outside regions_ (host allocator space, not the
        // guest address space) and outlive any individual CPUState snapshot they were allocated for.
        std::vector<std::pair<void*, size_t>> callret_buffers_;

#ifdef __APPLE__
        // Apple Silicon's 16KB host page is coarser than the guest's 4KB architectural page, so one host
        // mprotect cannot always express what the guest requested per 4KB page - a PE image's .text (RX)
        // directly followed by .data (RW) share a host page. page_shadow_apple_ is the per-4KB source of
        // truth (absent = never requested, which must still fault); mapped_host_pages_apple_ tracks
        // which host pages have a live mmap, so sync_host_page_apple can tell a first-time mmap from a
        // protection change.
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

            // stdio is not async-signal-safe; snprintf into a fixed stack buffer plus a single write(2).
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

    uint64_t fex_syscall_handler::HandleSyscall(FEXCore::Core::CpuStateFrame* /*frame*/, FEXCore::HLE::SyscallArguments* /*args*/)
    {
        // SyscallOp sets CPUState.rip to the address of the `syscall` instruction itself before invoking
        // us, which is the convention sogen's shared syscall layer expects: it leaves rip so that
        // advancing by the 2-byte syscall length always yields the intended next rip - the following
        // instruction for a plain syscall, and the real target for a redirecting one, which sets
        // rip = target - 2. On non-_WIN32 hosts `syscall` is not FLAGS_BLOCK_END, so without the
        // unconditional advance below the JIT falls through and either re-executes the syscall on block
        // re-entry or, for a redirect, branches into the middle of an instruction at target - 2.
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
        // FEXCore checks this before compiling a guest address and synthesizes a #PF if it is not
        // reported executable here, so a blanket {} would make every guest instruction fetch look like
        // a DEP violation.
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

        return {
            .Base = it->first,
            .Size = region_end - it->first,
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
