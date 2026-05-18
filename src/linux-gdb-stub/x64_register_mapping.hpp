#pragma once
#include <cstddef>
#include <optional>
#include <x86_register.hpp>

struct register_entry
{
    x86_register reg;
    std::optional<size_t> expected_size;
    std::optional<size_t> offset;

    register_entry(const x86_register reg = x86_register::invalid, const std::optional<size_t> expected_size = std::nullopt,
                   const std::optional<size_t> offset = std::nullopt)
        : reg(reg),
          expected_size(expected_size),
          offset(offset)
    {
    }
};

inline std::vector<register_entry> gdb_registers{
    x86_register::rax,
    x86_register::rbx,
    x86_register::rcx,
    x86_register::rdx,
    x86_register::rsi,
    x86_register::rdi,
    x86_register::rbp,
    x86_register::rsp,
    x86_register::r8,
    x86_register::r9,
    x86_register::r10,
    x86_register::r11,
    x86_register::r12,
    x86_register::r13,
    x86_register::r14,
    x86_register::r15,
    x86_register::rip,
    x86_register::eflags,

    {x86_register::cs, 4},
    {x86_register::ss, 4},
    {x86_register::ds, 4},
    {x86_register::es, 4},
    {x86_register::fs, 4},
    {x86_register::gs, 4},

    x86_register::st0,
    x86_register::st1,
    x86_register::st2,
    x86_register::st3,
    x86_register::st4,
    x86_register::st5,
    x86_register::st6,
    x86_register::st7,

    {x86_register::fpcw, 4},  // fctrl
    {x86_register::fpsw, 4},  // fstat
    {x86_register::fptag, 4}, // ftag
    {x86_register::fcs, 4},   // fiseg
    {x86_register::fip, 4},   // fioff
    {x86_register::fds, 4},   // foseg
    {x86_register::fdp, 4},   // fooff
    {x86_register::fop, 4},   // fop

    x86_register::xmm0,
    x86_register::xmm1,
    x86_register::xmm2,
    x86_register::xmm3,
    x86_register::xmm4,
    x86_register::xmm5,
    x86_register::xmm6,
    x86_register::xmm7,
    x86_register::xmm8,
    x86_register::xmm9,
    x86_register::xmm10,
    x86_register::xmm11,
    x86_register::xmm12,
    x86_register::xmm13,
    x86_register::xmm14,
    x86_register::xmm15,
    x86_register::mxcsr,
    x86_register::fs_base,
    x86_register::gs_base,
    {x86_register::ymm0, 16, 16},
    {x86_register::ymm1, 16, 16},
    {x86_register::ymm2, 16, 16},
    {x86_register::ymm3, 16, 16},
    {x86_register::ymm4, 16, 16},
    {x86_register::ymm5, 16, 16},
    {x86_register::ymm6, 16, 16},
    {x86_register::ymm7, 16, 16},
    {x86_register::ymm8, 16, 16},
    {x86_register::ymm9, 16, 16},
    {x86_register::ymm10, 16, 16},
    {x86_register::ymm11, 16, 16},
    {x86_register::ymm12, 16, 16},
    {x86_register::ymm13, 16, 16},
    {x86_register::ymm14, 16, 16},
    {x86_register::ymm15, 16, 16},
};
