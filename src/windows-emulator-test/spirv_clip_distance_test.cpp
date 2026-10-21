#include <gtest/gtest.h>

#include <devices/spirv_clip_distance.hpp>

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace sogen::test
{
    namespace
    {
        constexpr uint32_t version_1_0 = 0x00010000;
        constexpr uint32_t version_1_4 = 0x00010400;

        constexpr uint32_t op_capability = 17;
        constexpr uint32_t op_entry_point = 15;
        constexpr uint32_t op_type_pointer = 32;
        constexpr uint32_t op_variable = 59;
        constexpr uint32_t op_access_chain = 65;
        constexpr uint32_t op_decorate = 71;
        constexpr uint32_t op_member_decorate = 72;

        constexpr uint32_t capability_shader = 1;
        constexpr uint32_t capability_clip_distance = 32;
        constexpr uint32_t capability_cull_distance = 33;
        constexpr uint32_t built_in_position = 0;
        constexpr uint32_t built_in_clip_distance = 3;
        constexpr uint32_t built_in_cull_distance = 4;
        constexpr uint32_t storage_class_output = 3;
        constexpr uint32_t storage_class_private = 6;

        enum id : uint32_t
        {
            main_function = 1,
            void_type,
            function_type,
            float_type,
            uint_type,
            uint_0,
            uint_2,
            float_array_type,
            output_array_pointer,
            output_float_pointer,
            clip_variable,
            cull_variable,
            entry_label,
            clip_element,
            float_1,
            vec4_type,
            per_vertex_type,
            output_per_vertex_pointer,
            per_vertex_variable,
            bound,
        };

        class module_builder
        {
          public:
            explicit module_builder(const uint32_t version)
                : words_{0x07230203, version, 0, bound, 0}
            {
            }

            void op(const uint32_t opcode, const std::initializer_list<uint32_t> operands)
            {
                this->words_.push_back((static_cast<uint32_t>(operands.size() + 1) << 16) | opcode);
                this->words_.insert(this->words_.end(), operands);
            }

            std::vector<uint32_t> finish() const
            {
                return this->words_;
            }

          private:
            std::vector<uint32_t> words_;
        };

        constexpr uint32_t main_name = 0x6E69616D; // "main"

        // A vertex shader writing a standalone ClipDistance (and CullDistance) output, as DXVK's shader compiler emits.
        std::vector<uint32_t> make_standalone_module(const uint32_t version, const bool cull)
        {
            module_builder module{version};
            module.op(op_capability, {capability_shader});
            module.op(op_capability, {capability_clip_distance});
            if (cull)
            {
                module.op(op_capability, {capability_cull_distance});
            }
            module.op(14, {0, 1}); // OpMemoryModel Logical GLSL450
            if (cull)
            {
                module.op(op_entry_point, {0, main_function, main_name, 0, clip_variable, cull_variable});
            }
            else
            {
                module.op(op_entry_point, {0, main_function, main_name, 0, clip_variable});
            }
            module.op(op_decorate, {clip_variable, 11, built_in_clip_distance});
            if (cull)
            {
                module.op(op_decorate, {cull_variable, 11, built_in_cull_distance});
            }
            module.op(19, {void_type});
            module.op(33, {function_type, void_type});
            module.op(22, {float_type, 32});
            module.op(21, {uint_type, 32, 0});
            module.op(43, {uint_type, uint_0, 0});
            module.op(43, {uint_type, uint_2, 2});
            module.op(43, {float_type, float_1, 0x3F800000});
            module.op(28, {float_array_type, float_type, uint_2});
            module.op(op_type_pointer, {output_array_pointer, storage_class_output, float_array_type});
            module.op(op_type_pointer, {output_float_pointer, storage_class_output, float_type});
            module.op(op_variable, {output_array_pointer, clip_variable, storage_class_output});
            if (cull)
            {
                module.op(op_variable, {output_array_pointer, cull_variable, storage_class_output});
            }
            module.op(54, {void_type, main_function, 0, function_type});
            module.op(248, {entry_label});
            module.op(op_access_chain, {output_float_pointer, clip_element, clip_variable, uint_0});
            module.op(62, {clip_element, float_1});
            module.op(253, {});
            module.op(56, {});
            return module.finish();
        }

        // The same written through gl_PerVertex, as shaders compiled from GLSL declare it.
        std::vector<uint32_t> make_block_module()
        {
            module_builder module{version_1_0};
            module.op(op_capability, {capability_shader});
            module.op(op_capability, {capability_clip_distance});
            module.op(14, {0, 1});
            module.op(op_entry_point, {0, main_function, main_name, 0, per_vertex_variable});
            module.op(op_member_decorate, {per_vertex_type, 0, 11, built_in_position});
            module.op(op_member_decorate, {per_vertex_type, 1, 11, built_in_clip_distance});
            module.op(op_decorate, {per_vertex_type, 2}); // Block
            module.op(19, {void_type});
            module.op(33, {function_type, void_type});
            module.op(22, {float_type, 32});
            module.op(23, {vec4_type, float_type, 4});
            module.op(21, {uint_type, 32, 0});
            module.op(43, {uint_type, uint_0, 1});
            module.op(43, {uint_type, uint_2, 2});
            module.op(43, {float_type, float_1, 0x3F800000});
            module.op(28, {float_array_type, float_type, uint_2});
            module.op(30, {per_vertex_type, vec4_type, float_array_type});
            module.op(op_type_pointer, {output_per_vertex_pointer, storage_class_output, per_vertex_type});
            module.op(op_type_pointer, {output_float_pointer, storage_class_output, float_type});
            module.op(op_variable, {output_per_vertex_pointer, per_vertex_variable, storage_class_output});
            module.op(54, {void_type, main_function, 0, function_type});
            module.op(248, {entry_label});
            module.op(op_access_chain, {output_float_pointer, clip_element, per_vertex_variable, uint_0, uint_0});
            module.op(62, {clip_element, float_1});
            module.op(253, {});
            module.op(56, {});
            return module.finish();
        }

        struct instruction
        {
            uint32_t opcode;
            std::vector<uint32_t> operands;
        };

        std::vector<instruction> parse(const std::vector<uint32_t>& code)
        {
            std::vector<instruction> instructions;
            for (size_t offset = 5; offset < code.size();)
            {
                const auto word_count = code[offset] >> 16;
                instructions.push_back({.opcode = code[offset] & 0xFFFF,
                                        .operands = {code.begin() + static_cast<ptrdiff_t>(offset + 1),
                                                     code.begin() + static_cast<ptrdiff_t>(offset + word_count)}});
                offset += word_count;
            }
            return instructions;
        }

        bool has_capability(const std::vector<instruction>& instructions, const uint32_t capability)
        {
            return std::ranges::any_of(
                instructions, [&](const instruction& inst) { return inst.opcode == op_capability && inst.operands.at(0) == capability; });
        }

        const instruction& find_result(const std::vector<instruction>& instructions, const uint32_t opcode, const uint32_t result_id,
                                       const size_t result_index)
        {
            const auto it = std::ranges::find_if(instructions, [&](const instruction& inst) {
                return inst.opcode == opcode && inst.operands.size() > result_index && inst.operands[result_index] == result_id;
            });
            EXPECT_NE(it, instructions.end());
            return *it;
        }

        uint32_t pointer_storage_class(const std::vector<instruction>& instructions, const uint32_t pointer_type)
        {
            return find_result(instructions, op_type_pointer, pointer_type, 0).operands.at(1);
        }

        std::vector<uint32_t> interface_of(const std::vector<instruction>& instructions)
        {
            const auto& entry = *std::ranges::find_if(instructions, [](const instruction& inst) { return inst.opcode == op_entry_point; });
            return {entry.operands.begin() + 4, entry.operands.end()};
        }
    }

    TEST(SpirvClipDistanceTest, LeavesModulesWithoutTheBuiltInsAlone)
    {
        const auto module = make_standalone_module(version_1_0, false);
        EXPECT_FALSE(strip_clip_and_cull_distance(module, false, true).has_value());
        EXPECT_FALSE(strip_clip_and_cull_distance(module, false, false).has_value());
    }

    TEST(SpirvClipDistanceTest, DemotesAStandaloneClipDistanceBeforeSpirv14)
    {
        const auto stripped = strip_clip_and_cull_distance(make_standalone_module(version_1_0, false), true, false);
        ASSERT_TRUE(stripped.has_value());

        const auto instructions = parse(*stripped);
        EXPECT_FALSE(has_capability(instructions, capability_clip_distance));
        EXPECT_FALSE(std::ranges::any_of(
            instructions, [](const instruction& inst) { return inst.opcode == op_decorate && inst.operands.at(0) == clip_variable; }));

        const auto& variable = find_result(instructions, op_variable, clip_variable, 1);
        EXPECT_EQ(variable.operands.at(2), storage_class_private);
        EXPECT_EQ(pointer_storage_class(instructions, variable.operands.at(0)), storage_class_private);

        const auto& element = find_result(instructions, op_access_chain, clip_element, 1);
        EXPECT_EQ(pointer_storage_class(instructions, element.operands.at(0)), storage_class_private);

        EXPECT_TRUE(interface_of(instructions).empty());
        EXPECT_EQ((*stripped)[3], bound + 2);
    }

    TEST(SpirvClipDistanceTest, KeepsTheDemotedVariableInTheInterfaceFromSpirv14)
    {
        const auto stripped = strip_clip_and_cull_distance(make_standalone_module(version_1_4, false), true, false);
        ASSERT_TRUE(stripped.has_value());
        EXPECT_EQ(interface_of(parse(*stripped)), std::vector<uint32_t>{clip_variable});
    }

    TEST(SpirvClipDistanceTest, StripsOnlyTheRequestedBuiltIn)
    {
        const auto module = make_standalone_module(version_1_0, true);

        const auto clip_only = parse(*strip_clip_and_cull_distance(module, true, false));
        EXPECT_FALSE(has_capability(clip_only, capability_clip_distance));
        EXPECT_TRUE(has_capability(clip_only, capability_cull_distance));
        EXPECT_EQ(find_result(clip_only, op_variable, cull_variable, 1).operands.at(2), storage_class_output);

        const auto cull_only = parse(*strip_clip_and_cull_distance(module, false, true));
        EXPECT_TRUE(has_capability(cull_only, capability_clip_distance));
        EXPECT_FALSE(has_capability(cull_only, capability_cull_distance));
        EXPECT_EQ(find_result(cull_only, op_variable, clip_variable, 1).operands.at(2), storage_class_output);
        EXPECT_EQ(find_result(cull_only, op_variable, cull_variable, 1).operands.at(2), storage_class_private);
    }

    TEST(SpirvClipDistanceTest, LeavesBuiltInBlockMembersAlone)
    {
        EXPECT_FALSE(strip_clip_and_cull_distance(make_block_module(), true, true).has_value());
    }
}
