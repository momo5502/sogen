#pragma once

#include "cpu_interface.hpp"
#include "x86_register.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

namespace sogen::x87
{
    // The x87 tag word. FSAVE's full form gives each physical register two bits; FXSAVE's abridged form, the one every
    // backend gives, gives it one bit, set while the register holds a value.
    constexpr int register_count = 8;
    constexpr int full_tag_bits = 2;
    constexpr uint16_t full_tag_valid = 0b00;
    constexpr uint16_t full_tag_zero = 0b01;
    constexpr uint16_t full_tag_special = 0b10;
    constexpr uint16_t full_tag_empty = 0b11;

    // FSW.TOP, the physical register that holds ST(0).
    constexpr int status_top_shift = 11;

    // An 80-bit register: a 64-bit significand whose top bit is the integer (J) bit, then the sign and a 15-bit exponent.
    using register_value = std::array<uint8_t, 10>;
    constexpr uint64_t integer_bit = 1ULL << 63;
    constexpr uint16_t exponent_mask = 0x7FFF;

    // FXSAVE keeps a 1 for a valid, zero or special tag and a 0 for an empty one.
    inline uint16_t abridged_tag(const uint16_t full_tag)
    {
        uint16_t abridged{};
        for (int i = 0; i < register_count; ++i)
        {
            if (((full_tag >> (i * full_tag_bits)) & full_tag_empty) != full_tag_empty)
            {
                abridged |= static_cast<uint16_t>(1u << i);
            }
        }

        return abridged;
    }

    // The full tag of a register in use, from its value, as the SDM's table for recreating FSAVE's format gives it.
    inline uint16_t full_tag_of(const register_value& value)
    {
        uint64_t significand{};
        uint16_t sign_and_exponent{};
        memcpy(&significand, value.data(), sizeof(significand));
        memcpy(&sign_and_exponent, value.data() + sizeof(significand), sizeof(sign_and_exponent));

        const auto exponent = sign_and_exponent & exponent_mask;
        if (exponent == 0 && significand == 0)
        {
            return full_tag_zero;
        }

        if (exponent == 0 || exponent == exponent_mask || (significand & integer_bit) == 0)
        {
            return full_tag_special;
        }

        return full_tag_valid;
    }

    // FSAVE's form of an abridged tag word, from the processor's registers. The abridged form is in physical register
    // order, so physical register i is classified from ST((i - TOP) mod 8).
    inline uint16_t full_tag(cpu_interface& cpu, const uint16_t abridged)
    {
        uint16_t status{};
        cpu.read_raw_register(static_cast<int>(x86_register::fpsw), &status, sizeof(status));
        const auto top = (status >> status_top_shift) % register_count;

        uint16_t full{};
        for (int i = 0; i < register_count; ++i)
        {
            auto tag = full_tag_empty;
            if ((abridged & (1u << i)) != 0)
            {
                register_value stack_register{};
                const auto stack_index = (i + register_count - top) % register_count;
                cpu.read_raw_register(static_cast<int>(x86_register::st0) + stack_index, stack_register.data(), stack_register.size());
                tag = full_tag_of(stack_register);
            }

            full |= static_cast<uint16_t>(tag << (i * full_tag_bits));
        }

        return full;
    }

    // Reads the tag word in FSAVE's full form.
    inline size_t read_full_tag(cpu_interface& cpu, void* value, const size_t size)
    {
        uint16_t abridged{};
        cpu.read_raw_register(static_cast<int>(x86_register::fptag), &abridged, sizeof(abridged));
        const auto full = full_tag(cpu, abridged);

        const auto result_size = (std::min)(size, sizeof(full));
        memcpy(value, &full, result_size);
        return result_size;
    }

    // Writes a tag word given in FSAVE's full form.
    inline size_t write_full_tag(cpu_interface& cpu, const void* value, const size_t size)
    {
        uint16_t full{};
        memcpy(&full, value, (std::min)(size, sizeof(full)));
        const auto abridged = abridged_tag(full);
        cpu.write_raw_register(static_cast<int>(x86_register::fptag), &abridged, sizeof(abridged));
        return size;
    }
}
