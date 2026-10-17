#pragma once

#include "emulator.hpp"
#include "typed_cpu.hpp"

#include <utility>

namespace sogen
{

    // A machine viewed independently of its guest architecture: shared memory and hooks plus the
    // register-independent view of each virtual CPU.
    class guest_machine : public emulator
    {
      public:
        virtual size_t vcpu_count() const = 0;
        virtual guest_cpu& get_guest_cpu(size_t index) = 0;

        virtual bool supports_native_timestamp_counter() const
        {
            return false;
        }
    };

    template <typename Traits>
    class typed_emulator : public guest_machine
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
