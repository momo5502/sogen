#pragma once

#include "macos_syscall_context.hpp"

#include <array>
#include <string>
#include <string_view>

namespace sogen
{
    struct macos_syscall_entry
    {
        macos_syscall_handler handler{};
        std::string name{};
    };

    class macos_syscall_dispatcher
    {
      public:
        macos_syscall_dispatcher();

        instruction_hook_continuation dispatch(macos_emulator& emu) const;

      private:
        std::array<macos_syscall_entry, 600> unix_handlers_{};

        void register_unix_handler(uint64_t number, macos_syscall_handler handler, std::string_view name);
    };
}
