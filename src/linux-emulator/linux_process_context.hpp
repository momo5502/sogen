#pragma once

#include "std_include.hpp"
#include "linux_fd_table.hpp"
#include "linux_thread.hpp"
#include "linux_memory_manager.hpp"
#include "module/elf_mapping.hpp"

#include <arch_emulator.hpp>

struct linux_process_context
{
    linux_fd_table fds{};

    uint64_t brk_base{};
    uint64_t brk_current{};

    std::map<uint32_t, linux_thread> threads{};
    linux_thread* active_thread{};

    uint32_t pid{1};
    uint32_t ppid{0};

    uint32_t uid{1000};
    uint32_t gid{1000};
    uint32_t euid{1000};
    uint32_t egid{1000};

    std::optional<int> exit_status{};

    uint32_t next_tid{1};

    // Stored for procfs emulation
    std::vector<std::string> argv{};
    std::vector<std::string> envp{};

    uint32_t create_thread(uint64_t stack_base, uint64_t stack_size, uint64_t entry_point, uint64_t fs_base = 0)
    {
        const auto tid = this->next_tid++;

        linux_thread thread{};
        thread.tid = tid;
        thread.stack_base = stack_base;
        thread.stack_size = stack_size;
        thread.fs_base = fs_base;
        thread.saved_regs.rip = entry_point;
        thread.saved_regs.rsp = stack_base + stack_size;

        this->threads.emplace(tid, std::move(thread));
        return tid;
    }

    void setup(x86_64_emulator& emu, linux_memory_manager& memory, const linux_mapped_module& exe, const std::vector<std::string>& argv,
               const std::vector<std::string>& envp, uint64_t interpreter_base = 0, uint64_t initial_rip = 0, uint64_t vdso_base = 0);
};
