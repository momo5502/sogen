#include "std_include.hpp"
#include "signal_dispatch.hpp"
#include "linux_emulator.hpp"

#include <cstring>

using namespace linux_signals;

void signal_dispatcher::setup_trampoline(linux_emulator& emu)
{
    if (this->trampoline_ready)
    {
        return;
    }

    // Allocate a page for the sigreturn trampoline code
    // This is a synthetic code page containing:
    //   mov rax, 15    ; SYS_rt_sigreturn
    //   syscall
    //
    // x86-64 encoding:
    //   48 c7 c0 0f 00 00 00    mov rax, 0xf
    //   0f 05                    syscall

    constexpr uint64_t TRAMPOLINE_ADDR = 0x7fff0000;
    constexpr size_t TRAMPOLINE_SIZE = 0x1000;

    if (!emu.memory.allocate_memory(TRAMPOLINE_ADDR, TRAMPOLINE_SIZE, memory_permission::read_write))
    {
        emu.log.error("Failed to allocate sigreturn trampoline page\n");
        return;
    }

    // Write the trampoline code
    const uint8_t trampoline_code[] = {
        0x48, 0xC7, 0xC0, 0x0F, 0x00, 0x00, 0x00, // mov rax, 15 (SYS_rt_sigreturn)
        0x0F, 0x05,                               // syscall
    };

    emu.emu().write_memory(TRAMPOLINE_ADDR, trampoline_code, sizeof(trampoline_code));

    // Make the page read+execute (no write)
    emu.memory.protect_memory(TRAMPOLINE_ADDR, TRAMPOLINE_SIZE, memory_permission::read | memory_permission::exec);

    this->sigreturn_trampoline_addr = TRAMPOLINE_ADDR;
    this->trampoline_ready = true;
}

bool signal_dispatcher::deliver_signal(linux_emulator& emu, const int signum, const uint64_t fault_addr, const int si_code)
{
    if (signum < 1 || signum >= 64)
    {
        return false;
    }

    // Ensure trampoline is set up
    this->setup_trampoline(emu);

    const auto& action = this->actions[signum];
    const auto handler = action.linux_sa_handler;

    // Check if the signal is ignored
    if (handler == LINUX_SIG_IGN)
    {
        return true; // "handled" by ignoring
    }

    // Check if using the default action
    if (handler == LINUX_SIG_DFL)
    {
        // Default action for most signals is to terminate the process
        switch (signum)
        {
        case LINUX_SIGCHLD:
        case LINUX_SIGCONT:
            // Default action: ignore
            return true;

        default:
            // Default action: terminate
            emu.log.error("Signal %d: default action is terminate (fault_addr=0x%llx)\n", signum,
                          static_cast<unsigned long long>(fault_addr));
            emu.process.exit_status = 128 + signum;
            emu.stop();
            return false;
        }
    }

    // We have a user handler — build the signal frame and redirect execution
    auto& e = emu.emu();

    // Determine which stack to use
    uint64_t frame_sp = e.reg(x86_register::rsp);
    // TODO: check SA_ONSTACK and alternate signal stack

    // The frame must be 16-byte aligned. The rt_sigframe is pushed below RSP.
    // Compute the size and align
    constexpr size_t frame_size = sizeof(linux_rt_sigframe);

    // Align the frame start to 16 bytes (minus 8 for the pretcode "return address")
    frame_sp -= frame_size;
    frame_sp &= ~static_cast<uint64_t>(0xF); // 16-byte align
    frame_sp -= 8;                           // space for pretcode acting as a return address

    const uint64_t frame_addr = frame_sp;

    // Build the signal frame
    linux_rt_sigframe frame{};
    memset(&frame, 0, sizeof(frame));

    // pretcode: address of sigreturn trampoline
    // If SA_RESTORER is set, use the user-provided restorer address
    if ((action.linux_sa_flags & LINUX_SA_RESTORER) && action.linux_sa_restorer != 0)
    {
        frame.pretcode = action.linux_sa_restorer;
    }
    else
    {
        frame.pretcode = this->sigreturn_trampoline_addr;
    }

    // Fill siginfo
    frame.info.si_signo = static_cast<int32_t>(signum);
    frame.info.si_errno = 0;
    frame.info.si_code = static_cast<int32_t>(si_code);
    frame.info.si_addr = fault_addr;

    // Fill ucontext
    frame.uc.uc_flags = 0;
    frame.uc.uc_link = 0;

    // Save current register state into sigcontext
    auto& sc = frame.uc.uc_mcontext;
    sc.r8 = e.reg(x86_register::r8);
    sc.r9 = e.reg(x86_register::r9);
    sc.r10 = e.reg(x86_register::r10);
    sc.r11 = e.reg(x86_register::r11);
    sc.r12 = e.reg(x86_register::r12);
    sc.r13 = e.reg(x86_register::r13);
    sc.r14 = e.reg(x86_register::r14);
    sc.r15 = e.reg(x86_register::r15);
    sc.rdi = e.reg(x86_register::rdi);
    sc.rsi = e.reg(x86_register::rsi);
    sc.rbp = e.reg(x86_register::rbp);
    sc.rbx = e.reg(x86_register::rbx);
    sc.rdx = e.reg(x86_register::rdx);
    sc.rax = e.reg(x86_register::rax);
    sc.rcx = e.reg(x86_register::rcx);
    sc.rsp = e.reg(x86_register::rsp);
    sc.rip = e.reg(x86_register::rip);
    sc.eflags = e.reg(x86_register::rflags);
    sc.cs = 0x33;
    sc.ss = 0x2B;
    sc.err = 0;
    sc.trapno = (signum == LINUX_SIGSEGV) ? 14 : 0; // page fault trap number
    sc.oldmask = emu.process.active_thread ? emu.process.active_thread->signal_mask : 0;
    sc.cr2 = fault_addr;
    sc.fpstate_ptr = 0; // no FPU state for now

    // Save signal mask
    frame.uc.uc_sigmask = emu.process.active_thread ? emu.process.active_thread->signal_mask : 0;

    // Write the frame to the emulated stack
    e.write_memory(frame_addr, &frame, sizeof(frame));

    // Compute pointers for handler arguments
    const uint64_t siginfo_addr = frame_addr + offsetof(linux_rt_sigframe, info);
    const uint64_t ucontext_addr = frame_addr + offsetof(linux_rt_sigframe, uc);

    // Set up registers for handler call:
    //   RDI = signum
    //   RSI = &siginfo (only if SA_SIGINFO)
    //   RDX = &ucontext (only if SA_SIGINFO)
    //   RSP = frame_addr (pretcode is at [RSP], so 'ret' from handler goes to trampoline)
    //   RIP = handler address
    e.reg(x86_register::rdi, static_cast<uint64_t>(signum));

    if (action.linux_sa_flags & LINUX_SA_SIGINFO)
    {
        e.reg(x86_register::rsi, siginfo_addr);
        e.reg(x86_register::rdx, ucontext_addr);
    }
    else
    {
        e.reg(x86_register::rsi, 0);
        e.reg(x86_register::rdx, 0);
    }

    e.reg(x86_register::rsp, frame_addr);
    e.reg(x86_register::rip, handler);

    // Block signals specified in sa_mask during handler execution
    if (emu.process.active_thread)
    {
        const auto old_mask = emu.process.active_thread->signal_mask;

        // Block the delivered signal itself (unless SA_NODEFER)
        if (!(action.linux_sa_flags & LINUX_SA_NODEFER))
        {
            emu.process.active_thread->signal_mask |= (1ULL << (signum - 1));
        }

        // Block signals in sa_mask
        emu.process.active_thread->signal_mask |= action.linux_sa_mask;

        (void)old_mask; // stored in frame.uc.uc_sigmask for restore
    }

    // If SA_RESETHAND, reset handler to SIG_DFL
    if (action.linux_sa_flags & LINUX_SA_RESETHAND)
    {
        this->actions[signum].linux_sa_handler = LINUX_SIG_DFL;
    }

    return true;
}

void signal_dispatcher::sigreturn(linux_emulator& emu)
{
    auto& e = emu.emu();

    // The current RSP should point to the rt_sigframe (after the trampoline's 'ret' consumed the pretcode).
    // Actually, when rt_sigreturn is invoked via 'syscall', RSP still points just past the pretcode.
    // The frame is at RSP - 8 (the pretcode was at the start of the frame).
    //
    // Wait — the layout is: [pretcode][siginfo][ucontext] at frame_addr.
    // RSP was set to frame_addr. When the handler does 'ret', it pops pretcode from [RSP]
    // and jumps to the trampoline, RSP is now frame_addr + 8.
    // The trampoline does 'mov rax, 15; syscall' — RSP is still frame_addr + 8.
    // So the frame starts at RSP - 8.

    const uint64_t rsp = e.reg(x86_register::rsp);
    const uint64_t frame_addr = rsp - 8;
    // Read the frame
    linux_rt_sigframe frame{};
    e.read_memory(frame_addr, &frame, sizeof(frame));
    // Restore registers from sigcontext
    const auto& sc = frame.uc.uc_mcontext;
    e.reg(x86_register::r8, sc.r8);
    e.reg(x86_register::r9, sc.r9);
    e.reg(x86_register::r10, sc.r10);
    e.reg(x86_register::r11, sc.r11);
    e.reg(x86_register::r12, sc.r12);
    e.reg(x86_register::r13, sc.r13);
    e.reg(x86_register::r14, sc.r14);
    e.reg(x86_register::r15, sc.r15);
    e.reg(x86_register::rdi, sc.rdi);
    e.reg(x86_register::rsi, sc.rsi);
    e.reg(x86_register::rbp, sc.rbp);
    e.reg(x86_register::rbx, sc.rbx);
    e.reg(x86_register::rdx, sc.rdx);
    e.reg(x86_register::rax, sc.rax);
    e.reg(x86_register::rcx, sc.rcx);
    e.reg(x86_register::rsp, sc.rsp);
    // Subtract 2 from the desired RIP to compensate for the CPU backend
    // automatically advancing past the 2-byte `syscall` instruction (0f 05)
    // after this hook returns.
    e.reg(x86_register::rip, sc.rip - 2);
    e.reg(x86_register::rflags, sc.eflags);

    // Restore signal mask
    if (emu.process.active_thread)
    {
        emu.process.active_thread->signal_mask = frame.uc.uc_sigmask;
    }
}
