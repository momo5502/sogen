#include "spirv_clip_distance.hpp"

#include <unordered_map>
#include <unordered_set>

namespace sogen
{
    namespace
    {
        // https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html
        constexpr uint32_t spirv_magic = 0x07230203;
        constexpr size_t header_words = 5;
        constexpr size_t bound_word = 3;
        constexpr uint32_t version_1_4 = 0x00010400;

        constexpr uint32_t op_entry_point = 15;
        constexpr uint32_t op_capability = 17;
        constexpr uint32_t op_type_pointer = 32;
        constexpr uint32_t op_variable = 59;
        constexpr uint32_t op_access_chain = 65;
        constexpr uint32_t op_in_bounds_access_chain = 66;
        constexpr uint32_t op_ptr_access_chain = 67;
        constexpr uint32_t op_in_bounds_ptr_access_chain = 70;
        constexpr uint32_t op_decorate = 71;
        constexpr uint32_t op_member_decorate = 72;

        constexpr uint32_t capability_clip_distance = 32;
        constexpr uint32_t capability_cull_distance = 33;
        constexpr uint32_t decoration_built_in = 11;
        constexpr uint32_t built_in_clip_distance = 3;
        constexpr uint32_t built_in_cull_distance = 4;
        constexpr uint32_t storage_class_private = 6;

        struct instruction
        {
            size_t offset;
            uint32_t opcode;
            size_t word_count;
        };

        std::vector<instruction> parse(const std::span<const uint32_t> code)
        {
            std::vector<instruction> instructions{};
            for (size_t offset = header_words; offset < code.size();)
            {
                const auto word_count = static_cast<size_t>(code[offset] >> 16);
                if (word_count == 0 || offset + word_count > code.size())
                {
                    return {};
                }

                instructions.push_back({.offset = offset, .opcode = code[offset] & 0xFFFF, .word_count = word_count});
                offset += word_count;
            }
            return instructions;
        }

        bool has_zero_byte(const uint32_t word)
        {
            return (word & 0xFFu) == 0 || (word & 0xFF00u) == 0 || (word & 0xFF0000u) == 0 || (word & 0xFF000000u) == 0;
        }

        bool is_access_chain(const uint32_t opcode)
        {
            return opcode == op_access_chain || opcode == op_in_bounds_access_chain || opcode == op_ptr_access_chain ||
                   opcode == op_in_bounds_ptr_access_chain;
        }
    }

    std::optional<std::vector<uint32_t>> strip_clip_and_cull_distance(const std::span<const uint32_t> code, const bool strip_clip,
                                                                      const bool strip_cull)
    {
        if ((!strip_clip && !strip_cull) || code.size() < header_words || code[0] != spirv_magic)
        {
            return std::nullopt;
        }

        const auto instructions = parse(code);
        if (instructions.empty())
        {
            return std::nullopt;
        }

        const auto is_stripped_built_in = [&](const uint32_t built_in) {
            return (strip_clip && built_in == built_in_clip_distance) || (strip_cull && built_in == built_in_cull_distance);
        };

        std::unordered_set<uint32_t> demoted{};
        for (const auto& inst : instructions)
        {
            if (inst.opcode == op_decorate && inst.word_count >= 4 && code[inst.offset + 2] == decoration_built_in &&
                is_stripped_built_in(code[inst.offset + 3]))
            {
                demoted.insert(code[inst.offset + 1]);
            }
            else if (inst.opcode == op_member_decorate && inst.word_count >= 5 && code[inst.offset + 3] == decoration_built_in &&
                     is_stripped_built_in(code[inst.offset + 4]))
            {
                return std::nullopt;
            }
        }

        if (demoted.empty())
        {
            return std::nullopt;
        }

        struct pointer_type
        {
            uint32_t storage_class;
            uint32_t pointee;
        };

        std::unordered_map<uint32_t, pointer_type> pointer_types{};
        std::unordered_map<uint32_t, uint32_t> private_pointer_by_pointee{};
        for (const auto& inst : instructions)
        {
            if (inst.opcode == op_type_pointer && inst.word_count == 4)
            {
                const auto id = code[inst.offset + 1];
                const pointer_type type{.storage_class = code[inst.offset + 2], .pointee = code[inst.offset + 3]};
                pointer_types[id] = type;
                if (type.storage_class == storage_class_private)
                {
                    private_pointer_by_pointee.try_emplace(type.pointee, id);
                }
            }
        }

        // Pointer types reached from the demoted variables, in declaration order, and the private type replacing each.
        auto bound = code[bound_word];
        std::unordered_map<uint32_t, uint32_t> replacement_type{};
        std::unordered_map<uint32_t, uint32_t> declared_after{};
        const auto require_private = [&](const uint32_t type_id) -> bool {
            if (replacement_type.contains(type_id))
            {
                return true;
            }

            const auto type = pointer_types.find(type_id);
            if (type == pointer_types.end())
            {
                return false;
            }

            if (const auto existing = private_pointer_by_pointee.find(type->second.pointee); existing != private_pointer_by_pointee.end())
            {
                replacement_type[type_id] = existing->second;
                return true;
            }

            const auto new_id = bound++;
            replacement_type[type_id] = new_id;
            declared_after[type_id] = new_id;
            private_pointer_by_pointee[type->second.pointee] = new_id;
            return true;
        };

        std::unordered_set<uint32_t> demoted_pointers = demoted;
        for (const auto& inst : instructions)
        {
            const auto* words = &code[inst.offset];
            if (inst.opcode == op_variable && inst.word_count >= 4 && demoted.contains(words[2]))
            {
                if (!require_private(words[1]))
                {
                    return std::nullopt;
                }
            }
            else if (is_access_chain(inst.opcode) && inst.word_count >= 4 && demoted_pointers.contains(words[3]))
            {
                if (!require_private(words[1]))
                {
                    return std::nullopt;
                }
                demoted_pointers.insert(words[2]);
            }
        }

        const bool keep_in_interface = code[1] >= version_1_4;

        std::vector<uint32_t> result(code.begin(), code.begin() + header_words);
        result[bound_word] = bound;
        result.reserve(code.size() + (declared_after.size() * 4));

        for (const auto& inst : instructions)
        {
            const auto* words = &code[inst.offset];

            if (inst.opcode == op_capability && inst.word_count == 2 &&
                ((strip_clip && words[1] == capability_clip_distance) || (strip_cull && words[1] == capability_cull_distance)))
            {
                continue;
            }

            if (inst.opcode == op_decorate && inst.word_count >= 3 && demoted.contains(words[1]))
            {
                continue;
            }

            const auto start = result.size();
            result.insert(result.end(), words, words + inst.word_count);
            auto* emitted = &result[start];

            if (inst.opcode == op_entry_point && !keep_in_interface)
            {
                // Before SPIR-V 1.4 the interface lists only Input and Output variables.
                size_t name_end = 3;
                while (name_end < inst.word_count && !has_zero_byte(words[name_end]))
                {
                    ++name_end;
                }
                ++name_end;

                std::vector<uint32_t> rebuilt(words, words + std::min(name_end, inst.word_count));
                for (size_t i = name_end; i < inst.word_count; ++i)
                {
                    if (!demoted.contains(words[i]))
                    {
                        rebuilt.push_back(words[i]);
                    }
                }
                rebuilt[0] = (static_cast<uint32_t>(rebuilt.size()) << 16) | op_entry_point;
                result.resize(start);
                result.insert(result.end(), rebuilt.begin(), rebuilt.end());
            }
            else if (inst.opcode == op_type_pointer && inst.word_count == 4)
            {
                if (const auto added = declared_after.find(words[1]); added != declared_after.end())
                {
                    result.insert(result.end(), {(4u << 16) | op_type_pointer, added->second, storage_class_private, words[3]});
                }
            }
            else if (inst.opcode == op_variable && inst.word_count >= 4 && demoted.contains(words[2]))
            {
                emitted[1] = replacement_type.at(words[1]);
                emitted[3] = storage_class_private;
            }
            else if (is_access_chain(inst.opcode) && inst.word_count >= 4 && demoted_pointers.contains(words[3]))
            {
                emitted[1] = replacement_type.at(words[1]);
            }
        }

        return result;
    }
}
