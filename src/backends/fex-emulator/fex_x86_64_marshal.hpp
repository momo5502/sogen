#pragma once

#include <cstring>

#include <FEXCore/Core/CoreState.h>

// Header-only WoW64 gate-crossing state marshaling, factored out of fex_x86_64_emulator.cpp so it
// can be exercised directly by a standalone spike (src/spikes/fex-mac-gate-crossing) without the
// rest of the backend - the only way to verify the crossing at all until wow64.dll actually loads
// organically (task #27 in the plan).
namespace sogen::fex
{
    // Copies genuinely-architectural x86 CPU state (the register file) from one FEXCore engine's
    // CPUState to another's, for a WoW64 bitness gate crossing between context_ (64-bit) and
    // context32_ (32-bit).
    //
    // Deliberately does NOT copy the per-engine JIT bookkeeping fields that live interleaved in
    // CPUState - InlineJITBlockHeader, DeferredSignalRefCount, L1Pointer, L1Mask, callret_sp, _pad1,
    // segment_arrays, private_gdt. Each FEXCore thread owns its own call-ret shadow stack (_pad1/
    // callret_sp - see ensure_callret_stack), block-cache linkage (L1Pointer/L1Mask), and GDT
    // pointer (segment_arrays, set up by create_thread()/create_thread32()); copying them across
    // would point the destination engine's bookkeeping at the source engine's private buffers,
    // corrupting both engines' independent state the next time either runs (this is the exact design
    // pitfall the plan's "PHASE B, INCREMENT 5" section warns against - do NOT reach for
    // save_registers()/restore_registers(), which carry those fields forward on purpose).
    //
    // rip, the stack pointer (gregs[RSP]), and the CS/SS selectors are left untouched here: they are
    // set by the caller per the specific crossing convention (see perform_gate_crossing), which knows
    // the target entry point and mode. Everything else - all 16 GPRs (RAX..R15, including the
    // convention registers, matching the real heaven's-gate trampoline's iretq, which leaves the GPRs
    // intact), PF/AF, the whole XMM/AVX file, x87 MM registers, EFLAGS (flags[]), MXCSR, x87
    // FCW/AbridgedFTW, and the segment selectors/base caches - is genuine guest-visible architectural
    // state that must survive the crossing unchanged.
    inline void marshal_architectural_state(const FEXCore::Core::CPUState& src, FEXCore::Core::CPUState& dst)
    {
        std::memcpy(dst.gregs, src.gregs, sizeof(dst.gregs));

        dst.pf_raw = src.pf_raw;
        dst.af_raw = src.af_raw;

        std::memcpy(dst.avx_high, src.avx_high, sizeof(dst.avx_high));
        dst.xmm = src.xmm;
        std::memcpy(dst.mm, src.mm, sizeof(dst.mm));
        std::memcpy(dst.flags, src.flags, sizeof(dst.flags));

        dst.es_idx = src.es_idx;
        dst.ds_idx = src.ds_idx;
        dst.fs_idx = src.fs_idx;
        dst.gs_idx = src.gs_idx;

        dst.mxcsr = src.mxcsr;

        dst.es_cached = src.es_cached;
        dst.cs_cached = src.cs_cached;
        dst.ss_cached = src.ss_cached;
        dst.ds_cached = src.ds_cached;
        dst.gs_cached = src.gs_cached;
        dst.fs_cached = src.fs_cached;

        dst.FCW = src.FCW;
        dst.AbridgedFTW = src.AbridgedFTW;
    }
} // namespace sogen::fex
