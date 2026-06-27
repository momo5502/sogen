#pragma once

#include <cstdint>
#include <cstddef>
#include <optional>

#include <x86_register.hpp>

// Helpers that translate sogen's architectural register enum (x86_register) into the layout that
// FEXCore exposes through FEXCore::Core::CPUState. Kept header-only and free of FEX includes so it
// can be unit-reasoned about without the FEX toolchain present.
namespace sogen::fex::detail
{
    // FEXCore::Core::CPUState::gregs[] is indexed by the x86 hardware register encoding:
    //   0=RAX 1=RCX 2=RDX 3=RBX 4=RSP 5=RBP 6=RSI 7=RDI 8..15=R8..R15
    enum : int
    {
        greg_rax = 0,
        greg_rcx = 1,
        greg_rdx = 2,
        greg_rbx = 3,
        greg_rsp = 4,
        greg_rbp = 5,
        greg_rsi = 6,
        greg_rdi = 7,
        greg_r8 = 8,
        greg_count = 16,
        greg_invalid = -1,
    };

    enum class register_kind : uint8_t
    {
        unsupported,
        gpr,    // general purpose register (gregs[index], possibly a sub-register)
        rip,    // instruction pointer
        flags,  // (r/e)flags - reconstructed via the FEX context
        xmm,    // xmm0..xmm15 (128 bit, held in CPUState::xmm)
        mm,     // mm0..mm7 / st0..st7 mantissa (CPUState::mm)
        mxcsr,  //
        fcw,    // x87 control word
        fsw,    // x87 status word
        fs_base,
        gs_base,
        segment, // segment selector (cs/ds/es/fs/gs/ss index)
    };

    // Access description for a (sub-)register that lives inside a 64 bit greg slot.
    struct gpr_access
    {
        int index = greg_invalid; // index into gregs[]
        size_t byte_offset = 0;   // 0 for low part, 1 for the *h byte registers
        size_t width = 8;         // access width in bytes
        bool zero_extend_32 = false; // 32 bit writes clear the upper 32 bits (x86-64 semantics)
    };

    struct register_mapping
    {
        register_kind kind = register_kind::unsupported;
        gpr_access gpr{};
        int index = 0; // xmm/mm/segment index where applicable
    };

    inline std::optional<gpr_access> classify_gpr(const x86_register reg)
    {
        switch (reg)
        {
        // 64 bit
        case x86_register::rax: return gpr_access{greg_rax, 0, 8};
        case x86_register::rcx: return gpr_access{greg_rcx, 0, 8};
        case x86_register::rdx: return gpr_access{greg_rdx, 0, 8};
        case x86_register::rbx: return gpr_access{greg_rbx, 0, 8};
        case x86_register::rsp: return gpr_access{greg_rsp, 0, 8};
        case x86_register::rbp: return gpr_access{greg_rbp, 0, 8};
        case x86_register::rsi: return gpr_access{greg_rsi, 0, 8};
        case x86_register::rdi: return gpr_access{greg_rdi, 0, 8};
        case x86_register::r8: return gpr_access{greg_r8 + 0, 0, 8};
        case x86_register::r9: return gpr_access{greg_r8 + 1, 0, 8};
        case x86_register::r10: return gpr_access{greg_r8 + 2, 0, 8};
        case x86_register::r11: return gpr_access{greg_r8 + 3, 0, 8};
        case x86_register::r12: return gpr_access{greg_r8 + 4, 0, 8};
        case x86_register::r13: return gpr_access{greg_r8 + 5, 0, 8};
        case x86_register::r14: return gpr_access{greg_r8 + 6, 0, 8};
        case x86_register::r15: return gpr_access{greg_r8 + 7, 0, 8};

        // 32 bit (writes zero-extend to 64 bit)
        case x86_register::eax: return gpr_access{greg_rax, 0, 4, true};
        case x86_register::ecx: return gpr_access{greg_rcx, 0, 4, true};
        case x86_register::edx: return gpr_access{greg_rdx, 0, 4, true};
        case x86_register::ebx: return gpr_access{greg_rbx, 0, 4, true};
        case x86_register::esp: return gpr_access{greg_rsp, 0, 4, true};
        case x86_register::ebp: return gpr_access{greg_rbp, 0, 4, true};
        case x86_register::esi: return gpr_access{greg_rsi, 0, 4, true};
        case x86_register::edi: return gpr_access{greg_rdi, 0, 4, true};

        // 16 bit
        case x86_register::ax: return gpr_access{greg_rax, 0, 2};
        case x86_register::cx: return gpr_access{greg_rcx, 0, 2};
        case x86_register::dx: return gpr_access{greg_rdx, 0, 2};
        case x86_register::bx: return gpr_access{greg_rbx, 0, 2};
        case x86_register::sp: return gpr_access{greg_rsp, 0, 2};
        case x86_register::bp: return gpr_access{greg_rbp, 0, 2};
        case x86_register::si: return gpr_access{greg_rsi, 0, 2};
        case x86_register::di: return gpr_access{greg_rdi, 0, 2};

        // 8 bit low
        case x86_register::al: return gpr_access{greg_rax, 0, 1};
        case x86_register::cl: return gpr_access{greg_rcx, 0, 1};
        case x86_register::dl: return gpr_access{greg_rdx, 0, 1};
        case x86_register::bl: return gpr_access{greg_rbx, 0, 1};
        case x86_register::spl: return gpr_access{greg_rsp, 0, 1};
        case x86_register::bpl: return gpr_access{greg_rbp, 0, 1};
        case x86_register::sil: return gpr_access{greg_rsi, 0, 1};
        case x86_register::dil: return gpr_access{greg_rdi, 0, 1};

        // 8 bit high
        case x86_register::ah: return gpr_access{greg_rax, 1, 1};
        case x86_register::ch: return gpr_access{greg_rcx, 1, 1};
        case x86_register::dh: return gpr_access{greg_rdx, 1, 1};
        case x86_register::bh: return gpr_access{greg_rbx, 1, 1};

        default: return std::nullopt;
        }
    }

    inline int classify_segment(const x86_register reg)
    {
        // Selector index within CPUState (es,cs,ss,ds,fs,gs). Returned value is only used to pick the
        // matching *_idx field; base registers are handled separately via fs_base/gs_base.
        switch (reg)
        {
        case x86_register::es: return 0;
        case x86_register::cs: return 1;
        case x86_register::ss: return 2;
        case x86_register::ds: return 3;
        case x86_register::fs: return 4;
        case x86_register::gs: return 5;
        default: return -1;
        }
    }

    inline register_mapping map_register(const x86_register reg)
    {
        if (const auto gpr = classify_gpr(reg))
        {
            return register_mapping{.kind = register_kind::gpr, .gpr = *gpr};
        }

        switch (reg)
        {
        case x86_register::rip:
        case x86_register::eip:
        case x86_register::ip:
            return register_mapping{.kind = register_kind::rip};

        case x86_register::rflags:
        case x86_register::eflags:
        case x86_register::flags:
            return register_mapping{.kind = register_kind::flags};

        case x86_register::fs_base:
            return register_mapping{.kind = register_kind::fs_base};
        case x86_register::gs_base:
            return register_mapping{.kind = register_kind::gs_base};

        case x86_register::mxcsr:
            return register_mapping{.kind = register_kind::mxcsr};
        case x86_register::fpcw:
            return register_mapping{.kind = register_kind::fcw};
        case x86_register::fpsw:
            return register_mapping{.kind = register_kind::fsw};

        default:
            break;
        }

        if (reg >= x86_register::xmm0 && reg <= x86_register::xmm15)
        {
            return register_mapping{.kind = register_kind::xmm,
                                    .index = static_cast<int>(reg) - static_cast<int>(x86_register::xmm0)};
        }

        if (reg >= x86_register::mm0 && reg <= x86_register::mm7)
        {
            return register_mapping{.kind = register_kind::mm, .index = static_cast<int>(reg) - static_cast<int>(x86_register::mm0)};
        }

        if (const auto seg = classify_segment(reg); seg >= 0)
        {
            return register_mapping{.kind = register_kind::segment, .index = seg};
        }

        return register_mapping{.kind = register_kind::unsupported};
    }
} // namespace sogen::fex::detail
