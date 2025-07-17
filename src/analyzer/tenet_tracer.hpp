#pragma once

#include <windows_emulator.hpp>
#include <emulator/x86_register.hpp>
#include <emulator/scoped_hook.hpp>
#include <fstream>
#include <string>
#include <vector>
#include <array>
#include <sstream>
#include <string_view>

// List of registers to trace for the x64 architecture.
constexpr std::array<std::pair<x86_register, std::string_view>, 16> GPRs_TO_TRACE = {
    {
        {x86_register::rax, "rax"},
        {x86_register::rbx, "rbx"},
        {x86_register::rcx, "rcx"},
        {x86_register::rdx, "rdx"},
        {x86_register::rsi, "rsi"},
        {x86_register::rdi, "rdi"},
        {x86_register::rbp, "rbp"},
        {x86_register::rsp, "rsp"},
        {x86_register::r8, "r8"},
        {x86_register::r9, "r9"},
        {x86_register::r10, "r10"},
        {x86_register::r11, "r11"},
        {x86_register::r12, "r12"},
        {x86_register::r13, "r13"},
        {x86_register::r14, "r14"},
        {x86_register::r15, "r15"},
    },
};

class TenetTracer
{
  public:
    TenetTracer(windows_emulator& win_emu, const std::string& log_filename);
    ~TenetTracer();

    void process_instruction(uint64_t address);

  private:
    void filter_and_write_buffer();
    void log_memory_read(uint64_t address, const void* data, size_t size);
    void log_memory_write(uint64_t address, const void* data, size_t size);

    windows_emulator& m_win_emu;
    std::ofstream m_log_file;

    // In-memory buffering for performance.
    std::vector<std::string> m_raw_log_buffer;

    // Use an array instead of a map to store the register state of the previous instruction.
    std::array<uint64_t, GPRs_TO_TRACE.size()> m_previous_regs{};
    bool m_is_first_instruction = true;

    // To temporarily store memory operations.
    std::stringstream m_mem_read_log;
    std::stringstream m_mem_write_log;

    // To manage memory hooks.
    scoped_hook m_read_hook;
    scoped_hook m_write_hook;
};
