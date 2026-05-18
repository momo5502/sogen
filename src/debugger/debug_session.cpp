#include "debug_session.hpp"

#include <algorithm>
#include <array>
#include <span>
#include <unordered_set>

#include <disassembler.hpp>
#include <scoped_hook.hpp>

// Phase 2 implements the read-only introspection surface by reusing existing,
// proven primitives (the Capstone `disassembler`, `emu().reg`, `mod_manager`,
// `process.threads`). Breakpoints and the stepping engine are Phase 3 — their
// methods are defined as documented no-ops so the class stays complete and
// linkable, and the protocol layer can already advertise them.

namespace debugger
{
    struct debug_session::impl
    {
        std::vector<breakpoint> breakpoints{};
        std::unordered_set<uint64_t> breakpoint_addrs{};

        // A single per-instruction execute hook drives both breakpoints and
        // stepping. It lives exactly as long as the debug_session, so it is
        // never created/destroyed from inside its own callback (no
        // delete-in-callback UB, no nested emu().start()).
        scoped_hook control_hook{};
    };

    debug_session::debug_session(windows_emulator& emu)
        : emu_(&emu),
          impl_(std::make_unique<impl>())
    {
        auto& cpu = this->emu_->emu();
        this->impl_->control_hook = scoped_hook(cpu, cpu.hook_memory_execution([this](const uint64_t address) {
            if (this->should_break(address))
            {
                debugger::enter_breakpoint(*this->emu_, address);
            }
        }));
    }

    debug_session::~debug_session() = default;

    bool debug_session::should_break(const uint64_t address) const
    {
        return this->impl_->breakpoint_addrs.contains(address) || debugger::step_should_break(address);
    }

    // --- breakpoints (just a set; the persistent control hook does the work) ---

    bool debug_session::add_breakpoint(const uint64_t address, const breakpoint_type type, const size_t size)
    {
        std::erase_if(this->impl_->breakpoints, [&](const breakpoint& b) { return b.address == address; });
        this->impl_->breakpoints.push_back({address, size, type, true, false});
        this->impl_->breakpoint_addrs.insert(address);
        return true;
    }

    bool debug_session::remove_breakpoint(const uint64_t address)
    {
        this->impl_->breakpoint_addrs.erase(address);
        auto& bps = this->impl_->breakpoints;
        const auto before = bps.size();
        std::erase_if(bps, [&](const breakpoint& b) { return b.address == address; });
        return bps.size() != before;
    }

    void debug_session::clear_breakpoints()
    {
        this->impl_->breakpoint_addrs.clear();
        this->impl_->breakpoints.clear();
    }

    std::vector<breakpoint> debug_session::list_breakpoints() const
    {
        return this->impl_->breakpoints;
    }

    // --- stepping: just record the request; the break loop resolves and the
    // persistent control hook enforces it. Never nests emu().start(). ---

    void debug_session::step(const step_kind kind)
    {
        switch (kind)
        {
        case step_kind::into:
            debugger::request_resume(step_request::into);
            break;
        case step_kind::over:
            debugger::request_resume(step_request::over);
            break;
        case step_kind::out:
            debugger::request_resume(step_request::step_out);
            break;
        }
    }

    void debug_session::run_to(const uint64_t address)
    {
        debugger::request_resume(step_request::cont, address);
    }

    // --- read-only introspection ---

    uint64_t debug_session::instruction_pointer() const
    {
        return this->emu_->emu().reg<uint64_t>(x86_register::rip);
    }

    std::vector<register_value> debug_session::registers() const
    {
        auto& cpu = this->emu_->emu();

        static constexpr std::pair<const char*, x86_register> gpr[] = {
            {"rax", x86_register::rax}, {"rbx", x86_register::rbx},       {"rcx", x86_register::rcx}, {"rdx", x86_register::rdx},
            {"rsi", x86_register::rsi}, {"rdi", x86_register::rdi},       {"rbp", x86_register::rbp}, {"rsp", x86_register::rsp},
            {"r8", x86_register::r8},   {"r9", x86_register::r9},         {"r10", x86_register::r10}, {"r11", x86_register::r11},
            {"r12", x86_register::r12}, {"r13", x86_register::r13},       {"r14", x86_register::r14}, {"r15", x86_register::r15},
            {"rip", x86_register::rip}, {"rflags", x86_register::eflags},
        };
        static constexpr std::pair<const char*, x86_register> seg[] = {
            {"cs", x86_register::cs}, {"ss", x86_register::ss}, {"ds", x86_register::ds},
            {"es", x86_register::es}, {"fs", x86_register::fs}, {"gs", x86_register::gs},
        };

        std::vector<register_value> result{};
        result.reserve(std::size(gpr) + std::size(seg));

        for (const auto& [name, reg] : gpr)
        {
            result.push_back({name, cpu.reg<uint64_t>(reg), sizeof(uint64_t)});
        }
        for (const auto& [name, reg] : seg)
        {
            result.push_back({name, cpu.reg<uint16_t>(reg), sizeof(uint16_t)});
        }

        return result;
    }

    std::vector<disassembled_instruction> debug_session::disassemble(const uint64_t address, const size_t count) const
    {
        std::vector<disassembled_instruction> result{};
        if (count == 0)
        {
            return result;
        }

        auto& cpu = this->emu_->emu();

        // Clamp before sizing the buffer: `count` comes straight from an
        // untrusted debug command (scripting console), and an unbounded
        // `count * 16` resize is a trivial OOM / hang vector for the worker.
        static constexpr size_t max_instruction_count = 4096;
        const auto clamped_count = std::min(count, max_instruction_count);

        // Generous upper bound: x86 instructions are at most 15 bytes.
        std::vector<uint8_t> bytes{};
        bytes.resize(clamped_count * 16);
        if (!this->emu_->memory.try_read_memory(address, bytes.data(), bytes.size()))
        {
            // Shrink until a readable window is found (we may be near an edge).
            bytes.resize(16);
            if (!this->emu_->memory.try_read_memory(address, bytes.data(), bytes.size()))
            {
                return result;
            }
        }

        disassembler dis{};
        const auto cs_selector = cpu.reg<uint16_t>(x86_register::cs);
        const auto insns =
            dis.disassemble(cpu, cs_selector, std::span<const uint8_t>(bytes.data(), bytes.size()), clamped_count, address);

        for (const auto& insn : insns)
        {
            disassembled_instruction out{};
            out.address = insn.address;
            out.bytes.assign(insn.bytes, insn.bytes + insn.size);
            out.mnemonic = insn.mnemonic;
            out.operands = insn.op_str;

            if (const auto* name = this->emu_->mod_manager.find_name(insn.address); name && std::string_view(name) != "<N/A>")
            {
                out.symbol = name;
            }

            switch (insn.id)
            {
            case X86_INS_CALL:
                out.is_call = true;
                break;
            case X86_INS_JMP:
                out.is_jump = true;
                break;
            case X86_INS_RET:
            case X86_INS_RETF:
            case X86_INS_IRET:
            case X86_INS_IRETD:
            case X86_INS_IRETQ:
                out.is_return = true;
                break;
            default:
                // Conditional jumps (Jcc) all start with 'j'.
                out.is_jump = (out.mnemonic.size() > 1 && out.mnemonic[0] == 'j');
                break;
            }

            if ((out.is_call || out.is_jump) && insn.detail && insn.detail->x86.op_count > 0)
            {
                const auto& op = insn.detail->x86.operands[0];
                if (op.type == X86_OP_IMM)
                {
                    out.branch = static_cast<uint64_t>(op.imm);
                }
            }

            result.push_back(std::move(out));
        }

        return result;
    }

    std::vector<module_info> debug_session::modules() const
    {
        std::vector<module_info> result{};
        for (const auto& mod : this->emu_->mod_manager.modules() | std::views::values)
        {
            result.push_back({mod.name, mod.image_base, mod.size_of_image, mod.entry_point});
        }
        return result;
    }

    std::vector<thread_info> debug_session::threads() const
    {
        std::vector<thread_info> result{};
        const auto* active = this->emu_->process.active_thread;
        for (auto& thread : this->emu_->process.threads | std::views::values)
        {
            result.push_back({thread.id, thread.current_ip, &thread == active});
        }
        return result;
    }

    std::vector<stack_frame> debug_session::call_stack(std::optional<uint32_t> /*thread_id*/) const
    {
        // Phase 2: the current frame plus a bounded, best-effort frame-pointer
        // walk. A precise unwinder (CFI/PE unwind info) is a later phase.
        auto& cpu = this->emu_->emu();
        auto& memory = this->emu_->memory;

        const auto name_of = [&](const uint64_t ip) -> std::string {
            const auto* name = this->emu_->mod_manager.find_name(ip);
            return (name && std::string_view(name) != "<N/A>") ? std::string(name) : std::string{};
        };

        std::vector<stack_frame> frames{};

        const auto rip = cpu.reg<uint64_t>(x86_register::rip);
        const auto rsp = cpu.reg<uint64_t>(x86_register::rsp);
        frames.push_back({rip, rsp, name_of(rip), name_of(rip)});

        auto frame_pointer = cpu.reg<uint64_t>(x86_register::rbp);
        for (size_t depth = 0; depth < 64 && frame_pointer != 0; ++depth)
        {
            uint64_t saved_rbp = 0;
            uint64_t return_address = 0;
            if (!memory.try_read_memory(frame_pointer, &saved_rbp, sizeof(saved_rbp)) ||
                !memory.try_read_memory(frame_pointer + sizeof(uint64_t), &return_address, sizeof(return_address)))
            {
                break;
            }

            if (return_address == 0 || saved_rbp <= frame_pointer)
            {
                break;
            }

            frames.push_back({return_address, frame_pointer, name_of(return_address), name_of(return_address)});
            frame_pointer = saved_rbp;
        }

        return frames;
    }
}
