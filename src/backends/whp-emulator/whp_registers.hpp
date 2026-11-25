#pragma once

#include <windows.h>
#include <WinHvPlatform.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include <x86_register.hpp>

namespace sogen::whp
{
    enum class register_kind
    {
        reg64,
        segment,
        table,
        fp,
        fp_control,
        fp_last_instruction,
        fp_last_data,
        xmm_control,
        zero,
        reg128,
    };

    struct register_mapping
    {
        WHV_REGISTER_NAME name{};
        register_kind kind = register_kind::reg64;
        size_t logical_size = sizeof(uint64_t);
    };

    struct gp_register_access
    {
        size_t offset = 0;
        size_t width = sizeof(uint64_t);
        bool zero_extend_32 = false;
    };

    std::optional<gp_register_access> classify_gp_register_access(x86_register reg);
    WHV_X64_SEGMENT_REGISTER make_segment(uint16_t selector, bool is_code);
    register_mapping map_register(x86_register reg);

    constexpr std::array<WHV_REGISTER_NAME, 64> snapshot_register_names()
    {
        return std::array<WHV_REGISTER_NAME, 64>{{
            WHvX64RegisterRax,
            WHvX64RegisterRbx,
            WHvX64RegisterRcx,
            WHvX64RegisterRdx,
            WHvX64RegisterRsi,
            WHvX64RegisterRdi,
            WHvX64RegisterRbp,
            WHvX64RegisterRsp,
            WHvX64RegisterR8,
            WHvX64RegisterR9,
            WHvX64RegisterR10,
            WHvX64RegisterR11,
            WHvX64RegisterR12,
            WHvX64RegisterR13,
            WHvX64RegisterR14,
            WHvX64RegisterR15,
            WHvX64RegisterRip,
            WHvX64RegisterRflags,
            WHvX64RegisterCs,
            WHvX64RegisterSs,
            WHvX64RegisterDs,
            WHvX64RegisterEs,
            WHvX64RegisterFs,
            WHvX64RegisterGs,
            WHvX64RegisterCr0,
            WHvX64RegisterCr2,
            WHvX64RegisterCr3,
            WHvX64RegisterCr4,
            WHvX64RegisterDr0,
            WHvX64RegisterDr1,
            WHvX64RegisterDr2,
            WHvX64RegisterDr3,
            WHvX64RegisterDr6,
            WHvX64RegisterDr7,
            WHvX64RegisterGdtr,
            WHvX64RegisterIdtr,
            WHvX64RegisterEfer,
            WHvX64RegisterXCr0,
            WHvX64RegisterFpControlStatus,
            WHvX64RegisterXmmControlStatus,
            WHvX64RegisterFpMmx0,
            WHvX64RegisterFpMmx1,
            WHvX64RegisterFpMmx2,
            WHvX64RegisterFpMmx3,
            WHvX64RegisterFpMmx4,
            WHvX64RegisterFpMmx5,
            WHvX64RegisterFpMmx6,
            WHvX64RegisterFpMmx7,
            WHvX64RegisterXmm0,
            WHvX64RegisterXmm1,
            WHvX64RegisterXmm2,
            WHvX64RegisterXmm3,
            WHvX64RegisterXmm4,
            WHvX64RegisterXmm5,
            WHvX64RegisterXmm6,
            WHvX64RegisterXmm7,
            WHvX64RegisterXmm8,
            WHvX64RegisterXmm9,
            WHvX64RegisterXmm10,
            WHvX64RegisterXmm11,
            WHvX64RegisterXmm12,
            WHvX64RegisterXmm13,
            WHvX64RegisterXmm14,
            WHvX64RegisterXmm15,
        }};
    }

    // x86_64_cpu::floating_point_state's registers: the x87 and SSE control registers, then ST0-ST7 and XMM0-XMM15.
    constexpr std::array<WHV_REGISTER_NAME, 2> floating_point_control_names{{
        WHvX64RegisterFpControlStatus,
        WHvX64RegisterXmmControlStatus,
    }};
    constexpr size_t floating_point_first_st = floating_point_control_names.size();
    constexpr size_t floating_point_first_xmm = floating_point_first_st + 8;

    constexpr std::array<WHV_REGISTER_NAME, 26> floating_point_register_names()
    {
        return std::array<WHV_REGISTER_NAME, 26>{{
            WHvX64RegisterFpControlStatus,
            WHvX64RegisterXmmControlStatus,
            WHvX64RegisterFpMmx0,
            WHvX64RegisterFpMmx1,
            WHvX64RegisterFpMmx2,
            WHvX64RegisterFpMmx3,
            WHvX64RegisterFpMmx4,
            WHvX64RegisterFpMmx5,
            WHvX64RegisterFpMmx6,
            WHvX64RegisterFpMmx7,
            WHvX64RegisterXmm0,
            WHvX64RegisterXmm1,
            WHvX64RegisterXmm2,
            WHvX64RegisterXmm3,
            WHvX64RegisterXmm4,
            WHvX64RegisterXmm5,
            WHvX64RegisterXmm6,
            WHvX64RegisterXmm7,
            WHvX64RegisterXmm8,
            WHvX64RegisterXmm9,
            WHvX64RegisterXmm10,
            WHvX64RegisterXmm11,
            WHvX64RegisterXmm12,
            WHvX64RegisterXmm13,
            WHvX64RegisterXmm14,
            WHvX64RegisterXmm15,
        }};
    }

    // The MXCSR mask the backend writes with every MXCSR.
    constexpr UINT32 written_mxcsr_mask = 0xFFFFFFFF;
}
