#pragma once

// Debugger core abstraction — see docs/debugger/ARCHITECTURE.md
//
// `debug_session` is the first-class debugger subsystem. It wraps a live
// `windows_emulator` and exposes a clean, backend-agnostic API for
// breakpoints, stepping and read-only introspection. It deliberately only
// uses primitives that every emulator backend (Unicorn/WHP/icicle)
// implements, and reuses the exact mechanisms already proven by the GDB stub
// (`hook_memory_execution` + `scoped_hook`, `start(1)`) and the Capstone
// `disassembler`.
//
// Phase 1 defines the contract (this header + architecture doc). Phase 2/3
// provide the implementation and protocol wiring; nothing references this
// type yet, so it stays decoupled and independently reviewable.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <windows_emulator.hpp>

#include "event_handler.hpp"

namespace debugger
{
    enum class breakpoint_type : uint8_t
    {
        software_execute,
        hardware_execute,
        memory_read,
        memory_write,
        memory_access,
    };

    enum class step_kind : uint8_t
    {
        into, // execute exactly one instruction
        over, // step, but run calls to completion
        out,  // run until the current function returns
    };

    struct breakpoint
    {
        uint64_t address{};
        size_t size{1};
        breakpoint_type type{breakpoint_type::software_execute};
        bool enabled{true};
        bool temporary{false};
    };

    struct register_value
    {
        std::string name{};
        uint64_t value{};
        size_t size{};
    };

    struct disassembled_instruction
    {
        uint64_t address{};
        std::vector<uint8_t> bytes{};
        std::string mnemonic{};
        std::string operands{};
        std::string symbol{};           // resolved symbol at `address`, if any
        std::optional<uint64_t> branch; // resolved call/jump target, if any
        bool is_call{};
        bool is_jump{};
        bool is_return{};
    };

    struct module_info
    {
        std::string name{};
        uint64_t base{};
        uint64_t size{};
        uint64_t entry_point{};
    };

    struct thread_info
    {
        uint32_t id{};
        uint64_t instruction_pointer{};
        bool active{};
    };

    struct stack_frame
    {
        uint64_t instruction_pointer{};
        uint64_t stack_pointer{};
        std::string symbol{};
        std::string module{};
    };

    // Owns debugger state for one emulated process. Construct once per
    // `windows_emulator` (the browser runs a single instance). All
    // introspection is read-only and only meaningful while the emulator is
    // paused; callers must enforce that and never block the emulation thread.
    class debug_session
    {
      public:
        explicit debug_session(windows_emulator& emu);
        ~debug_session();

        debug_session(const debug_session&) = delete;
        debug_session& operator=(const debug_session&) = delete;
        debug_session(debug_session&&) = delete;
        debug_session& operator=(debug_session&&) = delete;

        // --- breakpoints (RAII execute hooks, like the GDB stub) ---
        bool add_breakpoint(uint64_t address, breakpoint_type type = breakpoint_type::software_execute, size_t size = 1);
        bool remove_breakpoint(uint64_t address);
        void clear_breakpoints();
        std::vector<breakpoint> list_breakpoints() const;

        // --- stepping (must not desync emulator state) ---
        // `step` executes the requested motion on the active thread and
        // returns when the emulator is paused again.
        void step(step_kind kind);
        void run_to(uint64_t address); // temporary breakpoint + continue

        // --- read-only introspection (valid while paused) ---
        uint64_t instruction_pointer() const;
        std::vector<register_value> registers() const;
        std::vector<disassembled_instruction> disassemble(uint64_t address, size_t count) const;
        std::vector<module_info> modules() const;
        std::vector<thread_info> threads() const;
        std::vector<stack_frame> call_stack(std::optional<uint32_t> thread_id = std::nullopt) const;

      private:
        bool should_break(uint64_t address) const;

        struct impl;
        windows_emulator* emu_{};
        std::unique_ptr<impl> impl_; // breakpoint set + persistent control hook
    };
}
