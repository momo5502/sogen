#pragma once

#include "std_include.hpp"
#include "bsd_syscall_numbers.hpp"
#include "macos_syscall_utils.hpp"

namespace sogen
{

    struct bsd_syscall_handler_entry
    {
        macos_syscall_handler handler{};
        std::string name{};
    };

    class macos_emulator;

    class bsd_syscall_dispatcher
    {
      public:
        bsd_syscall_dispatcher() = default;

        instruction_hook_continuation dispatch(macos_emulator& emu) const;
        void add_handlers();

        const bsd_syscall_handler_entry* get_entry(const uint64_t id) const
        {
            if (id >= this->handlers_.size())
            {
                return nullptr;
            }

            return &this->handlers_[static_cast<size_t>(id)];
        }

        const bsd_syscall_handler_entry* get_mach_trap_entry(const uint32_t index) const
        {
            if (index >= this->mach_traps_.size())
            {
                return nullptr;
            }

            return &this->mach_traps_[index];
        }

        friend std::vector<uint64_t> registered_bsd_syscall_numbers(const bsd_syscall_dispatcher& dispatcher);

      private:
        void register_handler(const uint64_t id, const macos_syscall_handler handler, const std::string_view name)
        {
            this->handlers_.at(static_cast<size_t>(id)) = {
                .handler = handler,
                .name = std::string{name},
            };
        }

        void register_mach_trap(const uint32_t index, const macos_syscall_handler handler, const std::string_view name)
        {
            this->mach_traps_.at(index) = {
                .handler = handler,
                .name = std::string{name},
            };
        }

        // Defined in mach/mach_traps.cpp so the Mach trap table lives beside its handlers without
        // widening register_mach_trap's access.
        void add_mach_traps();
        void add_dyld_handlers();

        std::array<bsd_syscall_handler_entry, 600> handlers_{};
        std::array<bsd_syscall_handler_entry, 128> mach_traps_{};
    };

    // No emulator code calls this; it exists for the coverage test and the audit it is meant to unblock.
    std::vector<uint64_t> registered_bsd_syscall_numbers(const bsd_syscall_dispatcher& dispatcher);

}
