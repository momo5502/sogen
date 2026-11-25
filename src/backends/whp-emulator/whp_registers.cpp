#include "whp_registers.hpp"

#include <stdexcept>

namespace sogen::whp
{
    std::optional<gp_register_access> classify_gp_register_access(const x86_register reg)
    {
        switch (reg)
        {
        case x86_register::al:
        case x86_register::bl:
        case x86_register::cl:
        case x86_register::dl:
        case x86_register::sil:
        case x86_register::dil:
        case x86_register::bpl:
        case x86_register::spl:
        case x86_register::r8b:
        case x86_register::r9b:
        case x86_register::r10b:
        case x86_register::r11b:
        case x86_register::r12b:
        case x86_register::r13b:
        case x86_register::r14b:
        case x86_register::r15b:
            return gp_register_access{.offset = 0, .width = sizeof(uint8_t)};
        case x86_register::ah:
        case x86_register::bh:
        case x86_register::ch:
        case x86_register::dh:
            return gp_register_access{.offset = 1, .width = sizeof(uint8_t)};
        case x86_register::ax:
        case x86_register::bx:
        case x86_register::cx:
        case x86_register::dx:
        case x86_register::si:
        case x86_register::di:
        case x86_register::bp:
        case x86_register::sp:
        case x86_register::r8w:
        case x86_register::r9w:
        case x86_register::r10w:
        case x86_register::r11w:
        case x86_register::r12w:
        case x86_register::r13w:
        case x86_register::r14w:
        case x86_register::r15w:
        case x86_register::ip:
        case x86_register::flags:
            return gp_register_access{.offset = 0, .width = sizeof(uint16_t)};
        case x86_register::eax:
        case x86_register::ebx:
        case x86_register::ecx:
        case x86_register::edx:
        case x86_register::esi:
        case x86_register::edi:
        case x86_register::ebp:
        case x86_register::esp:
        case x86_register::r8d:
        case x86_register::r9d:
        case x86_register::r10d:
        case x86_register::r11d:
        case x86_register::r12d:
        case x86_register::r13d:
        case x86_register::r14d:
        case x86_register::r15d:
        case x86_register::eip:
        case x86_register::eflags:
            return gp_register_access{.offset = 0, .width = sizeof(uint32_t), .zero_extend_32 = true};
        case x86_register::rax:
        case x86_register::rbx:
        case x86_register::rcx:
        case x86_register::rdx:
        case x86_register::rsi:
        case x86_register::rdi:
        case x86_register::rbp:
        case x86_register::rsp:
        case x86_register::r8:
        case x86_register::r9:
        case x86_register::r10:
        case x86_register::r11:
        case x86_register::r12:
        case x86_register::r13:
        case x86_register::r14:
        case x86_register::r15:
        case x86_register::rip:
        case x86_register::rflags:
            return gp_register_access{.offset = 0, .width = sizeof(uint64_t)};
        default:
            return std::nullopt;
        }
    }

    WHV_X64_SEGMENT_REGISTER make_segment(const uint16_t selector, const bool is_code)
    {
        WHV_X64_SEGMENT_REGISTER segment{};
        segment.Base = 0;
        segment.Limit = 0xFFFFF;
        segment.Selector = selector;
        segment.SegmentType = is_code ? 0xB : 0x3;
        segment.NonSystemSegment = 1;
        segment.DescriptorPrivilegeLevel = selector == 0x33 || selector == 0x2B || selector == 0x53 ? 3 : 0;
        segment.Present = 1;
        segment.Long = is_code ? 1 : 0;
        segment.Default = is_code ? 0 : 1;
        segment.Granularity = 1;
        return segment;
    }

    register_mapping map_register(const x86_register reg)
    {
        switch (reg)
        {
        case x86_register::al:
        case x86_register::ah:
        case x86_register::ax:
        case x86_register::eax:
        case x86_register::rax:
            return {
                .name = WHvX64RegisterRax,
                .kind = register_kind::reg64,
                .logical_size = sizeof(uint64_t),
            };
        case x86_register::bl:
        case x86_register::bh:
        case x86_register::bx:
        case x86_register::ebx:
        case x86_register::rbx:
            return {
                .name = WHvX64RegisterRbx,
                .kind = register_kind::reg64,
                .logical_size = sizeof(uint64_t),
            };
        case x86_register::cl:
        case x86_register::ch:
        case x86_register::cx:
        case x86_register::ecx:
        case x86_register::rcx:
            return {
                .name = WHvX64RegisterRcx,
                .kind = register_kind::reg64,
                .logical_size = sizeof(uint64_t),
            };
        case x86_register::dl:
        case x86_register::dh:
        case x86_register::dx:
        case x86_register::edx:
        case x86_register::rdx:
            return {
                .name = WHvX64RegisterRdx,
                .kind = register_kind::reg64,
                .logical_size = sizeof(uint64_t),
            };
        case x86_register::si:
        case x86_register::sil:
        case x86_register::esi:
        case x86_register::rsi:
            return {
                .name = WHvX64RegisterRsi,
                .kind = register_kind::reg64,
                .logical_size = sizeof(uint64_t),
            };
        case x86_register::di:
        case x86_register::dil:
        case x86_register::edi:
        case x86_register::rdi:
            return {
                .name = WHvX64RegisterRdi,
                .kind = register_kind::reg64,
                .logical_size = sizeof(uint64_t),
            };
        case x86_register::bp:
        case x86_register::bpl:
        case x86_register::ebp:
        case x86_register::rbp:
            return {
                .name = WHvX64RegisterRbp,
                .kind = register_kind::reg64,
                .logical_size = sizeof(uint64_t),
            };
        case x86_register::sp:
        case x86_register::spl:
        case x86_register::esp:
        case x86_register::rsp:
            return {
                .name = WHvX64RegisterRsp,
                .kind = register_kind::reg64,
                .logical_size = sizeof(uint64_t),
            };
        case x86_register::ip:
        case x86_register::eip:
        case x86_register::rip:
            return {
                .name = WHvX64RegisterRip,
                .kind = register_kind::reg64,
                .logical_size = sizeof(uint64_t),
            };
        case x86_register::r8:
        case x86_register::r8d:
        case x86_register::r8w:
        case x86_register::r8b:
            return {
                .name = WHvX64RegisterR8,
                .kind = register_kind::reg64,
                .logical_size = sizeof(uint64_t),
            };
        case x86_register::r9:
        case x86_register::r9d:
        case x86_register::r9w:
        case x86_register::r9b:
            return {
                .name = WHvX64RegisterR9,
                .kind = register_kind::reg64,
                .logical_size = sizeof(uint64_t),
            };
        case x86_register::r10:
        case x86_register::r10d:
        case x86_register::r10w:
        case x86_register::r10b:
            return {
                .name = WHvX64RegisterR10,
                .kind = register_kind::reg64,
                .logical_size = sizeof(uint64_t),
            };
        case x86_register::r11:
        case x86_register::r11d:
        case x86_register::r11w:
        case x86_register::r11b:
            return {
                .name = WHvX64RegisterR11,
                .kind = register_kind::reg64,
                .logical_size = sizeof(uint64_t),
            };
        case x86_register::r12:
        case x86_register::r12d:
        case x86_register::r12w:
        case x86_register::r12b:
            return {
                .name = WHvX64RegisterR12,
                .kind = register_kind::reg64,
                .logical_size = sizeof(uint64_t),
            };
        case x86_register::r13:
        case x86_register::r13d:
        case x86_register::r13w:
        case x86_register::r13b:
            return {
                .name = WHvX64RegisterR13,
                .kind = register_kind::reg64,
                .logical_size = sizeof(uint64_t),
            };
        case x86_register::r14:
        case x86_register::r14d:
        case x86_register::r14w:
        case x86_register::r14b:
            return {
                .name = WHvX64RegisterR14,
                .kind = register_kind::reg64,
                .logical_size = sizeof(uint64_t),
            };
        case x86_register::r15:
        case x86_register::r15d:
        case x86_register::r15w:
        case x86_register::r15b:
            return {
                .name = WHvX64RegisterR15,
                .kind = register_kind::reg64,
                .logical_size = sizeof(uint64_t),
            };
        case x86_register::flags:
            return {
                .name = WHvX64RegisterRflags,
                .kind = register_kind::reg64,
                .logical_size = sizeof(uint16_t),
            };
        case x86_register::eflags:
            return {
                .name = WHvX64RegisterRflags,
                .kind = register_kind::reg64,
                .logical_size = sizeof(uint32_t),
            };
        case x86_register::rflags:
            return {
                .name = WHvX64RegisterRflags,
                .kind = register_kind::reg64,
                .logical_size = sizeof(uint64_t),
            };
        case x86_register::cs:
            return {
                .name = WHvX64RegisterCs,
                .kind = register_kind::segment,
                .logical_size = sizeof(uint16_t),
            };
        case x86_register::ss:
            return {
                .name = WHvX64RegisterSs,
                .kind = register_kind::segment,
                .logical_size = sizeof(uint16_t),
            };
        case x86_register::ds:
            return {
                .name = WHvX64RegisterDs,
                .kind = register_kind::segment,
                .logical_size = sizeof(uint16_t),
            };
        case x86_register::es:
            return {
                .name = WHvX64RegisterEs,
                .kind = register_kind::segment,
                .logical_size = sizeof(uint16_t),
            };
        case x86_register::fs:
            return {
                .name = WHvX64RegisterFs,
                .kind = register_kind::segment,
                .logical_size = sizeof(uint16_t),
            };
        case x86_register::fs_base:
            return {
                .name = WHvX64RegisterFs,
                .kind = register_kind::segment,
                .logical_size = sizeof(uint64_t),
            };
        case x86_register::gs:
            return {
                .name = WHvX64RegisterGs,
                .kind = register_kind::segment,
                .logical_size = sizeof(uint16_t),
            };
        case x86_register::gs_base:
            return {
                .name = WHvX64RegisterGs,
                .kind = register_kind::segment,
                .logical_size = sizeof(uint64_t),
            };
        case x86_register::gdtr:
            return {
                .name = WHvX64RegisterGdtr,
                .kind = register_kind::table,
                .logical_size = sizeof(WHV_X64_TABLE_REGISTER),
            };
        case x86_register::idtr:
            return {
                .name = WHvX64RegisterIdtr,
                .kind = register_kind::table,
                .logical_size = sizeof(WHV_X64_TABLE_REGISTER),
            };
        case x86_register::cr0:
            return {
                .name = WHvX64RegisterCr0,
                .kind = register_kind::reg64,
                .logical_size = sizeof(uint64_t),
            };
        case x86_register::cr2:
            return {
                .name = WHvX64RegisterCr2,
                .kind = register_kind::reg64,
                .logical_size = sizeof(uint64_t),
            };
        case x86_register::cr3:
            return {
                .name = WHvX64RegisterCr3,
                .kind = register_kind::reg64,
                .logical_size = sizeof(uint64_t),
            };
        case x86_register::cr4:
            return {
                .name = WHvX64RegisterCr4,
                .kind = register_kind::reg64,
                .logical_size = sizeof(uint64_t),
            };
        case x86_register::dr0:
            return {
                .name = WHvX64RegisterDr0,
                .kind = register_kind::reg64,
                .logical_size = sizeof(uint64_t),
            };
        case x86_register::dr1:
            return {
                .name = WHvX64RegisterDr1,
                .kind = register_kind::reg64,
                .logical_size = sizeof(uint64_t),
            };
        case x86_register::dr2:
            return {
                .name = WHvX64RegisterDr2,
                .kind = register_kind::reg64,
                .logical_size = sizeof(uint64_t),
            };
        case x86_register::dr3:
            return {
                .name = WHvX64RegisterDr3,
                .kind = register_kind::reg64,
                .logical_size = sizeof(uint64_t),
            };
        case x86_register::dr6:
            return {
                .name = WHvX64RegisterDr6,
                .kind = register_kind::reg64,
                .logical_size = sizeof(uint64_t),
            };
        case x86_register::dr7:
            return {
                .name = WHvX64RegisterDr7,
                .kind = register_kind::reg64,
                .logical_size = sizeof(uint64_t),
            };
        case x86_register::fpcw:
        case x86_register::fpsw:
        case x86_register::fptag:
            return {
                .name = WHvX64RegisterFpControlStatus,
                .kind = register_kind::fp_control,
                .logical_size = sizeof(uint16_t),
            };
        case x86_register::fip:
        case x86_register::fcs:
        case x86_register::fop:
            return {
                .name = WHvX64RegisterFpControlStatus,
                .kind = register_kind::fp_last_instruction,
                .logical_size = sizeof(uint32_t),
            };
        case x86_register::fdp:
        case x86_register::fds:
            return {
                .name = WHvX64RegisterXmmControlStatus,
                .kind = register_kind::fp_last_data,
                .logical_size = sizeof(uint32_t),
            };
        case x86_register::mxcsr:
            return {
                .name = WHvX64RegisterXmmControlStatus,
                .kind = register_kind::xmm_control,
                .logical_size = sizeof(uint32_t),
            };
        case x86_register::msr:
            return {
                .name = WHvX64RegisterEfer,
                .kind = register_kind::reg64,
                .logical_size = sizeof(uint64_t),
            };
        default:
            break;
        }

        if (reg >= x86_register::xmm0 && reg <= x86_register::xmm15)
        {
            const auto index = static_cast<int>(reg) - static_cast<int>(x86_register::xmm0);
            return {
                .name = static_cast<WHV_REGISTER_NAME>(WHvX64RegisterXmm0 + index),
                .kind = register_kind::reg128,
                .logical_size = sizeof(WHV_UINT128),
            };
        }

        // WHP's AVX register layout does not match the debugger-facing YMM register
        // layout. Until we translate it explicitly, expose YMM as synthetic zero-filled
        // state rather than returning malformed data.
        if (reg >= x86_register::ymm0 && reg <= x86_register::ymm15)
        {
            return {
                .name = {}, // no WHV_REGISTER_NAME exists for this
                .kind = register_kind::zero,
                .logical_size = 32,
            };
        }

        if (reg >= x86_register::st0 && reg <= x86_register::st7)
        {
            const auto index = static_cast<int>(reg) - static_cast<int>(x86_register::st0);
            return {
                .name = static_cast<WHV_REGISTER_NAME>(WHvX64RegisterFpMmx0 + index),
                .kind = register_kind::fp,
                .logical_size = 10,
            };
        }

        throw std::runtime_error("Unsupported WHP register");
    }
}
