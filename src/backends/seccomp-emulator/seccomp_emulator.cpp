#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "seccomp_emulator.hpp"
#include "seccomp_single_step.hpp"

#if !defined(__aarch64__) || !defined(__linux__)
#error "Seccomp requires an ARM64 Linux or Android host"
#endif

#include <utils/finally.hpp>

#include <asm/sigcontext.h>
#include <asm/hwcap.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/mman.h>
#include <sys/auxv.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <ucontext.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <csetjmp>
#include <csignal>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>
#include <system_error>

extern "C"
{
    __attribute__((visibility("hidden"))) uint64_t sogen_seccomp_host_x18{};
    __attribute__((visibility("hidden"))) uint64_t sogen_seccomp_host_tls{};
    void sogen_seccomp_signal_entry(int signal, siginfo_t* info, void* context);
}

namespace sogen::seccomp
{
    namespace
    {
        constexpr uint64_t window_begin = 0x10000;
        constexpr uint64_t window_end = 0x1000000000;
        constexpr size_t page_size = 0x1000;
        constexpr int enter_signal = SIGUSR2;
        constexpr int stop_signal = SIGUSR1;
        constexpr std::array signals{enter_signal, stop_signal, SIGSYS, SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGFPE};

        [[noreturn]] void unsupported(const char* feature)
        {
            throw std::runtime_error(std::string{"Seccomp does not support "} + feature);
        }

        void check_system(const int result, const char* operation)
        {
            if (result < 0)
            {
                throw std::system_error(errno, std::generic_category(), operation);
            }
        }

        bool in_window(const uint64_t address, const size_t size)
        {
            return address >= window_begin && address < window_end && size <= window_end - address;
        }

        int protection(const memory_permission permissions)
        {
            if ((permissions & memory_permission::guard) != memory_permission::none)
            {
                return PROT_NONE;
            }
            int result = PROT_NONE;
            if ((permissions & memory_permission::read) != memory_permission::none)
            {
                result |= PROT_READ;
            }
            if ((permissions & memory_permission::write) != memory_permission::none)
            {
                result |= PROT_WRITE;
            }
            if (is_executable(permissions))
            {
                result |= PROT_EXEC;
            }
            return result;
        }

        struct instance_claim
        {
            inline static std::atomic_flag claimed{};

            instance_claim()
            {
                if (claimed.test_and_set())
                {
                    unsupported("multiple emulator instances in one process");
                }
            }

            ~instance_claim()
            {
                claimed.clear();
            }
        };

        struct guest_window
        {
            void* address{};

            guest_window()
            {
                if (sysconf(_SC_PAGESIZE) != page_size)
                {
                    unsupported("host page sizes other than 4 KiB");
                }
                address = mmap(reinterpret_cast<void*>(window_begin), window_end - window_begin, PROT_NONE,
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
                if (address == MAP_FAILED)
                {
                    throw std::system_error(errno, std::generic_category(), "Reserve seccomp guest window");
                }
                if (reinterpret_cast<uint64_t>(address) != window_begin)
                {
                    munmap(address, window_end - window_begin);
                    throw std::runtime_error("Seccomp guest window collides with host memory");
                }
            }

            ~guest_window()
            {
                munmap(address, window_end - window_begin);
            }
        };

        struct signal_environment
        {
            std::array<std::byte, 0x10000> stack{};
            stack_t previous_stack{};
            std::array<struct sigaction, signals.size()> previous_actions{};
            sigset_t previous_mask{};

            signal_environment()
            {
                stack_t alternate{};
                alternate.ss_sp = stack.data();
                alternate.ss_size = stack.size();
                check_system(sigaltstack(&alternate, &previous_stack), "sigaltstack");
                size_t installed{};
                try
                {
                    struct sigaction action{};
                    action.sa_sigaction = sogen_seccomp_signal_entry;
                    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
                    sigemptyset(&action.sa_mask);
                    for (const auto signal : signals)
                    {
                        sigaddset(&action.sa_mask, signal);
                    }
                    for (const auto signal : signals)
                    {
                        check_system(sigaction(signal, &action, &previous_actions[installed]), "sigaction");
                        ++installed;
                    }
                    check_system(sigprocmask(SIG_UNBLOCK, &action.sa_mask, &previous_mask), "Unblock seccomp signals");
                }
                catch (...)
                {
                    while (installed != 0)
                    {
                        --installed;
                        sigaction(signals[installed], &previous_actions[installed], nullptr);
                    }
                    sigaltstack(&previous_stack, nullptr);
                    throw;
                }
            }

            ~signal_environment()
            {
                for (size_t i = 0; i < signals.size(); ++i)
                {
                    sigaction(signals[i], &previous_actions[i], nullptr);
                }
                sigaltstack(&previous_stack, nullptr);
                sigprocmask(SIG_SETMASK, &previous_mask, nullptr);
            }
        };

        fpsimd_context* find_fpsimd(ucontext_t& context)
        {
            auto* cursor = reinterpret_cast<std::byte*>(context.uc_mcontext.__reserved);
            const auto* end = cursor + sizeof(context.uc_mcontext.__reserved);
            while (static_cast<size_t>(end - cursor) >= sizeof(_aarch64_ctx))
            {
                auto* header = reinterpret_cast<_aarch64_ctx*>(cursor);
                if (header->size < sizeof(_aarch64_ctx) || header->size > static_cast<size_t>(end - cursor))
                {
                    break;
                }
                if (header->magic == FPSIMD_MAGIC && header->size >= sizeof(fpsimd_context))
                {
                    return reinterpret_cast<fpsimd_context*>(cursor);
                }
                cursor += header->size;
            }
            return nullptr;
        }

        struct decoded_load
        {
            size_t size{};
            uint32_t destination{};
            bool sign_extend{};
            bool wide{};
            bool vector{};
            bool acquire{};
            bool writeback{};
            size_t post_increment{};
            uint32_t byte_lane_registers{};
        };

        std::optional<decoded_load> decode_mmio_load(const uint32_t instruction)
        {
            const auto size = instruction >> 30;
            const auto destination = instruction & 31;
            constexpr uint32_t ld1_vector_postindex = 0x4CDF7000;
            if ((instruction & 0xFFFFF000u) == ld1_vector_postindex)
            {
                return decoded_load{.size = 16, .destination = destination, .vector = true, .writeback = true, .post_increment = 16};
            }
            const auto byte_lane = instruction & 0xFFFFFC00u;
            if (byte_lane == 0x0DFF0000u || byte_lane == 0x0DFF2000u)
            {
                const uint32_t count = byte_lane == 0x0DFF0000u ? 2 : 4;
                return decoded_load{.size = count,
                                    .destination = destination,
                                    .vector = true,
                                    .writeback = true,
                                    .post_increment = count,
                                    .byte_lane_registers = count};
            }
            const auto acquire = instruction & 0x3FFFFC00u;
            if (acquire == 0x08DFFC00u || acquire == 0x38BFC000u)
            {
                return decoded_load{.size = size_t{1} << size, .destination = destination, .wide = size == 3, .acquire = true};
            }

            const bool unsigned_offset = (instruction & 0x3B000000u) == 0x39000000u;
            const bool unscaled = (instruction & 0x3B200C00u) == 0x38000000u;
            const bool register_offset = (instruction & 0x3B200C00u) == 0x38200800u && (instruction & 0x4000u) != 0;
            const bool acquire_unscaled = (instruction & 0x3F200C00u) == 0x19000000u;
            const bool indexed = (instruction & 0x3B200400u) == 0x38000400u;
            if (!unsigned_offset && !unscaled && !register_offset && !acquire_unscaled && !indexed)
            {
                return std::nullopt;
            }

            const auto opcode = (instruction >> 22) & 3;
            const bool vector = (instruction & (1u << 26)) != 0;
            const auto base = (instruction >> 5) & 31;
            if (indexed && !vector && base == destination && base != 31)
            {
                return std::nullopt;
            }
            if (vector)
            {
                if (opcode == 1 || (opcode == 3 && size == 0))
                {
                    return decoded_load{
                        .size = opcode == 3 ? 16 : size_t{1} << size, .destination = destination, .vector = true, .writeback = indexed};
                }
            }
            else if (opcode == 1 || (opcode == 2 && size < 3) || (opcode == 3 && size < 2))
            {
                return decoded_load{.size = size_t{1} << size,
                                    .destination = destination,
                                    .sign_extend = opcode != 1,
                                    .wide = opcode == 2 || size == 3,
                                    .acquire = acquire_unscaled,
                                    .writeback = indexed};
            }
            return std::nullopt;
        }

        struct decoded_load_address
        {
            uint64_t address;
            std::optional<uint64_t> writeback{};
        };

        decoded_load_address mmio_load_address(const register_state& state, const uint32_t instruction, const decoded_load& load)
        {
            const auto base = (instruction >> 5) & 31;
            const auto address = base == 31 ? state.sp : state.x[base];
            if (load.post_increment != 0)
            {
                return {address, address + load.post_increment};
            }
            if ((instruction & 0x3B000000u) == 0x39000000u)
            {
                return {address + ((instruction >> 10) & 0xFFF) * load.size};
            }
            if ((instruction & 0x3B200C00u) == 0x38200800u)
            {
                const auto index = (instruction >> 16) & 31;
                uint64_t offset = index == 31 ? 0 : state.x[index];
                const auto option = (instruction >> 13) & 7;
                if (option == 2)
                {
                    offset = static_cast<uint32_t>(offset);
                }
                else if (option == 6)
                {
                    offset = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(offset)));
                }
                return {address + offset * ((instruction & 0x1000) != 0 ? load.size : 1)};
            }
            if (load.writeback || (instruction & 0x3B200C00u) == 0x38000000u || (instruction & 0x3F200C00u) == 0x19000000u)
            {
                const auto immediate = (instruction >> 12) & 0x1FF;
                const auto adjusted = address + static_cast<uint64_t>(static_cast<int64_t>(immediate ^ 0x100) - 0x100);
                if (load.writeback)
                {
                    return {(instruction & 0x800) != 0 ? adjusted : address, adjusted};
                }
                return {adjusted};
            }
            return {address};
        }

        void install_filter()
        {
            // ARM64 seccomp reports the PC after SVC; the last 64 KiB of the window is never allocated by the memory manager.
            sock_filter filter[] = {
                BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, arch)),
                BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_AARCH64, 1, 0),
                BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
                BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, instruction_pointer) + 4),
                BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, window_end >> 32, 1, 0),
                BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRAP),
                BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
            };
            sock_fprog program{static_cast<unsigned short>(std::size(filter)), filter};
            check_system(prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0), "PR_SET_NO_NEW_PRIVS");
            check_system(static_cast<int>(syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &program)), "Install seccomp filter");
        }

        class seccomp_emulator final : public arm64_emulator
        {
          public:
            inline static seccomp_emulator* active{};

            seccomp_emulator()
                : owner_tid_(static_cast<pid_t>(syscall(SYS_gettid)))
            {
                if (reinterpret_cast<uint64_t>(&sogen_seccomp_signal_entry) < window_end)
                {
                    throw std::runtime_error("Seccomp host code must be above the guest window");
                }
                asm volatile("mov %0, x18" : "=r"(sogen_seccomp_host_x18));
                asm volatile("mrs %0, tpidr_el0" : "=r"(sogen_seccomp_host_tls));
                install_filter();
                active = this;
            }

            ~seccomp_emulator() override
            {
                active = nullptr;
            }

            std::string get_name() const override
            {
                return "Seccomp";
            }

            bool supports_multiple_vcpus() const override
            {
                return false;
            }

            bool supports_instruction_counting() const override
            {
                return false;
            }

            bool is_stop_thread_safe() const override
            {
                return true;
            }

            bool supports_global_memory_execution_hooks() const override
            {
                return false;
            }

            bool supports_memory_range_execution_hooks() const override
            {
                return false;
            }

            bool supports_interrupt_hooks() const override
            {
                return true;
            }

            bool supports_memory_violation_hooks() const override
            {
                return false;
            }

            bool supports_restartable_faults() const override
            {
                return false;
            }

            bool supports_native_timestamp_counter() const override
            {
                return true;
            }

            std::optional<uint64_t> native_timestamp_counter_frequency() const override
            {
                uint64_t frequency{};
                asm volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
                return frequency;
            }

            std::optional<uint64_t> read_native_timestamp_counter() override
            {
                uint64_t ticks{};
                asm volatile("isb; mrs %0, cntvct_el0" : "=r"(ticks) : : "memory");
                return ticks;
            }

            uint64_t guest_address_end() const override
            {
                return window_end;
            }

            uint64_t host_placement_address(const uint64_t address) const override
            {
                return address;
            }

            void serialize_state(utils::buffer_serializer&, bool) const override
            {
                unsupported("snapshots");
            }

            void deserialize_state(utils::buffer_deserializer&, bool) override
            {
                unsupported("snapshots");
            }

            std::vector<std::byte> save_registers() const override
            {
                std::vector<std::byte> result(sizeof(state_));
                std::memcpy(result.data(), &state_, sizeof(state_));
                return result;
            }

            void restore_registers(const std::vector<std::byte>& data) override
            {
                if (data.size() != sizeof(state_))
                {
                    throw std::invalid_argument("Invalid seccomp register state");
                }
                std::memcpy(&state_, data.data(), sizeof(state_));
            }

            size_t read_raw_register(const int reg, void* value, const size_t size) override
            {
                const auto id = static_cast<arm64_register>(reg);
                if (id == arm64_register::midr_el1 || id == arm64_register::id_aa64isar0_el1 || id == arm64_register::id_aa64isar1_el1)
                {
                    if (!value || size < sizeof(uint64_t))
                    {
                        throw std::invalid_argument("Seccomp register buffer is too small or null");
                    }
                    if ((getauxval(AT_HWCAP) & HWCAP_CPUID) == 0)
                    {
                        unsupported("host kernels without userspace CPU identification registers");
                    }
                    uint64_t identification{};
                    switch (id)
                    {
                    case arm64_register::midr_el1:
                        asm volatile("mrs %0, midr_el1" : "=r"(identification));
                        break;
                    case arm64_register::id_aa64isar0_el1:
                        asm volatile("mrs %0, id_aa64isar0_el1" : "=r"(identification));
                        break;
                    default:
                        asm volatile("mrs %0, id_aa64isar1_el1" : "=r"(identification));
                        break;
                    }
                    std::memset(value, 0, size);
                    std::memcpy(value, &identification, sizeof(identification));
                    return sizeof(identification);
                }
                auto [address, width] = register_storage(id);
                if (!value || size < width)
                {
                    throw std::invalid_argument("Seccomp register buffer is too small or null");
                }
                std::memset(value, 0, size);
                std::memcpy(value, address, width);
                if (id == arm64_register::nzcv)
                {
                    const auto nzcv = state_.pstate & 0xF0000000;
                    std::memcpy(value, &nzcv, width);
                }
                return width;
            }

            size_t write_raw_register(const int reg, const void* value, const size_t size) override
            {
                const auto id = static_cast<arm64_register>(reg);
                auto [address, width] = register_storage(id);
                if (!value || size < width)
                {
                    throw std::invalid_argument("Seccomp register buffer is too small or null");
                }
                if (id >= arm64_register::bcr0 && id <= arm64_register::wvr1)
                {
                    uint64_t enabled{};
                    std::memcpy(&enabled, value, width);
                    if (enabled != 0)
                    {
                        unsupported("hardware breakpoints and watchpoints");
                    }
                    return width;
                }
                if (id == arm64_register::pstate || id == arm64_register::nzcv)
                {
                    uint64_t flags{};
                    std::memcpy(&flags, value, std::min(size, sizeof(flags)));
                    const auto allowed = id == arm64_register::pstate ? pstate_nzcv | pstate_software_step : pstate_nzcv;
                    if ((flags & ~allowed) != 0)
                    {
                        unsupported("requested PSTATE bits");
                    }
                    state_.pstate = id == arm64_register::nzcv ? (state_.pstate & ~pstate_nzcv) | flags : flags;
                    return width;
                }
                if ((id >= arm64_register::w0 && id <= arm64_register::w30) || id == arm64_register::wsp)
                {
                    std::memset(address, 0, sizeof(uint64_t));
                }
                std::memcpy(address, value, width);
                return width;
            }

            bool has_violation() const override
            {
                return violation_;
            }

            void start(const size_t count) override
            {
                if (count != 0)
                {
                    unsupported("instruction-count execution budgets");
                }
                if (syscall(SYS_gettid) != owner_tid_)
                {
                    unsupported("moving a vCPU between host threads");
                }
                if (!svc_hook_)
                {
                    throw std::logic_error("Seccomp requires an SVC hook");
                }
                stop_requested_ = false;
                violation_ = false;
                running_ = true;
                const auto clear_running = utils::finally([&] { running_ = false; });
                uint64_t host_fpcr{};
                uint64_t host_fpsr{};
                asm volatile("mrs %0, fpcr; mrs %1, fpsr" : "=r"(host_fpcr), "=r"(host_fpsr));
                while (!stop_requested_)
                {
                    if (!in_window(state_.pc, 4) || state_.pc % 4 != 0 || state_.sp % 16 != 0)
                    {
                        throw std::runtime_error("Invalid seccomp entry PC or SP");
                    }
                    const bool single_step = (state_.pstate & pstate_software_step) != 0;
                    if (single_step)
                    {
                        if (!interrupt_hook_)
                        {
                            throw std::logic_error("Seccomp requires an interrupt hook for single-stepping");
                        }
                        uint32_t instruction{};
                        read_memory(state_.pc, &instruction, sizeof(instruction));
                        if ((instruction & 0xFFE0001F) == 0xD4000001)
                        {
                            // SVC raises its syscall exception instead of a software-step exception.
                            state_.pstate &= ~pstate_software_step;
                            state_.pc += 4;
                            dispatch_svc();
                            continue;
                        }
                        single_step_result result{};
                        {
                            const std::scoped_lock lock(step_mutex_);
                            if (stop_requested_)
                            {
                                break;
                            }
                            result = single_step_guest(state_, enter_signal);
                        }
                        state_.pstate &= ~pstate_software_step;
                        if (result.signal == SIGTRAP && result.info.si_code == TRAP_TRACE)
                        {
                            const auto callback = interrupt_hook_;
                            callback(*this, static_cast<int>(arm64_exception_class::software_step));
                            continue;
                        }
                        trapped_signal_ = result.signal;
                        trapped_code_ = result.info.si_code;
                        trapped_arch_ = result.signal == SIGSYS ? result.info.si_arch : 0;
                        trapped_address_ = reinterpret_cast<uint64_t>(result.info.si_addr);
                    }
                    else
                    {
                        if (sigsetjmp(host_return_, 1) == 0)
                        {
                            check_system(static_cast<int>(syscall(SYS_tgkill, getpid(), owner_tid_, enter_signal)), "Enter seccomp");
                            if (stop_requested_)
                            {
                                break;
                            }
                            throw std::runtime_error("Seccomp entry signal did not enter the guest");
                        }
                        asm volatile("msr fpcr, %0; msr fpsr, %1" : : "r"(host_fpcr), "r"(host_fpsr) : "memory");
                        if (missing_fpsimd_)
                        {
                            unsupported("signal frames without FPSIMD state");
                        }
                    }
                    if (trapped_signal_ == stop_signal)
                    {
                        break;
                    }
                    if ((trapped_signal_ == SIGSEGV || trapped_signal_ == SIGBUS) && trapped_code_ > 0 && handle_mmio_fault())
                    {
                        if (single_step)
                        {
                            const auto callback = interrupt_hook_;
                            callback(*this, static_cast<int>(arm64_exception_class::software_step));
                        }
                        continue;
                    }
                    if (trapped_signal_ == SIGSYS && trapped_code_ == 1 && trapped_arch_ == AUDIT_ARCH_AARCH64)
                    {
                        dispatch_svc();
                        continue;
                    }
                    if (trapped_signal_ == SIGILL && trapped_code_ > 0 && interrupt_hook_)
                    {
                        const auto callback = interrupt_hook_;
                        callback(*this, static_cast<int>(arm64_exception_class::undefined_instruction));
                        continue;
                    }
                    violation_ = true;
                    std::ostringstream message{};
                    message << "Seccomp guest signal " << trapped_signal_ << " (code " << trapped_code_ << ") at PC 0x" << std::hex
                            << state_.pc << ", address 0x" << trapped_address_;
                    throw std::runtime_error(message.str());
                }
            }

            void stop() override
            {
                const std::scoped_lock lock(step_mutex_);
                stop_requested_ = true;
                if (running_ && syscall(SYS_gettid) != owner_tid_)
                {
                    check_system(static_cast<int>(syscall(SYS_tgkill, getpid(), owner_tid_, stop_signal)), "Stop seccomp");
                }
            }

            void handle_signal(const int signal, const siginfo_t& info, ucontext_t& context)
            {
                auto& machine = context.uc_mcontext;
                if (signal == enter_signal && running_ && !stop_requested_)
                {
                    auto* fp = find_fpsimd(context);
                    if (!fp)
                    {
                        missing_fpsimd_ = true;
                        siglongjmp(host_return_, 1);
                    }
                    for (size_t i = 0; i < state_.x.size(); ++i)
                    {
                        machine.regs[i] = state_.x[i];
                    }
                    machine.sp = state_.sp;
                    machine.pc = state_.pc;
                    machine.pstate = state_.pstate & 0xF0000000;
                    std::memcpy(fp->vregs, state_.vectors.data(), sizeof(fp->vregs));
                    fp->fpcr = state_.fpcr;
                    fp->fpsr = state_.fpsr;
                    return;
                }
                if (signal == enter_signal || (signal == stop_signal && !in_window(machine.pc, 1)))
                {
                    return;
                }
                if (!running_ || !in_window(machine.pc, 1))
                {
                    constexpr char error[] = "Seccomp: signal outside guest execution\n";
                    write(STDERR_FILENO, error, sizeof(error) - 1);
                    _exit(128 + signal);
                }
                for (size_t i = 0; i < state_.x.size(); ++i)
                {
                    state_.x[i] = machine.regs[i];
                }
                state_.sp = machine.sp;
                state_.pc = machine.pc;
                state_.pstate = machine.pstate & 0xF0000000;
                auto* fp = find_fpsimd(context);
                missing_fpsimd_ = fp == nullptr;
                if (fp)
                {
                    std::memcpy(state_.vectors.data(), fp->vregs, sizeof(fp->vregs));
                    state_.fpcr = fp->fpcr;
                    state_.fpsr = fp->fpsr;
                }
                trapped_signal_ = signal;
                trapped_code_ = info.si_code;
                trapped_arch_ = signal == SIGSYS ? info.si_arch : 0;
                trapped_address_ = reinterpret_cast<uint64_t>(info.si_addr);
                siglongjmp(host_return_, 1);
            }

            void read_memory(const uint64_t address, void* data, const size_t size) const override
            {
                if (!try_read_memory(address, data, size))
                {
                    throw std::runtime_error("Seccomp read from unmapped memory");
                }
            }

            bool try_read_memory(const uint64_t address, void* data, const size_t size) const override
            {
                return copy_memory(address, data, size, false);
            }

            void write_memory(const uint64_t address, const void* data, const size_t size) override
            {
                if (!try_write_memory(address, data, size))
                {
                    throw std::runtime_error("Seccomp write to unmapped memory");
                }
            }

            bool try_write_memory(const uint64_t address, const void* data, const size_t size) override
            {
                return copy_memory(address, const_cast<void*>(data), size, true);
            }

            emulator_hook* hook_instruction(const int instruction, instruction_hook_callback callback) override
            {
                if (instruction != static_cast<int>(arm64_hookable_instructions::svc))
                {
                    unsupported("counter interception");
                }
                if (!callback || svc_hook_)
                {
                    throw std::invalid_argument("Seccomp requires exactly one nonempty SVC hook");
                }
                svc_hook_ = std::move(callback);
                return reinterpret_cast<emulator_hook*>(&svc_hook_);
            }

            emulator_hook* hook_interrupt(interrupt_hook_callback callback) override
            {
                if (!callback || interrupt_hook_)
                {
                    throw std::invalid_argument("Seccomp requires exactly one nonempty interrupt hook");
                }
                interrupt_hook_ = std::move(callback);
                return reinterpret_cast<emulator_hook*>(&interrupt_hook_);
            }

            emulator_hook* hook_memory_violation(memory_violation_hook_callback) override
            {
                unsupported("recoverable guest memory faults");
            }

            emulator_hook* hook_memory_execution(memory_execution_hook_callback) override
            {
                unsupported("execution hooks");
            }

            emulator_hook* hook_memory_execution(uint64_t, memory_execution_hook_callback) override
            {
                unsupported("execution hooks");
            }

            emulator_hook* hook_memory_range_execution(uint64_t, uint64_t, memory_execution_hook_callback) override
            {
                unsupported("execution hooks");
            }

            emulator_hook* hook_memory_read(uint64_t, uint64_t, memory_access_hook_callback) override
            {
                return add_ignored_hook();
            }

            emulator_hook* hook_memory_write(uint64_t, uint64_t, memory_access_hook_callback) override
            {
                return add_ignored_hook();
            }

            emulator_hook* hook_basic_block(basic_block_hook_callback) override
            {
                return add_ignored_hook();
            }

            void delete_hook(emulator_hook* hook) override
            {
                if (hook == reinterpret_cast<emulator_hook*>(&svc_hook_) && svc_hook_)
                {
                    svc_hook_ = {};
                    return;
                }
                if (hook == reinterpret_cast<emulator_hook*>(&interrupt_hook_) && interrupt_hook_)
                {
                    interrupt_hook_ = {};
                    return;
                }
                const auto ignored = std::ranges::find_if(
                    ignored_hooks_, [hook](const auto& entry) { return hook == reinterpret_cast<emulator_hook*>(entry.get()); });
                if (ignored == ignored_hooks_.end())
                {
                    throw std::invalid_argument("Unknown seccomp hook");
                }
                ignored_hooks_.erase(ignored);
            }

          private:
            instance_claim claim_{};
            guest_window window_{};
            std::unique_ptr<signal_environment> signals_ = std::make_unique<signal_environment>();
            pid_t owner_tid_{};
            std::atomic<bool> running_{};
            std::atomic<bool> stop_requested_{};
            std::mutex step_mutex_{};
            register_state state_{};
            uint64_t zero_{};
            sigjmp_buf host_return_{};
            int trapped_signal_{};
            int trapped_code_{};
            uint32_t trapped_arch_{};
            uint64_t trapped_address_{};
            bool missing_fpsimd_{};
            bool violation_{};
            instruction_hook_callback svc_hook_{};
            interrupt_hook_callback interrupt_hook_{};
            std::vector<std::unique_ptr<std::byte>> ignored_hooks_{};
            std::map<uint64_t, memory_permission> pages_{};

            struct mmio_region
            {
                uint64_t address;
                size_t size;
                mmio_read_callback read;
            };

            std::vector<mmio_region> mmio_regions_{};

            std::vector<mmio_region>::const_iterator find_mmio(const uint64_t address, const size_t size) const
            {
                return std::ranges::find_if(mmio_regions_, [&](const auto& region) {
                    return address < region.address + region.size && region.address < address + size;
                });
            }

            bool handle_mmio_fault()
            {
                if (trapped_address_ == state_.pc || !in_window(trapped_address_, 1))
                {
                    return false;
                }
                const auto region = find_mmio(trapped_address_, 1);
                if (region == mmio_regions_.end())
                {
                    return false;
                }
                uint32_t instruction{};
                read_memory(state_.pc, &instruction, sizeof(instruction));
                const auto load = decode_mmio_load(instruction);
                const auto address = load ? mmio_load_address(state_, instruction, *load) : decoded_load_address{};
                if (!load || load->size > region->size - (trapped_address_ - region->address) || address.address != trapped_address_)
                {
                    violation_ = true;
                    std::ostringstream message{};
                    message << "Unsupported seccomp MMIO instruction 0x" << std::hex << instruction << " at PC 0x" << state_.pc
                            << ", address 0x" << trapped_address_;
                    throw std::runtime_error(message.str());
                }
                std::array<std::byte, 16> data{};
                region->read(trapped_address_ - region->address, data.data(), load->size);
                if (load->acquire)
                {
                    std::atomic_thread_fence(std::memory_order_acquire);
                }
                if (load->vector)
                {
                    if (load->byte_lane_registers != 0)
                    {
                        for (uint32_t i = 0; i < load->byte_lane_registers; ++i)
                        {
                            state_.vectors[(load->destination + i) % state_.vectors.size()][0] = data[i];
                        }
                    }
                    else
                    {
                        state_.vectors[load->destination] = data;
                    }
                }
                else if (load->destination != 31)
                {
                    uint64_t value{};
                    std::memcpy(&value, data.data(), load->size);
                    if (load->sign_extend)
                    {
                        const auto sign = uint64_t{1} << (load->size * 8 - 1);
                        value = (value ^ sign) - sign;
                    }
                    state_.x[load->destination] = load->wide ? value : static_cast<uint32_t>(value);
                }
                if (address.writeback)
                {
                    const auto base = (instruction >> 5) & 31;
                    (base == 31 ? state_.sp : state_.x[base]) = *address.writeback;
                }
                state_.pc += 4;
                return true;
            }

            emulator_hook* add_ignored_hook()
            {
                // FEX also accepts memory read/write and basic-block hooks without delivering their callbacks.
                auto& hook = ignored_hooks_.emplace_back(std::make_unique<std::byte>());
                return reinterpret_cast<emulator_hook*>(hook.get());
            }

            std::pair<void*, size_t> register_storage(const arm64_register id)
            {
                if (id >= arm64_register::x0 && id <= arm64_register::x28)
                {
                    return {&state_.x[static_cast<int>(id) - static_cast<int>(arm64_register::x0)], 8};
                }
                if (id >= arm64_register::w0 && id <= arm64_register::w30)
                {
                    return {&state_.x[static_cast<int>(id) - static_cast<int>(arm64_register::w0)], 4};
                }
                if (id >= arm64_register::q0 && id <= arm64_register::q31)
                {
                    return {&state_.vectors[static_cast<int>(id) - static_cast<int>(arm64_register::q0)], 16};
                }
                if (id >= arm64_register::v0 && id <= arm64_register::v31)
                {
                    return {&state_.vectors[static_cast<int>(id) - static_cast<int>(arm64_register::v0)], 16};
                }
                if (id >= arm64_register::bcr0 && id <= arm64_register::wvr1)
                {
                    zero_ = 0;
                    const auto narrow = (id >= arm64_register::bcr0 && id <= arm64_register::bcr7) ||
                                        (id >= arm64_register::wcr0 && id <= arm64_register::wcr1);
                    return {&zero_, narrow ? 4 : 8};
                }
                switch (id)
                {
                case arm64_register::x29:
                    return {&state_.x[29], 8};
                case arm64_register::x30:
                    return {&state_.x[30], 8};
                case arm64_register::sp:
                    return {&state_.sp, 8};
                case arm64_register::wsp:
                    return {&state_.sp, 4};
                case arm64_register::pc:
                    return {&state_.pc, 8};
                case arm64_register::nzcv:
                case arm64_register::pstate:
                    return {&state_.pstate, 4};
                case arm64_register::fpcr:
                    return {&state_.fpcr, 4};
                case arm64_register::fpsr:
                    return {&state_.fpsr, 4};
                default:
                    throw std::runtime_error("Seccomp does not support register " + std::to_string(static_cast<int>(id)));
                }
            }

            void dispatch_svc()
            {
                state_.pc -= 4;
                uint32_t instruction{};
                read_memory(state_.pc, &instruction, sizeof(instruction));
                if ((instruction & 0xFFE0001F) != 0xD4000001)
                {
                    throw std::runtime_error("Seccomp SIGSYS does not follow an SVC instruction");
                }
                const auto callback = svc_hook_;
                switch (callback(*this, (instruction >> 5) & 0xFFFF))
                {
                case instruction_hook_continuation::skip_instruction:
                    state_.pc += 4;
                    return;
                case instruction_hook_continuation::finalized_instruction_pointer:
                    return;
                default:
                    unsupported("passing guest SVC instructions to the host kernel");
                }
            }

            void validate_mapping(const uint64_t address, const size_t size) const
            {
                if (!in_window(address, size) || size == 0 || address % page_size != 0 || size % page_size != 0)
                {
                    throw std::invalid_argument("Invalid seccomp mapping range");
                }
            }

            bool mapped(const uint64_t address, const size_t size) const
            {
                if (!in_window(address, size))
                {
                    return false;
                }
                for (auto page = address & ~(page_size - 1); page < address + size; page += page_size)
                {
                    if (!pages_.contains(page))
                    {
                        return false;
                    }
                }
                return true;
            }

            bool copy_memory(const uint64_t address, void* data, const size_t size, const bool write) const
            {
                if (size == 0)
                {
                    return true;
                }
                if (!data || !mapped(address, size))
                {
                    return false;
                }
                if (const auto region = find_mmio(address, size); region != mmio_regions_.end())
                {
                    if (write || address < region->address || size > region->size - (address - region->address))
                    {
                        return false;
                    }
                    region->read(address - region->address, data, size);
                    return true;
                }
                std::vector<std::pair<uint64_t, int>> changed{};
                const auto restore = utils::finally([&] {
                    for (const auto& [page, original] : changed)
                    {
                        if (mprotect(reinterpret_cast<void*>(page), page_size, original) != 0)
                        {
                            std::terminate();
                        }
                    }
                });
                const int required = write ? PROT_WRITE : PROT_READ;
                for (auto page = address & ~(page_size - 1); page < address + size; page += page_size)
                {
                    const auto original = protection(pages_.at(page));
                    if ((original & required) == 0)
                    {
                        changed.emplace_back(page, original);
                        check_system(mprotect(reinterpret_cast<void*>(page), page_size, original | required), "Access guest memory");
                    }
                }
                if (write)
                {
                    std::memmove(reinterpret_cast<void*>(address), data, size);
                    __builtin___clear_cache(reinterpret_cast<char*>(address), reinterpret_cast<char*>(address + size));
                }
                else
                {
                    std::memmove(data, reinterpret_cast<const void*>(address), size);
                }
                return true;
            }

            void map_memory(const uint64_t address, const size_t size, const memory_permission permissions) override
            {
                validate_mapping(address, size);
                for (auto page = address; page < address + size; page += page_size)
                {
                    if (pages_.contains(page))
                    {
                        throw std::runtime_error("Overlapping seccomp memory mapping");
                    }
                }
                check_system(mprotect(reinterpret_cast<void*>(address), size, protection(permissions)), "Map guest memory");
                for (auto page = address; page < address + size; page += page_size)
                {
                    pages_.emplace(page, permissions);
                }
            }

            void unmap_memory(const uint64_t address, const size_t size) override
            {
                validate_mapping(address, size);
                if (!mapped(address, size))
                {
                    throw std::runtime_error("Unmapping missing seccomp memory");
                }
                for (const auto& region : mmio_regions_)
                {
                    if (address < region.address + region.size && region.address < address + size &&
                        (address > region.address || address + size < region.address + region.size))
                    {
                        unsupported("partially unmapping an MMIO region");
                    }
                }
                check_system(mprotect(reinterpret_cast<void*>(address), size, PROT_NONE), "Unmap guest memory");
                check_system(madvise(reinterpret_cast<void*>(address), size, MADV_DONTNEED), "Discard guest memory");
                for (auto page = address; page < address + size; page += page_size)
                {
                    pages_.erase(page);
                }
                std::erase_if(mmio_regions_,
                              [&](const auto& region) { return region.address >= address && region.address < address + size; });
            }

            void apply_memory_protection(const uint64_t address, const size_t size, const memory_permission permissions) override
            {
                validate_mapping(address, size);
                if (!mapped(address, size))
                {
                    throw std::runtime_error("Protecting missing seccomp memory");
                }
                if (find_mmio(address, size) != mmio_regions_.end())
                {
                    unsupported("changing MMIO protection");
                }
                check_system(mprotect(reinterpret_cast<void*>(address), size, protection(permissions)), "Protect guest memory");
                for (auto page = address; page < address + size; page += page_size)
                {
                    pages_.at(page) = permissions;
                }
            }

            void map_mmio(const uint64_t address, const size_t size, mmio_read_callback read, mmio_write_callback) override
            {
                if (!read)
                {
                    throw std::invalid_argument("Seccomp MMIO requires a read callback");
                }
                map_memory(address, size, memory_permission::none);
                mmio_regions_.push_back({address, size, std::move(read)});
            }
        };
    }

    std::unique_ptr<arm64_emulator> create_arm64_emulator()
    {
        return std::make_unique<seccomp_emulator>();
    }
}

extern "C" __attribute__((visibility("hidden"))) void sogen_seccomp_signal_dispatch(const int signal, siginfo_t* info, void* context)
{
    if (!sogen::seccomp::seccomp_emulator::active || !info || !context)
    {
        _exit(128 + signal);
    }
    sogen::seccomp::seccomp_emulator::active->handle_signal(signal, *info, *static_cast<ucontext_t*>(context));
}
