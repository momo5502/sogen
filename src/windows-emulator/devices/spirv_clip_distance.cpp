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

        constexpr uint32_t op_source_continued = 2;
        constexpr uint32_t op_source = 3;
        constexpr uint32_t op_source_extension = 4;
        constexpr uint32_t op_name = 5;
        constexpr uint32_t op_member_name = 6;
        constexpr uint32_t op_string = 7;
        constexpr uint32_t op_line = 8;
        constexpr uint32_t op_extension = 10;
        constexpr uint32_t op_ext_inst_import = 11;
        constexpr uint32_t op_ext_inst = 12;
        constexpr uint32_t op_memory_model = 14;
        constexpr uint32_t op_entry_point = 15;
        constexpr uint32_t op_execution_mode = 16;
        constexpr uint32_t op_capability = 17;
        constexpr uint32_t op_type_int = 21;
        constexpr uint32_t op_type_float = 22;
        constexpr uint32_t op_type_vector = 23;
        constexpr uint32_t op_type_matrix = 24;
        constexpr uint32_t op_type_image = 25;
        constexpr uint32_t op_type_array = 28;
        constexpr uint32_t op_type_runtime_array = 29;
        constexpr uint32_t op_type_struct = 30;
        constexpr uint32_t op_type_pointer = 32;
        constexpr uint32_t op_type_forward_pointer = 39;
        constexpr uint32_t op_constant = 43;
        constexpr uint32_t op_spec_constant = 50;
        constexpr uint32_t op_spec_constant_op = 52;
        constexpr uint32_t op_function = 54;
        constexpr uint32_t op_variable = 59;
        constexpr uint32_t op_load = 61;
        constexpr uint32_t op_store = 62;
        constexpr uint32_t op_copy_memory = 63;
        constexpr uint32_t op_copy_memory_sized = 64;
        constexpr uint32_t op_access_chain = 65;
        constexpr uint32_t op_in_bounds_access_chain = 66;
        constexpr uint32_t op_ptr_access_chain = 67;
        constexpr uint32_t op_in_bounds_ptr_access_chain = 70;
        constexpr uint32_t op_decorate = 71;
        constexpr uint32_t op_member_decorate = 72;
        constexpr uint32_t op_vector_shuffle = 79;
        constexpr uint32_t op_composite_extract = 81;
        constexpr uint32_t op_composite_insert = 82;
        constexpr uint32_t op_image_sample_implicit_lod = 87;
        constexpr uint32_t op_image_write = 99;
        constexpr uint32_t op_loop_merge = 246;
        constexpr uint32_t op_selection_merge = 247;
        constexpr uint32_t op_branch_conditional = 250;
        constexpr uint32_t op_switch = 251;
        constexpr uint32_t op_image_sparse_sample_implicit_lod = 305;
        constexpr uint32_t op_image_sparse_dref_gather = 315;
        constexpr uint32_t op_no_line = 317;
        constexpr uint32_t op_image_sparse_read = 320;
        constexpr uint32_t op_module_processed = 330;
        constexpr uint32_t op_decorate_string = 5632;
        constexpr uint32_t op_member_decorate_string = 5633;

        constexpr uint32_t capability_clip_distance = 32;
        constexpr uint32_t capability_cull_distance = 33;
        constexpr uint32_t decoration_built_in = 11;
        constexpr uint32_t built_in_clip_distance = 3;
        constexpr uint32_t built_in_cull_distance = 4;
        constexpr uint32_t storage_class_output = 3;
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

        // The words of an instruction that can hold an ID are those in [begin, end) other than `literal`.
        struct id_operands
        {
            size_t begin{1};
            size_t end{};
            size_t literal{};
        };

        // The image operands mask that follows the coordinate (and the depth reference or gather component).
        size_t image_operands_index(const uint32_t opcode)
        {
            if (opcode == op_image_write)
            {
                return 4;
            }

            if (opcode == op_image_sparse_read)
            {
                return 5;
            }

            // The sampling, fetch and gather instructions come in the same order in the sparse range.
            const auto variant = opcode >= op_image_sparse_sample_implicit_lod ? opcode - op_image_sparse_sample_implicit_lod
                                                                               : opcode - op_image_sample_implicit_lod;
            switch (variant)
            {
            case 2:  // Dref ImplicitLod
            case 3:  // Dref ExplicitLod
            case 6:  // Proj Dref ImplicitLod
            case 7:  // Proj Dref ExplicitLod
            case 9:  // Gather
            case 10: // Dref Gather
                return 6;
            default:
                return 5;
            }
        }

        // The operands the spec defines as literals are known for the instructions that carry them in shaders. Every
        // operand of any other instruction is taken to be an ID, which can only make the pass leave a module alone.
        id_operands find_id_operands(const instruction& inst)
        {
            const auto count = inst.word_count;
            switch (inst.opcode)
            {
            case op_source_continued:
            case op_source_extension:
            case op_extension:
            case op_memory_model:
            case op_capability:
            case op_no_line:
            case op_module_processed:
                return {.end = 1};
            case op_source:
                return {.begin = 3, .end = std::min<size_t>(count, 4)};
            case op_name:
            case op_member_name:
            case op_string:
            case op_line:
            case op_ext_inst_import:
            case op_execution_mode:
            case op_type_int:
            case op_type_float:
            case op_selection_merge:
            case op_decorate:
            case op_member_decorate:
            case op_decorate_string:
            case op_member_decorate_string:
                return {.end = std::min<size_t>(count, 2)};
            case op_type_vector:
            case op_type_matrix:
            case op_type_image:
            case op_constant:
            case op_spec_constant:
            case op_store:
            case op_copy_memory:
            case op_loop_merge:
            case op_switch:
                return {.end = std::min<size_t>(count, 3)};
            case op_load:
            case op_copy_memory_sized:
            case op_composite_extract:
            case op_branch_conditional:
                return {.end = std::min<size_t>(count, 4)};
            case op_vector_shuffle:
            case op_composite_insert:
                return {.end = std::min<size_t>(count, 5)};
            case op_type_pointer:
            case op_type_forward_pointer:
                return {.end = count, .literal = 2};
            case op_spec_constant_op:
            case op_function:
            case op_variable:
                return {.end = count, .literal = 3};
            case op_ext_inst:
                return {.end = count, .literal = 4};
            default:
                break;
            }

            if ((inst.opcode >= op_image_sample_implicit_lod && inst.opcode <= op_image_write) ||
                (inst.opcode >= op_image_sparse_sample_implicit_lod && inst.opcode <= op_image_sparse_dref_gather) ||
                inst.opcode == op_image_sparse_read)
            {
                return {.end = count, .literal = image_operands_index(inst.opcode)};
            }

            return {.end = count};
        }

        template <typename F>
        bool any_id_operand(const std::span<const uint32_t> code, const instruction& inst, F&& predicate)
        {
            const auto operands = find_id_operands(inst);
            for (size_t i = operands.begin; i < operands.end; ++i)
            {
                if (i != operands.literal && predicate(i, code[inst.offset + i]))
                {
                    return true;
                }
            }
            return false;
        }

        // A built-in block such as gl_PerVertex whose trailing members are stripped clip or cull distances.
        struct stripped_block
        {
            uint32_t kept_members{};
            std::vector<uint32_t> member_types{};
        };
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
        std::unordered_map<uint32_t, std::unordered_set<uint32_t>> stripped_members{};
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
                stripped_members[code[inst.offset + 1]].insert(code[inst.offset + 2]);
            }
        }

        if (demoted.empty() && stripped_members.empty())
        {
            return std::nullopt;
        }

        // The stripped members are dropped from the end of their block, so the other members keep their indices. A
        // block used in any way other than through an output variable is left alone.
        std::unordered_map<uint32_t, stripped_block> blocks{};
        for (const auto& inst : instructions)
        {
            const auto* words = &code[inst.offset];
            if (inst.opcode == op_type_struct && inst.word_count >= 2)
            {
                if (const auto stripped = stripped_members.find(words[1]); stripped != stripped_members.end())
                {
                    const auto member_count = static_cast<uint32_t>(inst.word_count - 2);
                    const auto kept_members = member_count - static_cast<uint32_t>(stripped->second.size());
                    for (const auto member : stripped->second)
                    {
                        if (member < kept_members || member >= member_count)
                        {
                            return std::nullopt;
                        }
                    }

                    blocks[words[1]] = stripped_block{.kept_members = kept_members, .member_types = {words + 2, words + inst.word_count}};
                }

                for (size_t i = 2; i < inst.word_count; ++i)
                {
                    if (stripped_members.contains(words[i]))
                    {
                        return std::nullopt;
                    }
                }
            }
            else if ((inst.opcode == op_type_array || inst.opcode == op_type_runtime_array) && inst.word_count >= 3 &&
                     stripped_members.contains(words[2]))
            {
                return std::nullopt;
            }
            else if (inst.opcode == op_type_pointer && inst.word_count == 4 && stripped_members.contains(words[3]) &&
                     words[2] != storage_class_output)
            {
                return std::nullopt;
            }
        }

        if (blocks.size() != stripped_members.size())
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
        std::unordered_map<uint32_t, uint32_t> constants{};
        std::unordered_map<uint32_t, uint32_t> block_variables{};
        for (const auto& inst : instructions)
        {
            const auto* words = &code[inst.offset];
            if (inst.opcode == op_type_pointer && inst.word_count == 4)
            {
                const pointer_type type{.storage_class = words[2], .pointee = words[3]};
                pointer_types[words[1]] = type;
                if (type.storage_class == storage_class_private)
                {
                    private_pointer_by_pointee.try_emplace(type.pointee, words[1]);
                }
            }
            else if (inst.opcode == op_constant && inst.word_count == 4)
            {
                constants[words[2]] = words[3];
            }
            else if (inst.opcode == op_variable && inst.word_count >= 4)
            {
                if (const auto type = pointer_types.find(words[1]); type != pointer_types.end() && blocks.contains(type->second.pointee))
                {
                    block_variables[words[2]] = type->second.pointee;
                }
            }
        }

        // Only access chains into a block can be redirected. Any other use of a block variable or of the block type, such
        // as loading, copying or initializing the whole block or passing its pointer on, would still refer to the dropped
        // members.
        for (const auto& inst : instructions)
        {
            const auto* words = &code[inst.offset];
            const bool declares_or_names = inst.opcode == op_name || inst.opcode == op_member_name || inst.opcode == op_decorate ||
                                           inst.opcode == op_member_decorate || inst.opcode == op_decorate_string ||
                                           inst.opcode == op_member_decorate_string || inst.opcode == op_entry_point ||
                                           inst.opcode == op_type_struct || inst.opcode == op_type_pointer;
            const bool accesses_block = (inst.opcode == op_access_chain || inst.opcode == op_in_bounds_access_chain) &&
                                        inst.word_count >= 4 && block_variables.contains(words[3]);
            const bool declares_block_variable = inst.opcode == op_variable && inst.word_count == 4 && block_variables.contains(words[2]);
            if (declares_or_names || accesses_block || declares_block_variable)
            {
                continue;
            }

            if (any_id_operand(code, inst, [&](size_t, const uint32_t id) { return block_variables.contains(id) || blocks.contains(id); }))
            {
                return std::nullopt;
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

        std::vector<uint32_t> private_declarations{};
        std::vector<uint32_t> private_variables{};
        std::unordered_map<uint64_t, uint32_t> member_variables{};
        std::unordered_map<uint32_t, uint32_t> redirected_chains{};
        const auto member_variable = [&](const uint32_t block_variable, const uint32_t member) {
            const auto key = (static_cast<uint64_t>(block_variable) << 32) | member;
            if (const auto existing = member_variables.find(key); existing != member_variables.end())
            {
                return existing->second;
            }

            const auto member_type = blocks.at(block_variables.at(block_variable)).member_types.at(member);
            auto pointer = private_pointer_by_pointee.find(member_type);
            if (pointer == private_pointer_by_pointee.end())
            {
                const auto pointer_id = bound++;
                private_declarations.insert(private_declarations.end(),
                                            {(4u << 16) | op_type_pointer, pointer_id, storage_class_private, member_type});
                pointer = private_pointer_by_pointee.emplace(member_type, pointer_id).first;
            }

            const auto variable = bound++;
            private_declarations.insert(private_declarations.end(),
                                        {(4u << 16) | op_variable, pointer->second, variable, storage_class_private});
            private_variables.push_back(variable);
            member_variables[key] = variable;
            return variable;
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
            else if (is_access_chain(inst.opcode) && inst.word_count >= 4 && block_variables.contains(words[3]))
            {
                if (inst.opcode != op_access_chain && inst.opcode != op_in_bounds_access_chain)
                {
                    return std::nullopt;
                }

                if (inst.word_count < 5)
                {
                    return std::nullopt;
                }

                const auto member = constants.find(words[4]);
                if (member == constants.end())
                {
                    return std::nullopt;
                }

                if (member->second >= blocks.at(block_variables.at(words[3])).kept_members)
                {
                    if (!require_private(words[1]))
                    {
                        return std::nullopt;
                    }
                    redirected_chains[words[2]] = member_variable(words[3], member->second);
                    demoted_pointers.insert(words[2]);
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

        // Only the variables and their access chains are retyped, so a pointer passed on in any other way, such as to a
        // function, OpCopyObject, OpPhi or OpSelect, would keep its Output type.
        for (const auto& inst : instructions)
        {
            if (inst.opcode == op_name || inst.opcode == op_decorate || inst.opcode == op_entry_point)
            {
                continue;
            }

            const auto allowed_use = [&](const size_t index) {
                return (inst.opcode == op_variable && index == 2) || (is_access_chain(inst.opcode) && (index == 2 || index == 3)) ||
                       (inst.opcode == op_load && index == 3) || (inst.opcode == op_store && index == 1);
            };
            if (any_id_operand(code, inst,
                               [&](const size_t index, const uint32_t id) { return demoted_pointers.contains(id) && !allowed_use(index); }))
            {
                return std::nullopt;
            }
        }

        const bool keep_in_interface = code[1] >= version_1_4;

        std::vector<uint32_t> result(code.begin(), code.begin() + header_words);
        result.reserve(code.size() + (declared_after.size() * 4) + private_declarations.size() + private_variables.size());

        bool declared_private_variables = false;
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

            if ((inst.opcode == op_member_decorate || inst.opcode == op_member_decorate_string || inst.opcode == op_member_name) &&
                inst.word_count >= 3)
            {
                if (const auto block = blocks.find(words[1]); block != blocks.end() && words[2] >= block->second.kept_members)
                {
                    continue;
                }
            }

            if (inst.opcode == op_function && !declared_private_variables)
            {
                result.insert(result.end(), private_declarations.begin(), private_declarations.end());
                declared_private_variables = true;
            }

            const auto start = result.size();

            if (inst.opcode == op_type_struct)
            {
                if (const auto block = blocks.find(words[1]); block != blocks.end())
                {
                    const auto word_count = 2 + block->second.kept_members;
                    result.insert(result.end(), words, words + word_count);
                    result[start] = (word_count << 16) | op_type_struct;
                    continue;
                }
            }

            if (is_access_chain(inst.opcode) && inst.word_count >= 5)
            {
                if (const auto redirected = redirected_chains.find(words[2]); redirected != redirected_chains.end())
                {
                    const auto word_count = static_cast<uint32_t>(inst.word_count - 1);
                    result.insert(result.end(),
                                  {(word_count << 16) | inst.opcode, replacement_type.at(words[1]), words[2], redirected->second});
                    result.insert(result.end(), words + 5, words + inst.word_count);
                    continue;
                }
            }

            result.insert(result.end(), words, words + inst.word_count);
            auto* emitted = &result[start];

            if (inst.opcode == op_entry_point)
            {
                // Before SPIR-V 1.4 the interface lists only Input and Output variables, since 1.4 every global one.
                size_t name_end = 3;
                while (name_end < inst.word_count && !has_zero_byte(words[name_end]))
                {
                    ++name_end;
                }
                ++name_end;

                std::vector<uint32_t> rebuilt(words, words + std::min(name_end, inst.word_count));
                for (size_t i = name_end; i < inst.word_count; ++i)
                {
                    if (keep_in_interface || !demoted.contains(words[i]))
                    {
                        rebuilt.push_back(words[i]);
                    }
                }
                if (keep_in_interface)
                {
                    rebuilt.insert(rebuilt.end(), private_variables.begin(), private_variables.end());
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

        result[bound_word] = bound;
        return result;
    }
}
