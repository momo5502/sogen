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
    __attribute__((visibility("hidden"))) std::atomic<pid_t> sogen_seccomp_owner_tid{};
    void sogen_seccomp_signal_entry(int signal, siginfo_t* info, void* context);
    void sogen_seccomp_host_signal_dispatch(int signal, siginfo_t* info, void* context);
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

        struct host_signal_action
        {
            struct sigaction action{};
            std::atomic<bool> reset{};
        };

        std::array<host_signal_action, signals.size()> host_actions{};

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
                        check_system(sigaction(signal, nullptr, &host_actions[installed].action), "Read host sigaction");
                        host_actions[installed].reset = false;
                        action.sa_flags = SA_SIGINFO | SA_ONSTACK | (host_actions[installed].action.sa_flags & SA_RESTART);
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
                    auto previous = previous_actions[i];
                    if (host_actions[i].reset)
                    {
                        previous.sa_handler = SIG_DFL;
                        previous.sa_flags = 0;
                    }
                    sigaction(signals[i], &previous, nullptr);
                }
                sigaltstack(&previous_stack, nullptr);
                sigprocmask(SIG_SETMASK, &previous_mask, nullptr);
            }

            static void forward(const int signal, siginfo_t* info, void* context)
            {
                const auto index = static_cast<size_t>(std::ranges::find(signals, signal) - signals.begin());
                if (index == signals.size() || !context)
                {
                    _exit(128 + signal);
                }
                auto previous = host_actions[index].action;
                if ((previous.sa_flags & SA_RESETHAND) != 0 && host_actions[index].reset.exchange(true))
                {
                    previous.sa_handler = SIG_DFL;
                    previous.sa_flags = 0;
                }
                if (previous.sa_handler == SIG_IGN)
                {
                    return;
                }
                const auto saved_errno = errno;
                if (previous.sa_handler == SIG_DFL)
                {
                    sigaction(signal, &previous, nullptr);
                    syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), signal);
                }
                else
                {
                    auto mask = static_cast<ucontext_t*>(context)->uc_sigmask;
                    for (int member = 1; member < NSIG; ++member)
                    {
                        if (sigismember(&previous.sa_mask, member) == 1)
                        {
                            sigaddset(&mask, member);
                        }
                    }
                    if ((previous.sa_flags & SA_NODEFER) == 0)
                    {
                        sigaddset(&mask, signal);
                    }
                    sigset_t current_mask{};
                    sigprocmask(SIG_SETMASK, &mask, &current_mask);
                    errno = saved_errno;
                    if ((previous.sa_flags & SA_SIGINFO) != 0)
                    {
                        previous.sa_sigaction(signal, info, context);
                    }
                    else
                    {
                        previous.sa_handler(signal);
                    }
                    sigprocmask(SIG_SETMASK, &current_mask, nullptr);
                }
                errno = saved_errno;
            }
        };

        template <typename T>
        T* find_context(ucontext_t& context, const uint32_t magic)
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
                if (header->magic == magic && header->size >= sizeof(T))
                {
                    return reinterpret_cast<T*>(cursor);
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

        struct decoded_memory_access
        {
            memory_operation operation;
            size_t size;
        };

        std::optional<decoded_memory_access> decode_memory_access(const uint32_t instruction)
        {
            if (const auto load = decode_mmio_load(instruction))
            {
                return decoded_memory_access{memory_operation::read, load->size};
            }
            if ((instruction & (1u << 22)) == 0)
            {
                if (const auto store = decode_mmio_load(instruction | (1u << 22)))
                {
                    return decoded_memory_access{memory_operation::write, store->size};
                }
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
                static_assert(std::atomic<pid_t>::is_always_lock_free);
                static_assert(sizeof(sogen_seccomp_owner_tid) == 4);
                static_assert(std::atomic<bool>::is_always_lock_free);
                sogen_seccomp_owner_tid.store(owner_tid_, std::memory_order_release);
                active = this;
                try
                {
                    signals_ = std::make_unique<signal_environment>();
                    install_filter();
                }
                catch (...)
                {
                    signals_.reset();
                    active = nullptr;
                    sogen_seccomp_owner_tid.store(0, std::memory_order_release);
                    throw;
                }
            }

            ~seccomp_emulator() override
            {
                signals_.reset();
                active = nullptr;
                sogen_seccomp_owner_tid.store(0, std::memory_order_release);
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

            bool supports_restartable_faults() const override
            {
                return false;
            }

            bool supports_native_timestamp_counter() const override
            {
                return true;
            }

            bool supports_timestamp_counter_interception() const override
            {
                return false;
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
                    if (state_.pc % 4 != 0 || state_.sp % 16 != 0)
                    {
                        throw std::runtime_error("Invalid seccomp entry PC or SP");
                    }
                    if (!executable(state_.pc))
                    {
                        state_.pstate &= ~pstate_software_step;
                        if (dispatch_memory_violation(state_.pc, 4, memory_operation::exec))
                        {
                            continue;
                        }
                        violation_ = true;
                        throw std::runtime_error("Seccomp instruction fetch from non-executable memory");
                    }
                    if (!prepare_execution_hooks() || !executable(state_.pc))
                    {
                        continue;
                    }
                    const bool single_step = (state_.pstate & pstate_software_step) != 0;
                    const bool execution_step = page_has_execution_hooks(state_.pc);
                    const auto instruction_pc = state_.pc;
                    if (single_step || execution_step)
                    {
                        if (single_step && !interrupt_hook_)
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
                            const auto page = state_.pc & ~(page_size - 1);
                            const auto rearm = utils::finally([&] {
                                execution_step_page_.reset();
                                const auto entry = pages_.find(page);
                                if (execution_step && entry != pages_.end() &&
                                    mprotect(reinterpret_cast<void*>(page), page_size, effective_protection(page, entry->second)) != 0)
                                {
                                    std::terminate();
                                }
                            });
                            if (execution_step)
                            {
                                execution_step_page_ = page;
                                refresh_protection(page, page_size);
                            }
                            result = single_step_guest(state_, enter_signal);
                        }
                        state_.pstate &= ~pstate_software_step;
                        if (result.signal == SIGTRAP && result.info.si_code == TRAP_TRACE)
                        {
                            if (single_step)
                            {
                                const auto callback = interrupt_hook_;
                                callback(*this, static_cast<int>(arm64_exception_class::software_step));
                            }
                            continue;
                        }
                        trapped_signal_ = result.signal;
                        trapped_code_ = result.info.si_code;
                        trapped_arch_ = result.signal == SIGSYS ? result.info.si_arch : 0;
                        trapped_address_ = reinterpret_cast<uint64_t>(result.info.si_addr);
                        trapped_esr_ = 0;
                    }
                    else
                    {
                        if (sigsetjmp(host_return_, 1) == 0)
                        {
                            entry_pending_ = true;
                            check_system(static_cast<int>(syscall(SYS_tgkill, getpid(), owner_tid_, enter_signal)), "Enter seccomp");
                            entry_pending_ = false;
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
                    if (trapped_signal_ == SIGSEGV && trapped_code_ == SEGV_ACCERR &&
                        is_execution_hook_fault(single_step || execution_step, instruction_pc))
                    {
                        continue;
                    }
                    if (trapped_signal_ == SIGSEGV && trapped_code_ == SEGV_ACCERR && handle_mmio_fault())
                    {
                        if (single_step)
                        {
                            const auto callback = interrupt_hook_;
                            callback(*this, static_cast<int>(arm64_exception_class::software_step));
                        }
                        continue;
                    }
                    if (trapped_signal_ == SIGSEGV && (trapped_code_ == SEGV_MAPERR || trapped_code_ == SEGV_ACCERR) &&
                        handle_memory_fault())
                    {
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
                    if (trapped_signal_ == SIGTRAP && trapped_code_ == TRAP_BRKPT && interrupt_hook_)
                    {
                        const auto callback = interrupt_hook_;
                        callback(*this, static_cast<int>(arm64_exception_class::breakpoint_instruction));
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
                if (checking_alignment_ && signal == SIGBUS && info.si_code == BUS_ADRALN)
                {
                    siglongjmp(alignment_return_, 1);
                }
                const bool internal_signal = info.si_code == SI_TKILL && info.si_pid == getpid();
                if (signal == enter_signal && internal_signal && entry_pending_)
                {
                    entry_pending_ = false;
                    if (stop_requested_)
                    {
                        return;
                    }
                    auto* fp = find_context<fpsimd_context>(context, FPSIMD_MAGIC);
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
                    guest_active_ = true;
                    return;
                }
                if (signal == stop_signal && internal_signal && stop_requested_ && !guest_active_)
                {
                    return;
                }
                bool stopped_in_host_handler{};
                if (!running_ || !guest_active_ || (info.si_code <= 0 && !(signal == stop_signal && internal_signal && stop_requested_)))
                {
                    const bool was_guest = guest_active_;
                    guest_active_ = false;
                    signal_environment::forward(signal, const_cast<siginfo_t*>(&info), &context);
                    guest_active_ = was_guest;
                    if (!was_guest || !stop_requested_)
                    {
                        return;
                    }
                    stopped_in_host_handler = true;
                }
                guest_active_ = false;
                for (size_t i = 0; i < state_.x.size(); ++i)
                {
                    state_.x[i] = machine.regs[i];
                }
                state_.sp = machine.sp;
                state_.pc = machine.pc;
                state_.pstate = machine.pstate & 0xF0000000;
                auto* fp = find_context<fpsimd_context>(context, FPSIMD_MAGIC);
                missing_fpsimd_ = fp == nullptr;
                if (fp)
                {
                    std::memcpy(state_.vectors.data(), fp->vregs, sizeof(fp->vregs));
                    state_.fpcr = fp->fpcr;
                    state_.fpsr = fp->fpsr;
                }
                trapped_signal_ = stopped_in_host_handler ? stop_signal : signal;
                trapped_code_ = info.si_code;
                trapped_arch_ = signal == SIGSYS ? info.si_arch : 0;
                trapped_address_ = reinterpret_cast<uint64_t>(info.si_addr);
                const auto* esr = find_context<esr_context>(context, ESR_MAGIC);
                trapped_esr_ = esr ? esr->esr : 0;
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

            emulator_hook* hook_memory_violation(memory_violation_hook_callback callback) override
            {
                if (!callback || memory_violation_hook_)
                {
                    throw std::invalid_argument("Seccomp requires exactly one nonempty memory violation hook");
                }
                memory_violation_hook_ = std::move(callback);
                return reinterpret_cast<emulator_hook*>(&memory_violation_hook_);
            }

            emulator_hook* hook_memory_execution(memory_execution_hook_callback) override
            {
                unsupported("execution hooks");
            }

            emulator_hook* hook_memory_execution(const uint64_t address, memory_execution_hook_callback callback) override
            {
                return hook_memory_range_execution(address, 1, std::move(callback));
            }

            emulator_hook* hook_memory_range_execution(const uint64_t address, const uint64_t size,
                                                       memory_execution_hook_callback callback) override
            {
                if (!callback || size == 0 || !in_window(address, size))
                {
                    throw std::invalid_argument("Invalid seccomp execution hook");
                }
                auto entry = std::make_unique<execution_hook>(execution_hook{address, size, std::move(callback)});
                auto* hook = reinterpret_cast<emulator_hook*>(entry.get());
                execution_hooks_.push_back(std::move(entry));
                for (auto page = pages_.lower_bound(address & ~(page_size - 1)); page != pages_.end() && page->first < address + size;
                     ++page)
                {
                    ++page->second.execution_hook_count;
                }
                refresh_protection(address, size);
                return hook;
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
                if (hook == reinterpret_cast<emulator_hook*>(&memory_violation_hook_) && memory_violation_hook_)
                {
                    memory_violation_hook_ = {};
                    return;
                }
                const auto execution = std::ranges::find_if(
                    execution_hooks_, [hook](const auto& entry) { return hook == reinterpret_cast<emulator_hook*>(entry.get()); });
                if (execution != execution_hooks_.end())
                {
                    const auto address = (*execution)->address;
                    const auto size = (*execution)->size;
                    std::vector<mapped_page*> changed{};
                    auto rollback = utils::finally([&] {
                        for (auto* page : changed)
                        {
                            ++page->execution_hook_count;
                        }
                    });
                    for (auto page = pages_.lower_bound(address & ~(page_size - 1)); page != pages_.end() && page->first < address + size;
                         ++page)
                    {
                        if (page->second.execution_hook_count != 0)
                        {
                            changed.push_back(&page->second);
                            --page->second.execution_hook_count;
                        }
                    }
                    refresh_protection(address, size);
                    rollback.cancel();
                    execution_hooks_.erase(execution);
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
            std::unique_ptr<signal_environment> signals_{};
            pid_t owner_tid_{};
            std::atomic<bool> running_{};
            std::atomic<bool> stop_requested_{};
            std::mutex step_mutex_{};
            register_state state_{};
            uint64_t zero_{};
            sigjmp_buf host_return_{};
            sigjmp_buf alignment_return_{};
            volatile sig_atomic_t entry_pending_{};
            volatile sig_atomic_t guest_active_{};
            volatile sig_atomic_t checking_alignment_{};
            int trapped_signal_{};
            int trapped_code_{};
            uint32_t trapped_arch_{};
            uint64_t trapped_address_{};
            uint64_t trapped_esr_{};
            bool missing_fpsimd_{};
            bool violation_{};
            instruction_hook_callback svc_hook_{};
            interrupt_hook_callback interrupt_hook_{};
            memory_violation_hook_callback memory_violation_hook_{};
            std::vector<std::unique_ptr<std::byte>> ignored_hooks_{};

            struct mapped_page
            {
                memory_permission permissions;
                size_t execution_hook_count{};
            };

            struct execution_hook
            {
                uint64_t address;
                uint64_t size;
                memory_execution_hook_callback callback;
            };

            std::map<uint64_t, mapped_page> pages_{};
            std::vector<std::unique_ptr<execution_hook>> execution_hooks_{};
            std::optional<uint64_t> execution_step_page_{};
            std::optional<uint64_t> execution_resume_pc_{};

            struct mmio_region
            {
                uint64_t address;
                size_t size;
                mmio_read_callback read;
            };

            std::vector<mmio_region> mmio_regions_{};

            bool executable(const uint64_t address) const
            {
                const auto page = pages_.find(address & ~(page_size - 1));
                return in_window(address, 4) && page != pages_.end() && (protection(page->second.permissions) & PROT_EXEC) != 0;
            }

            bool page_has_execution_hooks(const uint64_t address) const
            {
                const auto page = pages_.find(address & ~(page_size - 1));
                return page != pages_.end() && page->second.execution_hook_count != 0;
            }

            int effective_protection(const uint64_t address, const mapped_page& page) const
            {
                const auto permissions = protection(page.permissions);
                return page.execution_hook_count != 0 && execution_step_page_ != address ? permissions & ~PROT_EXEC : permissions;
            }

            void refresh_protection(const uint64_t address, const uint64_t size) const
            {
                uint64_t run_begin{};
                uint64_t run_end{};
                int run_protection{};
                const auto flush = [&] {
                    if (run_begin != run_end)
                    {
                        check_system(mprotect(reinterpret_cast<void*>(run_begin), run_end - run_begin, run_protection),
                                     "Update seccomp page protection");
                    }
                };
                for (auto page = pages_.lower_bound(address & ~(page_size - 1)); page != pages_.end() && page->first < address + size;
                     ++page)
                {
                    const auto permissions = effective_protection(page->first, page->second);
                    if (run_end != page->first || run_protection != permissions)
                    {
                        flush();
                        run_begin = page->first;
                        run_protection = permissions;
                    }
                    run_end = page->first + page_size;
                }
                flush();
            }

            bool prepare_execution_hooks()
            {
                const auto pc = state_.pc;
                if (!page_has_execution_hooks(pc))
                {
                    execution_resume_pc_.reset();
                    return true;
                }
                if (execution_resume_pc_ != pc)
                {
                    std::vector<memory_execution_hook_callback> callbacks{};
                    for (const auto& hook : execution_hooks_)
                    {
                        if (pc >= hook->address && pc - hook->address < hook->size)
                        {
                            callbacks.push_back(hook->callback);
                        }
                    }
                    execution_resume_pc_ = pc;
                    for (const auto& callback : callbacks)
                    {
                        callback(*this, pc);
                    }
                }
                if (stop_requested_ || state_.pc != pc)
                {
                    return false;
                }
                execution_resume_pc_.reset();
                return true;
            }

            bool is_execution_hook_fault(const bool stepped, const uint64_t instruction_pc) const
            {
                if (trapped_address_ != state_.pc || !executable(state_.pc) || !page_has_execution_hooks(state_.pc))
                {
                    return false;
                }
                const auto exception_class = (trapped_esr_ >> 26) & 0x3F;
                return exception_class == 0x20 || (trapped_esr_ == 0 && (!stepped || state_.pc != instruction_pc));
            }

            bool dispatch_memory_violation(const uint64_t address, const size_t size, const memory_operation operation)
            {
                if (!memory_violation_hook_)
                {
                    return false;
                }
                const auto type = mapped(address, 1) ? memory_violation_type::protection : memory_violation_type::unmapped;
                const auto callback = memory_violation_hook_;
                switch (callback(*this, address, size, operation, type))
                {
                case memory_violation_continuation::stop:
                    stop();
                    return true;
                case memory_violation_continuation::resume:
                    return true;
                case memory_violation_continuation::restart:
                    unsupported("restarting memory faults");
                }
                throw std::logic_error("Invalid seccomp memory violation continuation");
            }

            bool handle_memory_fault()
            {
                if (!memory_violation_hook_)
                {
                    return false;
                }
                const auto exception_class = (trapped_esr_ >> 26) & 0x3F;
                constexpr uint64_t instruction_abort = 0x20;
                constexpr uint64_t data_abort = 0x24;
                if (exception_class == instruction_abort)
                {
                    return dispatch_memory_violation(trapped_address_, 4, memory_operation::exec);
                }
                if (trapped_esr_ == 0 && !executable(state_.pc))
                {
                    return dispatch_memory_violation(trapped_address_, 4, memory_operation::exec);
                }
                uint32_t instruction{};
                read_memory(state_.pc, &instruction, sizeof(instruction));
                const auto access = decode_memory_access(instruction);
                if (exception_class == data_abort)
                {
                    const auto operation = (trapped_esr_ & (1u << 6)) != 0 ? memory_operation::write : memory_operation::read;
                    auto size = access ? access->size : 1;
                    if ((trapped_esr_ & (1u << 24)) != 0)
                    {
                        size = size_t{1} << ((trapped_esr_ >> 22) & 3);
                    }
                    return dispatch_memory_violation(trapped_address_, size, operation);
                }
                // Ptrace signal-delivery stops precede construction of the signal frame containing ESR_EL1.
                if (trapped_esr_ == 0 && access)
                {
                    return dispatch_memory_violation(trapped_address_, access->size, access->operation);
                }
                unsupported("classifying this single-stepped memory fault without ESR_EL1");
            }

            std::vector<mmio_region>::const_iterator find_mmio(const uint64_t address, const size_t size) const
            {
                return std::ranges::find_if(mmio_regions_, [&](const auto& region) {
                    return address < region.address + region.size && region.address < address + size;
                });
            }

            void check_mmio_alignment(const decoded_load& load, const uint64_t address)
            {
                if (!load.acquire || address % load.size == 0)
                {
                    return;
                }
                // FEAT_LSE2 and SCTLR_EL1.nAA affect acquire-load alignment; use the host's actual access rules.
                alignas(16) const std::array<std::byte, 32> scratch{};
                const auto* source = scratch.data() + (address & 15);
                if (sigsetjmp(alignment_return_, 1) != 0)
                {
                    checking_alignment_ = false;
                    violation_ = true;
                    throw std::runtime_error("Seccomp MMIO acquire-load alignment fault");
                }
                checking_alignment_ = true;
                uint64_t value{};
                switch (load.size)
                {
                case 2:
                    asm volatile("ldarh %w0, [%1]" : "=r"(value) : "r"(source) : "memory");
                    break;
                case 4:
                    asm volatile("ldar %w0, [%1]" : "=r"(value) : "r"(source) : "memory");
                    break;
                case 8:
                    asm volatile("ldar %0, [%1]" : "=r"(value) : "r"(source) : "memory");
                    break;
                default:
                    checking_alignment_ = false;
                    throw std::logic_error("Invalid seccomp acquire-load size");
                }
                checking_alignment_ = false;
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
                check_mmio_alignment(*load, trapped_address_);
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
                    const auto original = effective_protection(page, pages_.at(page));
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
                for (auto page = address; page < address + size; page += page_size)
                {
                    const auto count = std::ranges::count_if(execution_hooks_, [&](const auto& hook) {
                        return page < hook->address + hook->size && hook->address < page + page_size;
                    });
                    pages_.emplace(page, mapped_page{permissions, static_cast<size_t>(count)});
                }
                refresh_protection(address, size);
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
                for (auto page = address; page < address + size; page += page_size)
                {
                    pages_.at(page).permissions = permissions;
                }
                refresh_protection(address, size);
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
        sogen::seccomp::signal_environment::forward(signal, info, context);
        return;
    }
    sogen::seccomp::seccomp_emulator::active->handle_signal(signal, *info, *static_cast<ucontext_t*>(context));
}

extern "C" __attribute__((visibility("hidden"))) void sogen_seccomp_host_signal_dispatch(const int signal, siginfo_t* info, void* context)
{
    sogen::seccomp::signal_environment::forward(signal, info, context);
}
