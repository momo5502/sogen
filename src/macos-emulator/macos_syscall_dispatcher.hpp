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
        std::array<macos_syscall_entry, 128> mach_handlers_{};
        std::array<macos_syscall_entry, 8> machdep_handlers_{};

        void register_unix_handler(uint64_t number, macos_syscall_handler handler, std::string_view name);
        void register_mach_handler(uint64_t number, macos_syscall_handler handler, std::string_view name);
        void register_machdep_handler(uint64_t number, macos_syscall_handler handler, std::string_view name);
        const macos_syscall_entry* find_entry(uint64_t syscall_class, uint64_t number) const;
    };
}
