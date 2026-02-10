#pragma once

#include "std_include.hpp"

#include <arch_emulator.hpp>
#include <cstring>

// Forward declarations
class linux_emulator;

// Linux signal numbers and constants
// All names use LINUX_ prefix to avoid macOS <signal.h> macro collisions
namespace linux_signals
{
    constexpr int LINUX_SIGHUP = 1;
    constexpr int LINUX_SIGINT = 2;
    constexpr int LINUX_SIGQUIT = 3;
    constexpr int LINUX_SIGILL = 4;
    constexpr int LINUX_SIGTRAP = 5;
    constexpr int LINUX_SIGABRT = 6;
    constexpr int LINUX_SIGBUS = 7;
    constexpr int LINUX_SIGFPE = 8;
    constexpr int LINUX_SIGKILL = 9;
    constexpr int LINUX_SIGUSR1 = 10;
    constexpr int LINUX_SIGSEGV = 11;
    constexpr int LINUX_SIGUSR2 = 12;
    constexpr int LINUX_SIGPIPE = 13;
    constexpr int LINUX_SIGALRM = 14;
    constexpr int LINUX_SIGTERM = 15;
    constexpr int LINUX_SIGSTKFLT = 16;
    constexpr int LINUX_SIGCHLD = 17;
    constexpr int LINUX_SIGCONT = 18;
    constexpr int LINUX_SIGSTOP = 19;
    constexpr int LINUX_SIGTSTP = 20;
    constexpr int LINUX_SIGTTIN = 21;
    constexpr int LINUX_SIGTTOU = 22;

    constexpr int LINUX_NSIG = 64;

    // Signal action flags
    constexpr uint64_t LINUX_SA_NOCLDSTOP = 0x00000001;
    constexpr uint64_t LINUX_SA_NOCLDWAIT = 0x00000002;
    constexpr uint64_t LINUX_SA_SIGINFO = 0x00000004;
    constexpr uint64_t LINUX_SA_ONSTACK = 0x08000000;
    constexpr uint64_t LINUX_SA_RESTART = 0x10000000;
    constexpr uint64_t LINUX_SA_NODEFER = 0x40000000;
    constexpr uint64_t LINUX_SA_RESETHAND = 0x80000000;
    constexpr uint64_t LINUX_SA_RESTORER = 0x04000000;

    // Special signal handler values
    constexpr uint64_t LINUX_SIG_DFL = 0;
    constexpr uint64_t LINUX_SIG_IGN = 1;

    // siginfo_t si_code values
    constexpr int LINUX_SI_USER = 0;
    constexpr int LINUX_SI_KERNEL = 128;

    // SIGSEGV si_code
    constexpr int LINUX_SEGV_MAPERR = 1; // address not mapped to object
    constexpr int LINUX_SEGV_ACCERR = 2; // invalid permissions for mapped object

    // SIGILL si_code
    constexpr int LINUX_ILL_ILLOPC = 1; // illegal opcode

    // SIGFPE si_code
    constexpr int LINUX_FPE_INTDIV = 1; // integer divide by zero

    // SIGTRAP si_code
    constexpr int LINUX_TRAP_BRKPT = 1; // process breakpoint
}

// Linux kernel sigaction structure (as seen by the kernel on x86-64)
// Field names use linux_ prefix to avoid macOS sa_handler macro
struct linux_kernel_sigaction
{
    uint64_t linux_sa_handler{}; // handler address (SIG_DFL=0, SIG_IGN=1, or function pointer)
    uint64_t linux_sa_flags{};
    uint64_t linux_sa_restorer{};
    uint64_t linux_sa_mask{}; // 64-bit signal mask
};

// Linux x86-64 sigcontext (from arch/x86/include/uapi/asm/sigcontext.h)
// This matches the kernel struct sigcontext layout exactly
#pragma pack(push, 1)
struct linux_sigcontext
{
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rbp;
    uint64_t rbx;
    uint64_t rdx;
    uint64_t rax;
    uint64_t rcx;
    uint64_t rsp;
    uint64_t rip;
    uint64_t eflags;
    uint16_t cs;
    uint16_t gs;
    uint16_t fs;
    uint16_t ss;
    uint64_t err;
    uint64_t trapno;
    uint64_t oldmask;
    uint64_t cr2;
    uint64_t fpstate_ptr; // pointer to fpstate (or 0)
    uint64_t reserved1[8];
};
#pragma pack(pop)

static_assert(sizeof(linux_sigcontext) == 256);

// Linux x86-64 siginfo_t (128 bytes)
#pragma pack(push, 1)
struct linux_siginfo_t
{
    int32_t si_signo;
    int32_t si_errno;
    int32_t si_code;
    int32_t _pad0;
    // Union of various fields -- for SIGSEGV, the fault address is at offset 16
    uint64_t fault_address; // _sigfault.si_addr
    uint8_t _pad[128 - 24]; // pad to 128 bytes total
};
#pragma pack(pop)

static_assert(sizeof(linux_siginfo_t) == 128);

// Linux x86-64 stack_t
#pragma pack(push, 1)
struct linux_stack_t
{
    uint64_t ss_sp;
    int32_t ss_flags;
    int32_t _pad;
    uint64_t ss_size;
};
#pragma pack(pop)

static_assert(sizeof(linux_stack_t) == 24);

// Linux x86-64 ucontext_t
#pragma pack(push, 1)
struct linux_ucontext_t
{
    uint64_t uc_flags;
    uint64_t uc_link;              // pointer to next ucontext
    linux_stack_t uc_stack;        // 24 bytes
    linux_sigcontext uc_mcontext;  // 256 bytes
    uint64_t uc_sigmask;           // signal mask (64-bit)
    uint8_t _pad_sigmask[128 - 8]; // __unused[] in kernel -- sigset_t is 128 bytes
};
#pragma pack(pop)

// rt_sigframe -- what the kernel pushes on the user stack before entering a signal handler
#pragma pack(push, 1)
struct linux_rt_sigframe
{
    uint64_t pretcode;    // return address: points to sigreturn trampoline
    linux_siginfo_t info; // 128 bytes
    linux_ucontext_t uc;  // ucontext
};
#pragma pack(pop)

// Signal dispatch engine
class signal_dispatcher
{
  public:
    // Registered signal handlers (indexed by signal number, 0-63)
    std::array<linux_kernel_sigaction, 64> actions{};

    // Address of the sigreturn trampoline code page
    uint64_t sigreturn_trampoline_addr{};

    // Whether the trampoline has been set up
    bool trampoline_ready{};

    // Install the sigreturn trampoline code page in emulated memory
    void setup_trampoline(linux_emulator& emu);

    // Deliver a signal to the current thread.
    // Returns true if a handler was invoked (RIP redirected to handler).
    // Returns false if the signal caused default action (process termination).
    bool deliver_signal(linux_emulator& emu, int signum, uint64_t fault_addr = 0, int si_code = 0);

    // Restore context from a signal frame on the stack (called by sys_rt_sigreturn).
    void sigreturn(linux_emulator& emu);
};
