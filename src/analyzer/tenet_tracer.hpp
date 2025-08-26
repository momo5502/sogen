#pragma once

#include <windows_emulator.hpp>
#include <emulator/x86_register.hpp>
#include <emulator/scoped_hook.hpp>

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

class tenet_tracer
{
  public:
    tenet_tracer(windows_emulator& win_emu, const std::filesystem::path& log_filename,
                 const std::set<std::string, std::less<>>& modules);
    ~tenet_tracer();

    tenet_tracer(tenet_tracer&) = delete;
    tenet_tracer(const tenet_tracer&) = delete;
    tenet_tracer& operator=(tenet_tracer&) = delete;
    tenet_tracer& operator=(const tenet_tracer&) = delete;

    void on_module_load(const mapped_module& mod);
    void on_module_unload(const mapped_module& mod);

  private:
    void setup_tracing_hooks();

    void filter_and_write_buffer();
    void log_memory_read(uint64_t address, const void* data, size_t size);
    void log_memory_write(uint64_t address, const void* data, size_t size);
    void process_instruction(uint64_t address);

    windows_emulator& win_emu_;
    std::ofstream log_file_;
    const std::set<std::string, std::less<>>& traced_modules_;

    std::vector<std::string> raw_log_buffer_;
    std::array<uint64_t, GPRs_TO_TRACE.size()> previous_registers_{};
    bool is_first_instruction_ = true;
    bool tracing_active_ = false;
    uint64_t traced_module_base_{};

    std::stringstream mem_read_log_;
    std::stringstream mem_write_log_;

    scoped_hook read_hook_;
    scoped_hook write_hook_;
    scoped_hook execute_hook_;
};