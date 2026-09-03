#pragma once

#include "mach_types.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <serialization.hpp>

namespace sogen
{
    class macos_emulator;
}

namespace sogen::mach
{
    namespace exception_type
    {
        constexpr uint32_t bad_access = 1;
        constexpr uint32_t bad_instruction = 2;
        constexpr uint32_t arithmetic = 3;
        constexpr uint32_t emulation = 4;
        constexpr uint32_t software = 5;
        constexpr uint32_t breakpoint = 6;
        constexpr uint32_t syscall = 7;
        constexpr uint32_t mach_syscall = 8;
        constexpr uint32_t rpc_alert = 9;
        constexpr uint32_t crash = 10;
        constexpr uint32_t guard = 12;
    }

    namespace exception_behavior
    {
        constexpr uint32_t defaults = 1;
        constexpr uint32_t state = 2;
        constexpr uint32_t state_identity = 3;

        // Set by everything modern; it widens the two codes from 32 to 64 bits and moves the routine id
        // from exception_raise 2401 to mach_exception_raise 2405.
        constexpr uint32_t mach_codes = 0x80000000u;
    }

    namespace signal_number
    {
        constexpr int32_t sigill = 4;
        constexpr int32_t sigtrap = 5;
        constexpr int32_t sigabrt = 6;
        constexpr int32_t sigfpe = 8;
        constexpr int32_t sigbus = 10;
        constexpr int32_t sigsegv = 11;
        constexpr int32_t sigsys = 12;
    }

    struct exception_handler_entry
    {
        uint32_t mask{};
        port_name_t port{};
        uint32_t behavior{};
        int32_t flavor{};

        void serialize(utils::buffer_serializer& buffer) const;
        void deserialize(utils::buffer_deserializer& buffer);
    };

    struct raised_exception
    {
        uint32_t type{};
        uint64_t code{};
        uint64_t subcode{};
        uint64_t pc{};
        int32_t signal{};
        bool delivered{};

        // Whether `subcode` is a faulting address the emulator actually knows. A CPU exception that
        // arrives through unicorn's interrupt hook carries none -- the abort is stopped before it
        // reaches EL1, so FAR_EL1 reads back zero -- and printing zero there reads exactly like a null
        // dereference, which is a lie a reader will chase.
        bool address_known{true};

        void serialize(utils::buffer_serializer& buffer) const;
        void deserialize(utils::buffer_deserializer& buffer);
    };

    class exception_state
    {
      public:
        kern_return_t set_ports(bool thread_level, uint32_t mask, port_name_t port, uint32_t behavior, int32_t flavor);
        std::optional<exception_handler_entry> find_handler(uint32_t type) const;

        void serialize(utils::buffer_serializer& buffer) const;
        void deserialize(utils::buffer_deserializer& buffer);

      private:
        std::vector<exception_handler_entry> thread_handlers_{};
        std::vector<exception_handler_entry> task_handlers_{};
    };

    int32_t exception_to_signal(uint32_t type);
    std::string_view exception_type_name(uint32_t type);
    std::string_view signal_name(int32_t signal);

    raised_exception raise_guest_exception(macos_emulator& emu, uint32_t type, uint64_t code, uint64_t subcode);
    raised_exception report_cpu_exception(macos_emulator& emu, uint32_t exception_index, uint64_t pc);
    raised_exception report_memory_violation(macos_emulator& emu, uint64_t address, bool protection_failure, uint64_t pc);

    std::string format_exception_detail(macos_emulator& emu, const raised_exception& raised);
}
