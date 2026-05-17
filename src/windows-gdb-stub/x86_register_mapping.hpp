#pragma once
#include "register_entry.hpp"
#include <vector>

inline std::vector<register_entry> x86_gdb_registers{
    x86_register::eax,        x86_register::ecx,     x86_register::edx,     x86_register::ebx,     x86_register::esp,
    x86_register::ebp,        x86_register::esi,     x86_register::edi,     x86_register::eip,     x86_register::eflags,

    {x86_register::cs, 4},    {x86_register::ss, 4}, {x86_register::ds, 4}, {x86_register::es, 4}, {x86_register::fs, 4},
    {x86_register::gs, 4},

    x86_register::st0,        x86_register::st1,     x86_register::st2,     x86_register::st3,     x86_register::st4,
    x86_register::st5,        x86_register::st6,     x86_register::st7,

    {x86_register::fpcw, 4},  // fctrl
    {x86_register::fpsw, 4},  // fstat
    {x86_register::fptag, 4}, // ftag
    {x86_register::fcs, 4},   // fiseg
    {x86_register::fip, 4},   // fioff
    {x86_register::fds, 4},   // foseg
    {x86_register::fdp, 4},   // fooff
    {x86_register::fop, 4},   // fop

    x86_register::xmm0,       x86_register::xmm1,    x86_register::xmm2,    x86_register::xmm3,    x86_register::xmm4,
    x86_register::xmm5,       x86_register::xmm6,    x86_register::xmm7,    x86_register::mxcsr,   x86_register::fs_base,
    x86_register::gs_base,
};
