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
    constexpr int greg_rax = 0;
    constexpr int greg_rcx = 1;
    constexpr int greg_rdx = 2;
    constexpr int greg_rbx = 3;
    constexpr int greg_rsp = 4;
    constexpr int greg_rbp = 5;
    constexpr int greg_rsi = 6;
    constexpr int greg_rdi = 7;
    constexpr int greg_r8 = 8;
    constexpr int greg_count = 16;
    constexpr int greg_invalid = -1;

    enum class register_kind : uint8_t
    {
        unsupported,
        gpr,   // general purpose register (gregs[index], possibly a sub-register)
        rip,   // instruction pointer
        flags, // (r/e)flags - reconstructed via the FEX context
        xmm,   // xmm0..xmm15 (128 bit, held in CPUState::xmm)
        mm,    // mm0..mm7 / st0..st7 mantissa (CPUState::mm)
        mxcsr,
        fcw, // x87 control word
        fsw, // x87 status word
        fs_base,
        gs_base,
        segment, // segment selector (cs/ds/es/fs/gs/ss index)
    };

    // Access description for a (sub-)register that lives inside a 64 bit greg slot.
    struct gpr_access
    {
        int index = greg_invalid;    // index into gregs[]
        size_t byte_offset = 0;      // 0 for low part, 1 for the *h byte registers
        size_t width = 8;            // access width in bytes
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
        case x86_register::rax:
            return gpr_access{.index = greg_rax, .byte_offset = 0, .width = 8};
        case x86_register::rcx:
            return gpr_access{.index = greg_rcx, .byte_offset = 0, .width = 8};
        case x86_register::rdx:
            return gpr_access{.index = greg_rdx, .byte_offset = 0, .width = 8};
        case x86_register::rbx:
            return gpr_access{.index = greg_rbx, .byte_offset = 0, .width = 8};
        case x86_register::rsp:
            return gpr_access{.index = greg_rsp, .byte_offset = 0, .width = 8};
        case x86_register::rbp:
            return gpr_access{.index = greg_rbp, .byte_offset = 0, .width = 8};
        case x86_register::rsi:
            return gpr_access{.index = greg_rsi, .byte_offset = 0, .width = 8};
        case x86_register::rdi:
            return gpr_access{.index = greg_rdi, .byte_offset = 0, .width = 8};
        case x86_register::r8:
            return gpr_access{.index = greg_r8 + 0, .byte_offset = 0, .width = 8};
        case x86_register::r9:
            return gpr_access{.index = greg_r8 + 1, .byte_offset = 0, .width = 8};
        case x86_register::r10:
            return gpr_access{.index = greg_r8 + 2, .byte_offset = 0, .width = 8};
        case x86_register::r11:
            return gpr_access{.index = greg_r8 + 3, .byte_offset = 0, .width = 8};
        case x86_register::r12:
            return gpr_access{.index = greg_r8 + 4, .byte_offset = 0, .width = 8};
        case x86_register::r13:
            return gpr_access{.index = greg_r8 + 5, .byte_offset = 0, .width = 8};
        case x86_register::r14:
            return gpr_access{.index = greg_r8 + 6, .byte_offset = 0, .width = 8};
        case x86_register::r15:
            return gpr_access{.index = greg_r8 + 7, .byte_offset = 0, .width = 8};

        // 32 bit (writes zero-extend to 64 bit)
        case x86_register::eax:
            return gpr_access{.index = greg_rax, .byte_offset = 0, .width = 4, .zero_extend_32 = true};
        case x86_register::ecx:
            return gpr_access{.index = greg_rcx, .byte_offset = 0, .width = 4, .zero_extend_32 = true};
        case x86_register::edx:
            return gpr_access{.index = greg_rdx, .byte_offset = 0, .width = 4, .zero_extend_32 = true};
        case x86_register::ebx:
            return gpr_access{.index = greg_rbx, .byte_offset = 0, .width = 4, .zero_extend_32 = true};
        case x86_register::esp:
            return gpr_access{.index = greg_rsp, .byte_offset = 0, .width = 4, .zero_extend_32 = true};
        case x86_register::ebp:
            return gpr_access{.index = greg_rbp, .byte_offset = 0, .width = 4, .zero_extend_32 = true};
        case x86_register::esi:
            return gpr_access{.index = greg_rsi, .byte_offset = 0, .width = 4, .zero_extend_32 = true};
        case x86_register::edi:
            return gpr_access{.index = greg_rdi, .byte_offset = 0, .width = 4, .zero_extend_32 = true};
        case x86_register::r8d:
            return gpr_access{.index = greg_r8 + 0, .byte_offset = 0, .width = 4, .zero_extend_32 = true};
        case x86_register::r9d:
            return gpr_access{.index = greg_r8 + 1, .byte_offset = 0, .width = 4, .zero_extend_32 = true};
        case x86_register::r10d:
            return gpr_access{.index = greg_r8 + 2, .byte_offset = 0, .width = 4, .zero_extend_32 = true};
        case x86_register::r11d:
            return gpr_access{.index = greg_r8 + 3, .byte_offset = 0, .width = 4, .zero_extend_32 = true};
        case x86_register::r12d:
            return gpr_access{.index = greg_r8 + 4, .byte_offset = 0, .width = 4, .zero_extend_32 = true};
        case x86_register::r13d:
            return gpr_access{.index = greg_r8 + 5, .byte_offset = 0, .width = 4, .zero_extend_32 = true};
        case x86_register::r14d:
            return gpr_access{.index = greg_r8 + 6, .byte_offset = 0, .width = 4, .zero_extend_32 = true};
        case x86_register::r15d:
            return gpr_access{.index = greg_r8 + 7, .byte_offset = 0, .width = 4, .zero_extend_32 = true};

        // 16 bit
        case x86_register::ax:
            return gpr_access{.index = greg_rax, .byte_offset = 0, .width = 2};
        case x86_register::cx:
            return gpr_access{.index = greg_rcx, .byte_offset = 0, .width = 2};
        case x86_register::dx:
            return gpr_access{.index = greg_rdx, .byte_offset = 0, .width = 2};
        case x86_register::bx:
            return gpr_access{.index = greg_rbx, .byte_offset = 0, .width = 2};
        case x86_register::sp:
            return gpr_access{.index = greg_rsp, .byte_offset = 0, .width = 2};
        case x86_register::bp:
            return gpr_access{.index = greg_rbp, .byte_offset = 0, .width = 2};
        case x86_register::si:
            return gpr_access{.index = greg_rsi, .byte_offset = 0, .width = 2};
        case x86_register::di:
            return gpr_access{.index = greg_rdi, .byte_offset = 0, .width = 2};
        case x86_register::r8w:
            return gpr_access{.index = greg_r8 + 0, .byte_offset = 0, .width = 2};
        case x86_register::r9w:
            return gpr_access{.index = greg_r8 + 1, .byte_offset = 0, .width = 2};
        case x86_register::r10w:
            return gpr_access{.index = greg_r8 + 2, .byte_offset = 0, .width = 2};
        case x86_register::r11w:
            return gpr_access{.index = greg_r8 + 3, .byte_offset = 0, .width = 2};
        case x86_register::r12w:
            return gpr_access{.index = greg_r8 + 4, .byte_offset = 0, .width = 2};
        case x86_register::r13w:
            return gpr_access{.index = greg_r8 + 5, .byte_offset = 0, .width = 2};
        case x86_register::r14w:
            return gpr_access{.index = greg_r8 + 6, .byte_offset = 0, .width = 2};
        case x86_register::r15w:
            return gpr_access{.index = greg_r8 + 7, .byte_offset = 0, .width = 2};

        // 8 bit low
        case x86_register::al:
            return gpr_access{.index = greg_rax, .byte_offset = 0, .width = 1};
        case x86_register::cl:
            return gpr_access{.index = greg_rcx, .byte_offset = 0, .width = 1};
        case x86_register::dl:
            return gpr_access{.index = greg_rdx, .byte_offset = 0, .width = 1};
        case x86_register::bl:
            return gpr_access{.index = greg_rbx, .byte_offset = 0, .width = 1};
        case x86_register::spl:
            return gpr_access{.index = greg_rsp, .byte_offset = 0, .width = 1};
        case x86_register::bpl:
            return gpr_access{.index = greg_rbp, .byte_offset = 0, .width = 1};
        case x86_register::sil:
            return gpr_access{.index = greg_rsi, .byte_offset = 0, .width = 1};
        case x86_register::dil:
            return gpr_access{.index = greg_rdi, .byte_offset = 0, .width = 1};
        case x86_register::r8b:
            return gpr_access{.index = greg_r8 + 0, .byte_offset = 0, .width = 1};
        case x86_register::r9b:
            return gpr_access{.index = greg_r8 + 1, .byte_offset = 0, .width = 1};
        case x86_register::r10b:
            return gpr_access{.index = greg_r8 + 2, .byte_offset = 0, .width = 1};
        case x86_register::r11b:
            return gpr_access{.index = greg_r8 + 3, .byte_offset = 0, .width = 1};
        case x86_register::r12b:
            return gpr_access{.index = greg_r8 + 4, .byte_offset = 0, .width = 1};
        case x86_register::r13b:
            return gpr_access{.index = greg_r8 + 5, .byte_offset = 0, .width = 1};
        case x86_register::r14b:
            return gpr_access{.index = greg_r8 + 6, .byte_offset = 0, .width = 1};
        case x86_register::r15b:
            return gpr_access{.index = greg_r8 + 7, .byte_offset = 0, .width = 1};

        // 8 bit high
        case x86_register::ah:
            return gpr_access{.index = greg_rax, .byte_offset = 1, .width = 1};
        case x86_register::ch:
            return gpr_access{.index = greg_rcx, .byte_offset = 1, .width = 1};
        case x86_register::dh:
            return gpr_access{.index = greg_rdx, .byte_offset = 1, .width = 1};
        case x86_register::bh:
            return gpr_access{.index = greg_rbx, .byte_offset = 1, .width = 1};

        default:
            return std::nullopt;
        }
    }

    inline int classify_segment(const x86_register reg)
    {
        // Selector index within CPUState (es,cs,ss,ds,fs,gs). Returned value is only used to pick the
        // matching *_idx field; base registers are handled separately via fs_base/gs_base.
        switch (reg)
        {
        case x86_register::es:
            return 0;
        case x86_register::cs:
            return 1;
        case x86_register::ss:
            return 2;
        case x86_register::ds:
            return 3;
        case x86_register::fs:
            return 4;
        case x86_register::gs:
            return 5;
        default:
            return -1;
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
            return register_mapping{.kind = register_kind::xmm, .index = static_cast<int>(reg) - static_cast<int>(x86_register::xmm0)};
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
