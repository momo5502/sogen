#pragma once

#include <memory_interface.hpp>

namespace sogen
{
    class macos_memory_manager
    {
      public:
        explicit macos_memory_manager(memory_interface& memory)
            : memory_(&memory)
        {
        }

        void map(const uint64_t address, const size_t size, const memory_permission permissions)
        {
            this->memory_->map_memory(address, size, permissions);
        }

        void protect(const uint64_t address, const size_t size, const memory_permission permissions)
        {
            this->memory_->apply_memory_protection(address, size, permissions);
        }

      private:
        memory_interface* memory_{};
    };
}
