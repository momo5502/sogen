#pragma once

#include <optional>
#include <cstdint>

#include "emulator.hpp"
#include "x86_register.hpp"

namespace segment_utils
{
    enum class segment_bitness
    {
        bit16 = 16,
        bit32 = 32,
        bit64 = 64,
    };

#pragma pack(push, 1)
    struct raw_segment_descriptor
    {
        uint16_t limit_low;
        uint16_t base_low;
        uint8_t base_mid;
        uint8_t access;
        uint8_t limit_high_flags;
        uint8_t base_high;
    };
#pragma pack(pop)

    struct descriptor
    {
        uint64_t base{};
        uint32_t limit{};
        bool present{};
        bool system{};
        uint8_t type{};
        bool long_mode{};
        bool default_op_size{};
    };

    inline std::optional<cpu_interface::descriptor_table_register> read_descriptor_table(emulator& cpu, const x86_register reg)
    {
        cpu_interface::descriptor_table_register table{};
        if (!cpu.read_descriptor_table(static_cast<int>(reg), table))
        {
            return std::nullopt;
        }

        return table;
    }

    inline std::optional<uint16_t> read_selector(emulator& cpu, const x86_register reg)
    {
        uint16_t selector{};
        const auto bytes_read = cpu.read_raw_register(static_cast<int>(reg), &selector, sizeof(selector));
        if (bytes_read < sizeof(selector))
        {
            return std::nullopt;
        }

        return selector;
    }

    inline std::optional<descriptor> read_descriptor(emulator& cpu, const cpu_interface::descriptor_table_register& table,
                                                     const uint16_t selector)
    {
        const auto index = selector >> 3;
        const auto byte_offset = static_cast<uint64_t>(index) * sizeof(raw_segment_descriptor);
        const auto table_size = static_cast<uint64_t>(table.limit) + 1;
        if (byte_offset + sizeof(raw_segment_descriptor) > table_size)
        {
            return std::nullopt;
        }

        raw_segment_descriptor raw{};
        cpu.read_memory(table.base + byte_offset, &raw, sizeof(raw));

        descriptor desc{};
        uint64_t base = raw.base_low;
        base |= static_cast<uint64_t>(raw.base_mid) << 16;
        base |= static_cast<uint64_t>(raw.base_high) << 24;
        desc.base = base;

        const auto limit_high = static_cast<uint32_t>(raw.limit_high_flags & 0x0F);
        uint32_t limit = raw.limit_low | (limit_high << 16);
        const bool granularity = (raw.limit_high_flags & 0x80) != 0;
        if (granularity)
        {
            limit = (limit << 12) | 0xFFF;
        }
        desc.limit = limit;

        desc.present = (raw.access & 0x80) != 0;
        desc.system = (raw.access & 0x10) == 0;
        desc.type = static_cast<uint8_t>(raw.access & 0x0F);
        desc.long_mode = (raw.limit_high_flags & 0x20) != 0;
        desc.default_op_size = (raw.limit_high_flags & 0x40) != 0;

        if (desc.system)
        {
            const bool needs_high_base = desc.type == 0x2 || desc.type == 0x9 || desc.type == 0xB;
            if (needs_high_base)
            {
                if (byte_offset + (2 * sizeof(raw_segment_descriptor)) > table_size)
                {
                    return std::nullopt;
                }

                uint32_t base_high{};
                cpu.read_memory(table.base + byte_offset + sizeof(raw_segment_descriptor), &base_high, sizeof(base_high));
                desc.base |= static_cast<uint64_t>(base_high) << 32;
            }
        }

        return desc;
    }

    inline std::optional<cpu_interface::descriptor_table_register> resolve_table(emulator& cpu, const uint16_t selector)
    {
        auto gdt = read_descriptor_table(cpu, x86_register::gdtr);
        if (!gdt)
        {
            return std::nullopt;
        }

        const bool table_indicator = (selector & 0x4) != 0;
        if (!table_indicator)
        {
            return gdt;
        }

        auto ldtr_selector = read_selector(cpu, x86_register::ldtr);
        if (!ldtr_selector || ((*ldtr_selector) & ~0x3u) == 0)
        {
            return std::nullopt;
        }

        auto ldt_descriptor = read_descriptor(cpu, *gdt, *ldtr_selector);
        if (!ldt_descriptor || !ldt_descriptor->present || !ldt_descriptor->system || ldt_descriptor->type != 0x2)
        {
            return std::nullopt;
        }

        cpu_interface::descriptor_table_register ldt{};
        ldt.base = ldt_descriptor->base;
        ldt.limit = ldt_descriptor->limit;
        return ldt;
    }

    inline std::optional<segment_bitness> get_segment_bitness(emulator& cpu, const uint16_t selector)
    {
        if ((selector & ~0x3u) == 0)
        {
            return std::nullopt;
        }

        auto table = resolve_table(cpu, selector);
        if (!table)
        {
            return std::nullopt;
        }

        auto desc = read_descriptor(cpu, *table, selector);
        if (!desc || !desc->present || desc->system || (desc->type & 0x8) == 0)
        {
            return std::nullopt;
        }

        if (desc->long_mode)
        {
            return segment_bitness::bit64;
        }

        if (desc->default_op_size)
        {
            return segment_bitness::bit32;
        }

        return segment_bitness::bit16;
    }
}
