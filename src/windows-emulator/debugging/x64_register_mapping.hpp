#pragma once
#include <cstddef>
#include <optional>
#include <x64_register.hpp>

struct register_entry
{
    x64_register reg;
    std::optional<size_t> expected_size;
    std::optional<size_t> offset;

    register_entry(const x64_register reg = x64_register::invalid,
                   const std::optional<size_t> expected_size = std::nullopt,
                   const std::optional<size_t> offset = std::nullopt)
        : reg(reg),
          expected_size(expected_size),
          offset(offset)
    {
    }
};

inline std::vector<register_entry> gdb_registers{
    x64_register::rax,
    x64_register::rbx,
    x64_register::rcx,
    x64_register::rdx,
    x64_register::rsi,
    x64_register::rdi,
    x64_register::rbp,
    x64_register::rsp,
    x64_register::r8,
    x64_register::r9,
    x64_register::r10,
    x64_register::r11,
    x64_register::r12,
    x64_register::r13,
    x64_register::r14,
    x64_register::r15,
    x64_register::rip,
    x64_register::eflags,

    {x64_register::cs, 4},
    {x64_register::ss, 4},
    {x64_register::ds, 4},
    {x64_register::es, 4},
    {x64_register::fs, 4},
    {x64_register::gs, 4},

    x64_register::st0,
    x64_register::st1,
    x64_register::st2,
    x64_register::st3,
    x64_register::st4,
    x64_register::st5,
    x64_register::st6,
    x64_register::st7,

    {x64_register::fpcw, 4},  // fctrl
    {x64_register::fpsw, 4},  // fstat
    {x64_register::fptag, 4}, // ftag
    {x64_register::fcs, 4},   // fiseg
    {x64_register::fip, 4},   // fioff
    {x64_register::fds, 4},   // foseg
    {x64_register::fdp, 4},   // fooff
    {x64_register::fop, 4},   // fop

    x64_register::xmm0,
    x64_register::xmm1,
    x64_register::xmm2,
    x64_register::xmm3,
    x64_register::xmm4,
    x64_register::xmm5,
    x64_register::xmm6,
    x64_register::xmm7,
    x64_register::xmm8,
    x64_register::xmm9,
    x64_register::xmm10,
    x64_register::xmm11,
    x64_register::xmm12,
    x64_register::xmm13,
    x64_register::xmm14,
    x64_register::xmm15,
    x64_register::mxcsr,
    x64_register::fs_base,
    x64_register::gs_base,
    {x64_register::ymm0, 16, 16},
    {x64_register::ymm1, 16, 16},
    {x64_register::ymm2, 16, 16},
    {x64_register::ymm3, 16, 16},
    {x64_register::ymm4, 16, 16},
    {x64_register::ymm5, 16, 16},
    {x64_register::ymm6, 16, 16},
    {x64_register::ymm7, 16, 16},
    {x64_register::ymm8, 16, 16},
    {x64_register::ymm9, 16, 16},
    {x64_register::ymm10, 16, 16},
    {x64_register::ymm11, 16, 16},
    {x64_register::ymm12, 16, 16},
    {x64_register::ymm13, 16, 16},
    {x64_register::ymm14, 16, 16},
    {x64_register::ymm15, 16, 16},
};
