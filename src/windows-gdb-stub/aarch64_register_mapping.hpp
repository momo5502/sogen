#pragma once
#include "register_entry.hpp"
#include <array>

namespace sogen
{

    // Numbering of aarch64_target_descriptions: x0-x30, sp, pc, cpsr, v0-v31, fpsr, fpcr.
    inline constexpr auto aarch64_gdb_registers = std::to_array<register_entry>({
        arm64_register::x0,  arm64_register::x1,        arm64_register::x2,        arm64_register::x3,          arm64_register::x4,
        arm64_register::x5,  arm64_register::x6,        arm64_register::x7,        arm64_register::x8,          arm64_register::x9,
        arm64_register::x10, arm64_register::x11,       arm64_register::x12,       arm64_register::x13,         arm64_register::x14,
        arm64_register::x15, arm64_register::x16,       arm64_register::x17,       arm64_register::x18,         arm64_register::x19,
        arm64_register::x20, arm64_register::x21,       arm64_register::x22,       arm64_register::x23,         arm64_register::x24,
        arm64_register::x25, arm64_register::x26,       arm64_register::x27,       arm64_register::x28,         arm64_register::x29,
        arm64_register::x30, arm64_register::sp,        arm64_register::pc,        {arm64_register::pstate, 4}, arm64_register::q0,
        arm64_register::q1,  arm64_register::q2,        arm64_register::q3,        arm64_register::q4,          arm64_register::q5,
        arm64_register::q6,  arm64_register::q7,        arm64_register::q8,        arm64_register::q9,          arm64_register::q10,
        arm64_register::q11, arm64_register::q12,       arm64_register::q13,       arm64_register::q14,         arm64_register::q15,
        arm64_register::q16, arm64_register::q17,       arm64_register::q18,       arm64_register::q19,         arm64_register::q20,
        arm64_register::q21, arm64_register::q22,       arm64_register::q23,       arm64_register::q24,         arm64_register::q25,
        arm64_register::q26, arm64_register::q27,       arm64_register::q28,       arm64_register::q29,         arm64_register::q30,
        arm64_register::q31, {arm64_register::fpsr, 4}, {arm64_register::fpcr, 4},
    });

} // namespace sogen
