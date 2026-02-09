#pragma once

#include "std_include.hpp"

#include <arch_emulator.hpp>
#include <platform/compiler.hpp>

#include "linux_logger.hpp"
#include "linux_file_system.hpp"
#include "linux_memory_manager.hpp"
#include "linux_process_context.hpp"
#include "linux_syscall_dispatcher.hpp"
#include "signal_dispatch.hpp"
#include "procfs.hpp"
#include "vdso.hpp"
#include "module/linux_module_manager.hpp"

class linux_emulator
{
    uint64_t executed_instructions_{0};
    std::unique_ptr<x86_64_emulator> emu_{};

  public:
    std::filesystem::path emulation_root{};
    linux_logger log{};
    linux_file_system file_sys{};
    linux_memory_manager memory;
    linux_module_manager mod_manager;
    linux_process_context process{};
    linux_syscall_dispatcher dispatcher{};
    signal_dispatcher signals{};
    procfs proc_fs{};
    linux_vdso vdso{};

    linux_emulator(std::unique_ptr<x86_64_emulator> emu, const std::filesystem::path& emulation_root,
                   const std::filesystem::path& executable, std::vector<std::string> argv, std::vector<std::string> envp);

    linux_emulator(const linux_emulator&) = delete;
    linux_emulator& operator=(const linux_emulator&) = delete;
    linux_emulator(linux_emulator&&) = delete;
    linux_emulator& operator=(linux_emulator&&) = delete;

    ~linux_emulator() = default;

    x86_64_emulator& emu()
    {
        return *this->emu_;
    }

    const x86_64_emulator& emu() const
    {
        return *this->emu_;
    }

    // Callbacks for emulated stdout/stderr output
    std::function<void(std::string_view data)> on_stdout{};
    std::function<void(std::string_view data)> on_stderr{};

    // Callback invoked periodically during emulation (every 0x20000 instructions).
    // Used by the web debugger for ASYNCIFY yield and event handling.
    std::function<void()> on_periodic_event{};

    void start(size_t count = 0);
    void stop();

    uint64_t get_executed_instructions() const
    {
        return this->executed_instructions_;
    }

  private:
    std::atomic_bool should_stop{false};

    void setup_hooks();
    void on_instruction_execution(uint64_t address);
    void resolve_irelative_relocations();
};
