#pragma once

#include "emulator.hpp"
#include "typed_cpu.hpp"

#include <utility>

namespace sogen
{

    template <typename Traits>
    class typed_emulator : public emulator
    {
      public:
        using registers = Traits::register_type;
        using pointer_type = Traits::pointer_type;
        using hookable_instructions = Traits::hookable_instructions;

        emulator_hook* hook_instruction(hookable_instructions instruction_type, instruction_hook_callback callback)
        {
            return this->hook_instruction(static_cast<int>(instruction_type), std::move(callback));
        }

        emulator_hook* hook_instruction(hookable_instructions instruction_type, simple_instruction_hook_callback callback)
        {
            return this->hook_instruction(instruction_type, [c = std::move(callback)](cpu_interface&, const uint64_t) { return c(); });
        }

      private:
        emulator_hook* hook_instruction(int instruction_type, instruction_hook_callback callback) override = 0;
    };

} // namespace sogen
