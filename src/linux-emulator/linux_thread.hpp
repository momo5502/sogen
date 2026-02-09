#pragma once

#include "std_include.hpp"
#include <arch_emulator.hpp>

struct linux_saved_registers
{
    uint64_t rax{};
    uint64_t rbx{};
    uint64_t rcx{};
    uint64_t rdx{};
    uint64_t rsi{};
    uint64_t rdi{};
    uint64_t rbp{};
    uint64_t rsp{};
    uint64_t r8{};
    uint64_t r9{};
    uint64_t r10{};
    uint64_t r11{};
    uint64_t r12{};
    uint64_t r13{};
    uint64_t r14{};
    uint64_t r15{};
    uint64_t rip{};
    uint64_t rflags{};
    uint64_t fs_base{};
    uint64_t gs_base{};
};

// Thread wait states for the scheduler
enum class thread_wait_state : uint8_t
{
    running,    // Thread is runnable
    futex_wait, // Blocked on FUTEX_WAIT — waiting for a wake on futex_wait_address
    sleeping,   // Blocked on nanosleep/clock_nanosleep — waiting until sleep_deadline
};

struct linux_thread
{
    uint32_t tid{};

    uint64_t stack_base{};
    uint64_t stack_size{};
    uint64_t fs_base{};

    uint64_t clear_child_tid{};
    uint64_t set_child_tid{};
    uint64_t robust_list_head{};

    uint64_t signal_mask{};

    bool terminated{};
    int exit_code{};

    uint64_t executed_instructions{};

    // Wait state for scheduler
    thread_wait_state wait_state{thread_wait_state::running};
    uint64_t futex_wait_address{};          // Address being waited on (for futex_wait state)
    uint32_t futex_wait_val{};              // Expected value at the futex address
    uint32_t futex_wait_bitset{0xFFFFFFFF}; // Bitset mask for FUTEX_WAIT_BITSET
    uint64_t sleep_deadline_ns{};           // Absolute nanosecond deadline (for sleeping state)

    linux_saved_registers saved_regs{};

    // Check if this thread is ready to run.
    // A thread is ready if it's in running state, or if its wait condition
    // has been satisfied (futex value changed, sleep expired, signal pending).
    bool is_thread_ready(const uint64_t current_time_ns = 0) const
    {
        if (this->terminated)
        {
            return false;
        }

        switch (this->wait_state)
        {
        case thread_wait_state::running:
            return true;

        case thread_wait_state::sleeping:
            // Ready if the sleep deadline has passed
            return current_time_ns >= this->sleep_deadline_ns;

        case thread_wait_state::futex_wait:
            // Futex readiness is checked externally by reading the memory value.
            // The scheduler should call check_futex_ready() with the memory interface.
            // For the simple check without memory access, we return false.
            return false;
        }

        return false;
    }

    // Check if a futex wait has become ready by reading the current value at the
    // futex address. Returns true if the value has changed (spurious wakeup).
    template <typename MemoryReader>
    bool check_futex_ready(MemoryReader& memory) const
    {
        if (this->wait_state != thread_wait_state::futex_wait)
        {
            return this->wait_state == thread_wait_state::running;
        }

        uint32_t current_val{};
        memory.read_memory(this->futex_wait_address, &current_val, sizeof(current_val));

        // If the value at the futex address has changed, the thread can wake up
        return current_val != this->futex_wait_val;
    }

    // Wake this thread from a futex wait. Called by FUTEX_WAKE.
    void wake_from_futex()
    {
        if (this->wait_state == thread_wait_state::futex_wait)
        {
            this->wait_state = thread_wait_state::running;
            this->futex_wait_address = 0;
        }
    }

    // Wake this thread from a sleep. Called when sleep timeout expires or signal arrives.
    void wake_from_sleep()
    {
        if (this->wait_state == thread_wait_state::sleeping)
        {
            this->wait_state = thread_wait_state::running;
            this->sleep_deadline_ns = 0;
        }
    }

    void save(x86_64_emulator& emu)
    {
        this->saved_regs.rax = emu.reg(x86_register::rax);
        this->saved_regs.rbx = emu.reg(x86_register::rbx);
        this->saved_regs.rcx = emu.reg(x86_register::rcx);
        this->saved_regs.rdx = emu.reg(x86_register::rdx);
        this->saved_regs.rsi = emu.reg(x86_register::rsi);
        this->saved_regs.rdi = emu.reg(x86_register::rdi);
        this->saved_regs.rbp = emu.reg(x86_register::rbp);
        this->saved_regs.rsp = emu.reg(x86_register::rsp);
        this->saved_regs.r8 = emu.reg(x86_register::r8);
        this->saved_regs.r9 = emu.reg(x86_register::r9);
        this->saved_regs.r10 = emu.reg(x86_register::r10);
        this->saved_regs.r11 = emu.reg(x86_register::r11);
        this->saved_regs.r12 = emu.reg(x86_register::r12);
        this->saved_regs.r13 = emu.reg(x86_register::r13);
        this->saved_regs.r14 = emu.reg(x86_register::r14);
        this->saved_regs.r15 = emu.reg(x86_register::r15);
        this->saved_regs.rip = emu.reg(x86_register::rip);
        this->saved_regs.rflags = emu.reg(x86_register::rflags);
        this->saved_regs.fs_base = emu.get_segment_base(x86_register::fs);
        this->saved_regs.gs_base = emu.get_segment_base(x86_register::gs);
    }

    void restore(x86_64_emulator& emu) const
    {
        emu.reg(x86_register::rax, this->saved_regs.rax);
        emu.reg(x86_register::rbx, this->saved_regs.rbx);
        emu.reg(x86_register::rcx, this->saved_regs.rcx);
        emu.reg(x86_register::rdx, this->saved_regs.rdx);
        emu.reg(x86_register::rsi, this->saved_regs.rsi);
        emu.reg(x86_register::rdi, this->saved_regs.rdi);
        emu.reg(x86_register::rbp, this->saved_regs.rbp);
        emu.reg(x86_register::rsp, this->saved_regs.rsp);
        emu.reg(x86_register::r8, this->saved_regs.r8);
        emu.reg(x86_register::r9, this->saved_regs.r9);
        emu.reg(x86_register::r10, this->saved_regs.r10);
        emu.reg(x86_register::r11, this->saved_regs.r11);
        emu.reg(x86_register::r12, this->saved_regs.r12);
        emu.reg(x86_register::r13, this->saved_regs.r13);
        emu.reg(x86_register::r14, this->saved_regs.r14);
        emu.reg(x86_register::r15, this->saved_regs.r15);
        emu.reg(x86_register::rip, this->saved_regs.rip);
        emu.reg(x86_register::rflags, this->saved_regs.rflags);
        emu.set_segment_base(x86_register::fs, this->saved_regs.fs_base);
        emu.set_segment_base(x86_register::gs, this->saved_regs.gs_base);
    }
};
