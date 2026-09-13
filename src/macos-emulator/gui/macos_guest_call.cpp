#include "../std_include.hpp"
#include "macos_guest_call.hpp"

#include "../macos_emulator.hpp"

namespace sogen
{
    namespace
    {
        // arm64e signs a return address in the top bits, and a callee that returns with `retab` leaves
        // the signature in x30 rather than the address it branched to. So lr read back at the trap is
        // the trap page with a signature on it, and an unmasked comparison does not recognise it.
        constexpr uint64_t POINTER_BITS = 0x0000FFFFFFFFFFFFULL;

        arm64_register general_register(const size_t index)
        {
            return static_cast<arm64_register>(static_cast<uint32_t>(arm64_register::x0) + index);
        }

        macos_guest_resume_state capture_state(arm64_64_emulator& backend)
        {
            macos_guest_resume_state state{};

            for (size_t index = 0; index < state.x.size(); ++index)
            {
                state.x[index] = backend.reg(general_register(index));
            }

            state.fp = backend.reg(arm64_register::x29);
            state.lr = backend.reg(arm64_register::x30);
            state.sp = backend.reg(arm64_register::sp);
            state.nzcv = backend.reg(arm64_register::nzcv);
            state.pc = backend.reg(arm64_register::pc);
            return state;
        }

        void restore_state(arm64_64_emulator& backend, const macos_guest_resume_state& state)
        {
            for (size_t index = 0; index < state.x.size(); ++index)
            {
                backend.reg(general_register(index), state.x[index]);
            }

            backend.reg(arm64_register::x29, state.fp);
            backend.reg(arm64_register::x30, state.lr);
            backend.reg(arm64_register::sp, state.sp);
            backend.reg(arm64_register::nzcv, state.nzcv);
            backend.reg(arm64_register::pc, state.pc);
        }
    }

    bool macos_guest_call_stack::prepare(macos_emulator& emu)
    {
        if (this->prepared_)
        {
            return true;
        }

        if (!emu.memory.allocate_memory(MACOS_GUI_TRAP_BASE, MACOS_PAGE_SIZE, memory_permission::read_exec))
        {
            emu.log.warn("Unable to map the guest-call trap page at 0x%" PRIx64 "\n", MACOS_GUI_TRAP_BASE);
            return false;
        }

        // Every word traps, not just the first: a callee that returns past its own link register -- a
        // tail call whose frame was already popped, say -- still lands on an svc rather than running off
        // into whatever follows.
        const std::vector<uint32_t> words(MACOS_PAGE_SIZE / sizeof(uint32_t), MACOS_ARM64_SVC_80);
        if (!emu.memory.try_write_memory(MACOS_GUI_TRAP_BASE, words.data(), words.size() * sizeof(uint32_t)))
        {
            return false;
        }

        this->prepared_ = true;
        return true;
    }

    bool macos_guest_call_stack::is_trap(const uint64_t entry) const
    {
        return this->prepared_ && entry >= MACOS_GUI_TRAP_BASE && entry < MACOS_GUI_TRAP_BASE + MACOS_PAGE_SIZE;
    }

    bool macos_guest_call_stack::begin(macos_emulator& emu, macos_guest_call_request request)
    {
        if (!this->prepared_ || request.function == 0)
        {
            return false;
        }

        uint32_t probe = 0;
        if (!emu.memory.try_read_memory(request.function, &probe, sizeof(probe)))
        {
            emu.log.warn("Refusing a guest call to unmapped address 0x%" PRIx64 "\n", request.function);
            return false;
        }

        if (this->frames_.size() >= MACOS_GUI_MAX_CALL_DEPTH)
        {
            emu.log.warn("Guest call chain exceeded %zu frames; abandoning it\n", MACOS_GUI_MAX_CALL_DEPTH);
            return false;
        }

        auto& backend = emu.emu();

        const auto caller_return = backend.reg(arm64_register::lr);
        const auto return_address = this->is_trap(caller_return & POINTER_BITS) ? this->inherited_return_ : caller_return;

        for (size_t index = 0; index < request.args.size(); ++index)
        {
            backend.reg(static_cast<arm64_register>(static_cast<uint32_t>(arm64_register::x0) + index), request.args[index]);
        }

        for (size_t index = 0; index < request.double_args.size(); ++index)
        {
            uint64_t bits = 0;
            std::memcpy(&bits, &request.double_args[index], sizeof(bits));
            backend.reg(static_cast<arm64_register>(static_cast<uint32_t>(arm64_register::d0) + index), bits);
        }

        backend.reg(arm64_register::lr, MACOS_GUI_TRAP_BASE);
        backend.reg(arm64_register::pc, request.function);

        this->frames_.push_back(frame{.on_return = std::move(request.on_return), .return_address = return_address});
        return true;
    }

    bool macos_guest_call_stack::handle_trap(macos_emulator& emu, const uint64_t entry)
    {
        if (!this->is_trap(entry) || this->frames_.empty())
        {
            return false;
        }

        auto& backend = emu.emu();
        const auto result = backend.reg(arm64_register::x0);

        auto completed = std::move(this->frames_.back());
        this->frames_.pop_back();

        const auto depth_after_pop = this->frames_.size();

        const auto outer_inherited = this->inherited_return_;
        this->inherited_return_ = completed.return_address;

        if (completed.on_return)
        {
            completed.on_return(emu, result);
        }

        this->inherited_return_ = outer_inherited;

        // A continuation that started another call has already pointed pc at it, and that call carries
        // this frame's return address forward. Only when the continuation started nothing is the guest
        // sent back to its caller -- and x0 is left exactly as the continuation left it, because that
        // value is what the caller receives.
        if (this->frames_.size() == depth_after_pop)
        {
            if (this->frames_.empty() && this->resume_)
            {
                restore_state(backend, *this->resume_);
                this->resume_.reset();
                return true;
            }

            backend.reg(arm64_register::lr, completed.return_address);
            backend.reg(arm64_register::pc, completed.return_address);
        }

        return true;
    }

    bool macos_guest_call_stack::arm_resume(macos_emulator& emu)
    {
        if (!this->prepared_ || !this->frames_.empty() || this->resume_)
        {
            return false;
        }

        this->resume_ = capture_state(emu.emu());
        return true;
    }

    void macos_guest_call_stack::disarm_resume()
    {
        this->resume_.reset();
    }

    void macos_guest_call_stack::reset()
    {
        this->frames_.clear();
        this->inherited_return_ = 0;
        this->resume_.reset();
    }
}
