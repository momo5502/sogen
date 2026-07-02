#define KVM_EMULATOR_IMPL
#include "kvm_x86_64_emulator.hpp"
#include "kvm_x86_64_common.hpp"

#include <fcntl.h>
#include <linux/kvm.h>
#include <pthread.h>
// NOLINTNEXTLINE(hicpp-deprecated-headers,modernize-deprecated-headers): POSIX sigaction/SIGRTMIN are not in <csignal>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <immintrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <utils/object.hpp>

#ifndef MSR_LSTAR
#define MSR_LSTAR 0xC0000082
#endif

#ifndef MSR_STAR
#define MSR_STAR 0xC0000081
#endif

#ifndef MSR_SYSCALL_MASK
#define MSR_SYSCALL_MASK 0xC0000084
#endif

namespace sogen::kvm
{
    namespace
    {
        using detail::classify_gp_register_access;
        using detail::execution_hook_entry;
        using detail::instruction_hook_entry;
        using detail::internal_page_table_base;
        using detail::is_page_aligned;
        using detail::mapped_page;
        using detail::memory_access_hook_entry;
        using detail::mmio_region;
        using detail::page_size;

        constexpr uint32_t vp_index = 0;
        constexpr uint32_t breakpoint_interrupt = 3;
        constexpr int invalid_opcode_interrupt = 6;
        constexpr std::byte int3_opcode{0xCC};
        constexpr uint64_t syscall_instruction_size = 2;

        constexpr uintptr_t cache_line_size = 64;
        constexpr uint64_t guest_physical_page_base = 0x0000000100000000ull;

        // clflushopt is unordered, so consecutive evictions pipeline instead of serializing like clflush does
        // on every line; for the bulk flushes the GPU bridge issues before each submit that is a meaningful
        // win. It needs a target attribute to compile on a generic x86-64 build and is only selected when the
        // CPU advertises it at runtime. Both variants leave ordering to the caller's sfence.
        __attribute__((target("clflushopt"))) void flush_cache_lines_clflushopt(uintptr_t first, uintptr_t last)
        {
            for (auto line = first; line < last; line += cache_line_size)
            {
                _mm_clflushopt(reinterpret_cast<void*>(line));
            }
        }

        void flush_cache_lines_clflush(uintptr_t first, uintptr_t last)
        {
            for (auto line = first; line < last; line += cache_line_size)
            {
                _mm_clflush(reinterpret_cast<const void*>(line));
            }
        }

        // Evict the CPU cache for [base, base + size) and order the evictions before subsequent device access.
        void flush_cache_line_range(const void* base, size_t size)
        {
            if (base == nullptr || size == 0)
            {
                return;
            }

            const auto first = reinterpret_cast<uintptr_t>(base) & ~(cache_line_size - 1);
            const auto last = reinterpret_cast<uintptr_t>(base) + size;

            static const bool has_clflushopt = __builtin_cpu_supports("clflushopt");
            if (has_clflushopt)
            {
                flush_cache_lines_clflushopt(first, last);
            }
            else
            {
                flush_cache_lines_clflush(first, last);
            }

            _mm_sfence();
        }

        // Synthetic exception delivery: guest exceptions are routed through an internal IDT whose
        // gates point at single-HLT stubs running at CPL0. The HLT exit identifies the vector, and
        // the run loop reconstructs the faulting context from the pushed exception frame.
        constexpr uint32_t exception_vector_count = 32;
        constexpr uint64_t exception_stub_stride = 8;
        constexpr uint16_t kernel_code_selector = 0x08; // 64-bit ring-0 code segment in the guest GDT
        constexpr uint16_t task_state_selector = 0x38;
        constexpr uint16_t tss_descriptor_limit = 0x67;
        constexpr uint8_t exception_ist_index = 1;

        bool exception_has_error_code(const uint32_t vector)
        {
            switch (vector)
            {
            case 8:  // #DF
            case 10: // #TS
            case 11: // #NP
            case 12: // #SS
            case 13: // #GP
            case 14: // #PF
            case 17: // #AC
            case 21: // #CP
                return true;
            default:
                return false;
            }
        }

        [[noreturn]] void throw_errno(const char* action)
        {
            std::ostringstream stream;
            stream << action << " failed: " << std::strerror(errno);
            throw std::runtime_error(stream.str());
        }

        void check_ioctl_result(const int rc, const char* action)
        {
            if (rc < 0)
            {
                throw_errno(action);
            }
        }

#if defined(KVM_SET_GUEST_DEBUG) && defined(KVM_GUESTDBG_ENABLE) && defined(KVM_GUESTDBG_SINGLESTEP)
        void enable_guest_single_step(const int vcpu_fd)
        {
            kvm_guest_debug debug{};
            debug.control = KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_SINGLESTEP;
            check_ioctl_result(::ioctl(vcpu_fd, KVM_SET_GUEST_DEBUG, &debug), "KVM_SET_GUEST_DEBUG");
        }

        void clear_guest_debug_control_noexcept(const int vcpu_fd) noexcept
        {
            kvm_guest_debug debug{};
            (void)::ioctl(vcpu_fd, KVM_SET_GUEST_DEBUG, &debug);
        }
#else
        void enable_guest_single_step(const int)
        {
            throw std::runtime_error("KVM backend single-step requires KVM guest debug support");
        }

        void clear_guest_debug_control_noexcept(const int) noexcept
        {
        }
#endif

        class scoped_guest_debug
        {
          public:
            scoped_guest_debug(const int vcpu_fd, const bool enable)
                : vcpu_fd_(vcpu_fd),
                  active_(enable)
            {
                if (this->active_)
                {
                    enable_guest_single_step(this->vcpu_fd_);
                }
            }

            scoped_guest_debug(const scoped_guest_debug&) = delete;
            scoped_guest_debug& operator=(const scoped_guest_debug&) = delete;

            ~scoped_guest_debug()
            {
                if (this->active_)
                {
                    clear_guest_debug_control_noexcept(this->vcpu_fd_);
                }
            }

          private:
            int vcpu_fd_ = -1;
            bool active_ = false;
        };

        // No-op handler whose only purpose is to interrupt a blocking KVM_RUN ioctl so the
        // run loop can observe a pending stop request. Installed without SA_RESTART so the
        // ioctl returns EINTR instead of being silently restarted.
        void vcpu_kick_handler(int)
        {
        }

        int install_vcpu_kick_signal()
        {
            const int signal_number = SIGRTMIN;

            struct sigaction action{};
            action.sa_handler = vcpu_kick_handler;
            sigemptyset(&action.sa_mask);
            action.sa_flags = 0;

            if (sigaction(signal_number, &action, nullptr) != 0)
            {
                throw_errno("sigaction");
            }

            return signal_number;
        }

        int vcpu_kick_signal()
        {
            static const int signal_number = install_vcpu_kick_signal();
            return signal_number;
        }

        class file_descriptor
        {
          public:
            file_descriptor() = default;
            explicit file_descriptor(const int fd)
                : fd_(fd)
            {
            }

            file_descriptor(const file_descriptor&) = delete;
            file_descriptor& operator=(const file_descriptor&) = delete;

            file_descriptor(file_descriptor&& other) noexcept
                : fd_(std::exchange(other.fd_, -1))
            {
            }

            file_descriptor& operator=(file_descriptor&& other) noexcept
            {
                if (this != &other)
                {
                    this->reset();
                    this->fd_ = std::exchange(other.fd_, -1);
                }
                return *this;
            }

            ~file_descriptor()
            {
                this->reset();
            }

            int get() const
            {
                return this->fd_;
            }

            explicit operator bool() const
            {
                return this->fd_ >= 0;
            }

            void reset(const int fd = -1)
            {
                if (this->fd_ >= 0)
                {
                    ::close(this->fd_);
                }

                this->fd_ = fd;
            }

          private:
            int fd_ = -1;
        };

        struct mapped_run_deleter
        {
            size_t size = 0;

            void operator()(uint8_t* page) const
            {
                if (page != nullptr)
                {
                    ::munmap(page, this->size);
                }
            }
        };

        std::shared_ptr<uint8_t> allocate_backing_memory(const size_t size)
        {
            auto* raw_memory = static_cast<uint8_t*>(::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
            if (raw_memory == MAP_FAILED)
            {
                throw_errno("mmap");
            }

            std::memset(raw_memory, 0, size);
            return std::shared_ptr<uint8_t>(raw_memory, mapped_run_deleter{size});
        }

        enum class register_kind
        {
            reg64,
            segment,
            table,
            fp,
            fp_control,
            xmm_control,
            reg128,
        };

        enum class register_name
        {
            rax,
            rbx,
            rcx,
            rdx,
            rsi,
            rdi,
            rbp,
            rsp,
            rip,
            r8,
            r9,
            r10,
            r11,
            r12,
            r13,
            r14,
            r15,
            rflags,
            cs,
            ss,
            ds,
            es,
            fs,
            gs,
            gdtr,
            idtr,
            cr0,
            cr2,
            cr3,
            cr4,
            dr0,
            dr1,
            dr2,
            dr3,
            dr6,
            dr7,
            efer,
            xmm0,
            fp0,
            fp_control_status,
            xmm_control_status,
        };

        struct register_mapping
        {
            register_name name{};
            register_kind kind = register_kind::reg64;
            size_t logical_size = sizeof(uint64_t);
        };

        // 4 KiB matches the base KVM_GET_XSAVE region (kvm_xsave::region[1024]); large enough for
        // x87 + SSE + AVX state. uint32_t keeps it 4-byte aligned for the ioctl copy.
        using xsave_area = std::array<uint32_t, 1024>;

        struct register_snapshot
        {
            kvm_regs regs{};
            kvm_sregs sregs{};
            xsave_area xsave{};
            uint64_t lstar{};
            uint64_t star{};
            uint64_t sfmask{};
        };

        kvm_segment make_segment(uint16_t selector, bool is_code, bool is_user);
        register_mapping map_register(x86_register reg);

        class kvm_x86_64_emulator final : public x86_64_emulator
        {
          public:
            kvm_x86_64_emulator()
            {
                this->kick_signal_ = vcpu_kick_signal();
                this->ensure_platform_support();
                this->configure_partition();
                this->configure_virtual_processor();
                this->initialize_long_mode_page_tables();
                this->initialize_virtual_processor_state();
                this->initialize_syscall_intercept_page();
                this->initialize_exception_handling();
            }
            ~kvm_x86_64_emulator() override
            {
                utils::reset_object_with_delayed_destruction(this->memory_write_hooks_);
                utils::reset_object_with_delayed_destruction(this->memory_read_hooks_);
                utils::reset_object_with_delayed_destruction(this->memory_execution_hooks_);
                utils::reset_object_with_delayed_destruction(this->memory_violation_hooks_);
                utils::reset_object_with_delayed_destruction(this->interrupt_hooks_);
                utils::reset_object_with_delayed_destruction(this->basic_block_hooks_);
                utils::reset_object_with_delayed_destruction(this->instruction_hooks_);

                if (this->run_ != nullptr)
                {
                    ::munmap(this->run_, this->vcpu_mmap_size_);
                }
            }

            bool read_descriptor_table(int reg, descriptor_table_register& table) override
            {
                if (reg != static_cast<int>(x86_register::gdtr) && reg != static_cast<int>(x86_register::idtr))
                {
                    return false;
                }

                const auto sregs = this->get_sregs();
                const auto& value =
                    get_table_register(sregs, reg == static_cast<int>(x86_register::gdtr) ? register_name::gdtr : register_name::idtr);
                table.base = value.base;
                table.limit = value.limit;
                return true;
            }
            void start(size_t count) override
            {
                if (count > 1)
                {
                    throw std::runtime_error("KVM backend does not support exact instruction counts greater than one yet");
                }

                const bool single_step = count == 1;
                const scoped_guest_debug guest_debug(this->vcpu_fd_.get(), single_step);

                this->stop_requested_ = false;
                this->vcpu_thread_.store(pthread_self(), std::memory_order_release);
                this->run_->immediate_exit = 0;

                while (!this->stop_requested_)
                {
                    const auto step_rip = this->read_instruction_pointer();
                    if (this->handle_pre_run_instruction())
                    {
                        this->run_memory_execution_hooks(step_rip);
                        if (single_step)
                        {
                            return;
                        }

                        continue;
                    }

                    this->refresh_mmio_pages();
                    this->flush_dirty_mappings();
                    this->flush_register_cache();

                    this->run_active_ = true;
                    const auto rc = ::ioctl(this->vcpu_fd_.get(), KVM_RUN, 0);
                    this->run_active_ = false;
                    this->invalidate_register_cache();

                    if (rc < 0)
                    {
                        if (errno == EINTR && this->stop_requested_)
                        {
                            return;
                        }

                        if (errno == EINTR)
                        {
                            continue;
                        }

                        throw_errno("KVM_RUN");
                    }

                    switch (this->run_->exit_reason)
                    {
                    case KVM_EXIT_HLT: {
                        const auto rip = this->read_instruction_pointer();
                        if (this->syscall_hook_ && rip == (this->syscall_hook_page_ + 1))
                        {
                            if (const auto executed_rip = this->handle_syscall_halt())
                            {
                                if (single_step)
                                {
                                    this->run_memory_execution_hooks(*executed_rip);
                                    return;
                                }

                                continue;
                            }

                            return;
                        }

                        const auto stub_end = this->exception_stub_page_ + exception_vector_count * exception_stub_stride;
                        if (rip > this->exception_stub_page_ && rip <= stub_end)
                        {
                            if (this->handle_exception_trap(rip))
                            {
                                this->clear_pending_exception_state();
                                continue;
                            }
                        }

                        return;
                    }
                    case KVM_EXIT_MMIO:
                        if (this->handle_mmio_exit())
                        {
                            continue;
                        }

                        return;
                    case KVM_EXIT_EXCEPTION:
                        if (this->handle_exception(this->run_->ex.exception, this->run_->ex.error_code))
                        {
                            this->clear_pending_exception_state();
                            continue;
                        }

                        return;
                    case KVM_EXIT_DEBUG:
                        if (single_step)
                        {
                            this->run_memory_execution_hooks(step_rip);
                            return;
                        }

                        if (this->handle_debug_exit())
                        {
                            continue;
                        }

                        return;
                    case KVM_EXIT_INTR:
                        if (this->stop_requested_)
                        {
                            return;
                        }

                        continue;
                    case KVM_EXIT_SHUTDOWN:
                        throw std::runtime_error("KVM guest triple-faulted (SHUTDOWN) at " +
                                                 std::to_string(this->read_instruction_pointer()));
                    case KVM_EXIT_FAIL_ENTRY:
                        throw std::runtime_error("KVM vCPU failed to enter guest mode");
                    case KVM_EXIT_INTERNAL_ERROR:
                        throw std::runtime_error("KVM reported an internal error");
                    default:
                        throw std::runtime_error("Unhandled KVM exit reason: " + std::to_string(this->run_->exit_reason));
                    }
                }
            }
            void stop() override
            {
                this->stop_requested_ = true;

                if (this->run_ != nullptr)
                {
                    // Honoured at the next KVM_RUN entry; covers the race where the vCPU has not
                    // entered guest mode yet.
                    this->run_->immediate_exit = 1;
                }

                if (this->run_active_)
                {
                    // The vCPU may already be executing guest code, where immediate_exit no longer
                    // applies. Signal the vCPU thread to force the in-flight KVM_RUN to return EINTR.
                    pthread_kill(this->vcpu_thread_.load(std::memory_order_acquire), this->kick_signal_);
                }
            }
            size_t read_raw_register(int reg, void* value, size_t size) override
            {
                const auto xreg = static_cast<x86_register>(reg);
                const auto mapping = map_register(xreg);

                switch (mapping.kind)
                {
                case register_kind::reg64: {
                    if (mapping.name == register_name::efer || mapping.name == register_name::cr0 || mapping.name == register_name::cr2 ||
                        mapping.name == register_name::cr3 || mapping.name == register_name::cr4)
                    {
                        const auto sregs = this->get_sregs();
                        const void* source = nullptr;
                        switch (mapping.name)
                        {
                        case register_name::efer:
                            source = &sregs.efer;
                            break;
                        case register_name::cr0:
                            source = &sregs.cr0;
                            break;
                        case register_name::cr2:
                            source = &sregs.cr2;
                            break;
                        case register_name::cr3:
                            source = &sregs.cr3;
                            break;
                        case register_name::cr4:
                            source = &sregs.cr4;
                            break;
                        default:
                            break;
                        }
                        std::memcpy(value, source, (std::min)(size, sizeof(uint64_t)));
                        return size;
                    }

                    if (mapping.name == register_name::dr0 || mapping.name == register_name::dr1 || mapping.name == register_name::dr2 ||
                        mapping.name == register_name::dr3 || mapping.name == register_name::dr6 || mapping.name == register_name::dr7)
                    {
                        const auto debugregs = this->get_debugregs();
                        const void* source = nullptr;
                        switch (mapping.name)
                        {
                        case register_name::dr0:
                            source = &debugregs.db[0];
                            break;
                        case register_name::dr1:
                            source = &debugregs.db[1];
                            break;
                        case register_name::dr2:
                            source = &debugregs.db[2];
                            break;
                        case register_name::dr3:
                            source = &debugregs.db[3];
                            break;
                        case register_name::dr6:
                            source = &debugregs.dr6;
                            break;
                        case register_name::dr7:
                            source = &debugregs.dr7;
                            break;
                        default:
                            break;
                        }
                        std::memcpy(value, source, (std::min)(size, sizeof(uint64_t)));
                        return size;
                    }

                    const auto regs = this->get_regs();
                    const auto* reg_ptr = get_gp_register_pointer(regs, mapping.name);
                    if (const auto access = classify_gp_register_access(xreg))
                    {
                        const auto narrowed = *reg_ptr >> (access->offset * 8);
                        std::memcpy(value, &narrowed, (std::min)(size, access->width));
                    }
                    else
                    {
                        std::memcpy(value, reg_ptr, (std::min)(size, sizeof(*reg_ptr)));
                    }
                    return size;
                }
                case register_kind::segment: {
                    const auto sregs = this->get_sregs();
                    const auto& segment = get_segment_register(sregs, mapping.name);
                    if (xreg == x86_register::fs_base || xreg == x86_register::gs_base)
                    {
                        std::memcpy(value, &segment.base, (std::min)(size, sizeof(segment.base)));
                    }
                    else
                    {
                        std::memcpy(value, &segment.selector, (std::min)(size, sizeof(segment.selector)));
                    }
                    return size;
                }
                case register_kind::table: {
                    const auto sregs = this->get_sregs();
                    const auto& table = get_table_register(sregs, mapping.name);
                    std::memcpy(value, &table, (std::min)(size, sizeof(table)));
                    return size;
                }
                case register_kind::fp: {
                    const auto fpu = this->get_fpu();
                    const auto* fp = get_fp_register_pointer(fpu, mapping.name);
                    std::memcpy(value, fp, (std::min)(size, size_t{16}));
                    return size;
                }
                case register_kind::fp_control: {
                    const auto fpu = this->get_fpu();
                    if (xreg == x86_register::fpcw)
                    {
                        std::memcpy(value, &fpu.fcw, (std::min)(size, sizeof(fpu.fcw)));
                    }
                    else if (xreg == x86_register::fpsw)
                    {
                        std::memcpy(value, &fpu.fsw, (std::min)(size, sizeof(fpu.fsw)));
                    }
                    else
                    {
                        const auto fp_tag = static_cast<uint16_t>(fpu.ftwx);
                        std::memcpy(value, &fp_tag, (std::min)(size, sizeof(fp_tag)));
                    }
                    return size;
                }
                case register_kind::xmm_control: {
                    const auto fpu = this->get_fpu();
                    std::memcpy(value, &fpu.mxcsr, (std::min)(size, sizeof(fpu.mxcsr)));
                    return size;
                }
                case register_kind::reg128: {
                    const auto fpu = this->get_fpu();
                    const auto* xmm = get_xmm_register_pointer(fpu, mapping.name);
                    std::memcpy(value, xmm, (std::min)(size, size_t{16}));
                    return size;
                }
                }

                return size;
            }
            size_t write_raw_register(int reg, const void* value, size_t size) override
            {
                const auto xreg = static_cast<x86_register>(reg);
                const auto mapping = map_register(xreg);

                switch (mapping.kind)
                {
                case register_kind::reg64: {
                    if (mapping.name == register_name::efer || mapping.name == register_name::cr0 || mapping.name == register_name::cr2 ||
                        mapping.name == register_name::cr3 || mapping.name == register_name::cr4)
                    {
                        auto sregs = this->get_sregs();
                        void* target = nullptr;
                        switch (mapping.name)
                        {
                        case register_name::efer:
                            target = &sregs.efer;
                            break;
                        case register_name::cr0:
                            target = &sregs.cr0;
                            break;
                        case register_name::cr2:
                            target = &sregs.cr2;
                            break;
                        case register_name::cr3:
                            target = &sregs.cr3;
                            break;
                        case register_name::cr4:
                            target = &sregs.cr4;
                            break;
                        default:
                            break;
                        }
                        std::memcpy(target, value, (std::min)(size, sizeof(uint64_t)));
                        this->set_sregs(sregs);
                        return size;
                    }

                    if (mapping.name == register_name::dr0 || mapping.name == register_name::dr1 || mapping.name == register_name::dr2 ||
                        mapping.name == register_name::dr3 || mapping.name == register_name::dr6 || mapping.name == register_name::dr7)
                    {
                        auto debugregs = this->get_debugregs();
                        void* target = nullptr;
                        switch (mapping.name)
                        {
                        case register_name::dr0:
                            target = &debugregs.db[0];
                            break;
                        case register_name::dr1:
                            target = &debugregs.db[1];
                            break;
                        case register_name::dr2:
                            target = &debugregs.db[2];
                            break;
                        case register_name::dr3:
                            target = &debugregs.db[3];
                            break;
                        case register_name::dr6:
                            target = &debugregs.dr6;
                            break;
                        case register_name::dr7:
                            target = &debugregs.dr7;
                            break;
                        default:
                            break;
                        }
                        std::memcpy(target, value, (std::min)(size, sizeof(uint64_t)));
                        this->set_debugregs(debugregs);
                        return size;
                    }

                    auto regs = this->get_regs();
                    auto* reg_ptr = get_gp_register_pointer(regs, mapping.name);
                    if (const auto access = classify_gp_register_access(xreg))
                    {
                        uint64_t incoming = 0;
                        std::memcpy(&incoming, value, (std::min)(size, access->width));

                        if (access->zero_extend_32)
                        {
                            *reg_ptr = static_cast<uint32_t>(incoming);
                        }
                        else if (access->width >= sizeof(*reg_ptr))
                        {
                            *reg_ptr = incoming;
                        }
                        else
                        {
                            const auto mask = ((uint64_t{1} << (access->width * 8)) - 1) << (access->offset * 8);
                            *reg_ptr = (*reg_ptr & ~mask) | ((incoming << (access->offset * 8)) & mask);
                        }
                    }
                    else
                    {
                        std::memcpy(reg_ptr, value, (std::min)(size, sizeof(*reg_ptr)));
                    }

                    this->set_regs(regs);
                    return size;
                }
                case register_kind::segment: {
                    auto sregs = this->get_sregs();
                    auto& segment = get_segment_register(sregs, mapping.name);
                    if (xreg == x86_register::fs_base || xreg == x86_register::gs_base)
                    {
                        std::memcpy(&segment.base, value, (std::min)(size, sizeof(segment.base)));
                    }
                    else
                    {
                        std::memcpy(&segment.selector, value, (std::min)(size, sizeof(segment.selector)));
                    }
                    this->set_sregs(sregs);
                    return size;
                }
                case register_kind::table: {
                    auto sregs = this->get_sregs();
                    auto& table = get_table_register(sregs, mapping.name);
                    std::memcpy(&table, value, (std::min)(size, sizeof(table)));
                    this->set_sregs(sregs);
                    return size;
                }
                case register_kind::fp: {
                    auto fpu = this->get_fpu();
                    auto* fp = get_fp_register_pointer(fpu, mapping.name);
                    std::memcpy(fp, value, (std::min)(size, size_t{16}));
                    this->set_fpu(fpu);
                    return size;
                }
                case register_kind::fp_control: {
                    auto fpu = this->get_fpu();
                    if (xreg == x86_register::fpcw)
                    {
                        std::memcpy(&fpu.fcw, value, (std::min)(size, sizeof(fpu.fcw)));
                    }
                    else if (xreg == x86_register::fpsw)
                    {
                        std::memcpy(&fpu.fsw, value, (std::min)(size, sizeof(fpu.fsw)));
                    }
                    else
                    {
                        uint16_t fp_tag{};
                        std::memcpy(&fp_tag, value, (std::min)(size, sizeof(fp_tag)));
                        fpu.ftwx = static_cast<uint8_t>(fp_tag);
                    }
                    this->set_fpu(fpu);
                    return size;
                }
                case register_kind::xmm_control: {
                    auto fpu = this->get_fpu();
                    std::memcpy(&fpu.mxcsr, value, (std::min)(size, sizeof(fpu.mxcsr)));
                    this->set_fpu(fpu);
                    return size;
                }
                case register_kind::reg128: {
                    auto fpu = this->get_fpu();
                    auto* xmm = get_xmm_register_pointer(fpu, mapping.name);
                    std::memcpy(xmm, value, (std::min)(size, size_t{16}));
                    this->set_fpu(fpu);
                    return size;
                }
                }

                return size;
            }
            std::vector<std::byte> save_registers() const override
            {
                register_snapshot snapshot{};
                snapshot.regs = this->get_regs();
                snapshot.sregs = this->get_sregs();
                snapshot.xsave = this->get_xsave();
                snapshot.star = this->get_msr(MSR_STAR);
                snapshot.lstar = this->get_msr(MSR_LSTAR);
                snapshot.sfmask = this->get_msr(MSR_SYSCALL_MASK);

                std::vector<std::byte> bytes(sizeof(snapshot));
                std::memcpy(bytes.data(), &snapshot, sizeof(snapshot));
                return bytes;
            }
            void restore_registers(const std::vector<std::byte>& register_data) override
            {
                if (register_data.size() != sizeof(register_snapshot))
                {
                    throw std::runtime_error("Unexpected KVM register snapshot size");
                }

                register_snapshot snapshot{};
                std::memcpy(&snapshot, register_data.data(), sizeof(snapshot));
                this->set_regs(snapshot.regs);
                this->set_sregs(snapshot.sregs);
                this->set_xsave(snapshot.xsave);
                this->set_msr(MSR_STAR, snapshot.star);
                this->set_msr(MSR_LSTAR, snapshot.lstar);
                this->set_msr(MSR_SYSCALL_MASK, snapshot.sfmask);
            }
            bool has_violation() const override
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

            bool supports_multiple_vcpus() const override
            {
                // KVM could support multiple vCPUs per VM, but the backend is
                // single-vCPU for now (docs/multi-vcpu-design.md, Phase 5).
                return false;
            }

            std::string get_name() const override
            {
                return "Linux KVM";
            }
            void set_segment_base(x86_register base, pointer_type value) override
            {
                auto sregs = this->get_sregs();
                auto& segment = get_segment_register(sregs, map_register(base).name);
                segment.base = value;
                this->set_sregs(sregs);
            }
            pointer_type get_segment_base(x86_register base) override
            {
                const auto sregs = this->get_sregs();
                return get_segment_register(sregs, map_register(base).name).base;
            }
            void load_gdt(pointer_type address, uint32_t limit) override
            {
                auto sregs = this->get_sregs();
                sregs.gdt.base = address;
                sregs.gdt.limit = static_cast<uint16_t>(limit);
                this->set_sregs(sregs);
                this->install_exception_gdt_entries();
            }

            void read_memory(uint64_t address, void* data, size_t size) const override
            {
                if (!this->try_read_memory(address, data, size))
                {
                    throw std::runtime_error("Failed to read KVM guest memory");
                }
            }
            bool try_read_memory(uint64_t address, void* data, size_t size) const override
            {
                return detail::access_memory(this->mapped_pages_, address, data, size, false);
            }
            void write_memory(uint64_t address, const void* data, size_t size) override
            {
                if (!this->try_write_memory(address, data, size))
                {
                    throw std::runtime_error("Failed to write KVM guest memory");
                }
            }
            bool try_write_memory(uint64_t address, const void* data, size_t size) override
            {
                return detail::access_memory(this->mapped_pages_, address, const_cast<void*>(data), size, true);
            }

            // Fine-grained memory read/write/execution and basic-block hooks are registered for API
            // compatibility but never fire: the guest runs natively in the vCPU, so there is no
            // per-access/per-instruction instrumentation point without prohibitive single-stepping.
            // Callers that need these (some analyzer features) are unsupported under the KVM backend.
            emulator_hook* hook_memory_execution(memory_execution_hook_callback callback) override
            {
                auto* hook = this->make_hook();
                this->memory_execution_hooks_[hook] =
                    execution_hook_entry{.address = std::nullopt, .size = 0, .callback = std::move(callback)};
                return hook;
            }
            emulator_hook* hook_memory_execution(uint64_t address, memory_execution_hook_callback callback) override
            {
                auto* hook = this->make_hook();
                this->memory_execution_hooks_[hook] = execution_hook_entry{.address = address, .size = 1, .callback = std::move(callback)};
                return hook;
            }
            emulator_hook* hook_memory_range_execution(uint64_t address, uint64_t size, memory_execution_hook_callback callback) override
            {
                auto* hook = this->make_hook();
                this->memory_execution_hooks_[hook] =
                    execution_hook_entry{.address = address, .size = size, .callback = std::move(callback)};
                return hook;
            }
            emulator_hook* hook_memory_read(uint64_t address, uint64_t size, memory_access_hook_callback callback) override
            {
                auto* hook = this->make_hook();
                this->memory_read_hooks_[hook] =
                    memory_access_hook_entry{.address = address, .size = size, .callback = std::move(callback)};
                return hook;
            }
            emulator_hook* hook_memory_write(uint64_t address, uint64_t size, memory_access_hook_callback callback) override
            {
                auto* hook = this->make_hook();
                this->memory_write_hooks_[hook] =
                    memory_access_hook_entry{.address = address, .size = size, .callback = std::move(callback)};
                return hook;
            }
            emulator_hook* hook_instruction(int instruction_type, instruction_hook_callback callback) override
            {
                auto* hook = this->make_hook();
                auto& entry = this->instruction_hooks_[hook];
                entry.type = static_cast<x86_hookable_instructions>(instruction_type);
                entry.callback = std::move(callback);
                if (entry.type == x86_hookable_instructions::syscall)
                {
                    this->syscall_hook_ = &entry;
                }
                return hook;
            }
            emulator_hook* hook_interrupt(interrupt_hook_callback callback) override
            {
                auto* hook = this->make_hook();
                this->interrupt_hooks_[hook] = std::move(callback);
                return hook;
            }
            emulator_hook* hook_memory_violation(memory_violation_hook_callback callback) override
            {
                auto* hook = this->make_hook();
                this->memory_violation_hooks_[hook] = std::move(callback);
                return hook;
            }
            emulator_hook* hook_basic_block(basic_block_hook_callback callback) override
            {
                auto* hook = this->make_hook();
                this->basic_block_hooks_[hook] = std::move(callback);
                return hook;
            }
            void delete_hook(emulator_hook* hook) override
            {
                const auto instruction_it = this->instruction_hooks_.find(hook);
                if (instruction_it != this->instruction_hooks_.end() && &instruction_it->second == this->syscall_hook_)
                {
                    this->syscall_hook_ = nullptr;
                }

                this->instruction_hooks_.erase(hook);
                this->basic_block_hooks_.erase(hook);
                this->interrupt_hooks_.erase(hook);
                this->memory_violation_hooks_.erase(hook);
                this->memory_execution_hooks_.erase(hook);
                this->memory_read_hooks_.erase(hook);
                this->memory_write_hooks_.erase(hook);
            }

            void serialize_state(utils::buffer_serializer& buffer, bool) const override
            {
                buffer.write_vector(this->save_registers());
            }
            void deserialize_state(utils::buffer_deserializer& buffer, bool) override
            {
                this->restore_registers(buffer.read_vector<std::byte>());
            }

            void run_memory_execution_hooks(const uint64_t address)
            {
                std::vector<memory_execution_hook_callback> callbacks{};
                for (const auto& [_, hook] : this->memory_execution_hooks_)
                {
                    if (!hook.address || (hook.size != 0 && address >= *hook.address && address - *hook.address < hook.size))
                    {
                        callbacks.push_back(hook.callback);
                    }
                }

                for (const auto& callback : callbacks)
                {
                    callback(address);
                }
            }

          private:
            void map_mmio(uint64_t address, size_t size, mmio_read_callback read_cb, mmio_write_callback write_cb) override
            {
                if (!is_page_aligned(address) || !is_page_aligned(size))
                {
                    throw std::runtime_error("KVM MMIO mappings must be page aligned");
                }

                mmio_region region{.address = address, .size = size, .read_cb = std::move(read_cb), .write_cb = std::move(write_cb)};

                // Back MMIO with a read-only guest mapping rather than relying on KVM_EXIT_MMIO. KVM
                // services unmapped accesses by emulating the faulting instruction, but its emulator does
                // not support SSE/AVX, so a vectorized access (e.g. an SSE wcslen over a string in
                // KUSER_SHARED_DATA) would fault with #UD. A read-only memslot lets reads execute
                // natively; only writes trap and are routed through the write callback.
                for (size_t offset = 0; offset < size; offset += page_size)
                {
                    const auto guest_address = address + offset;
                    auto& page = this->mapped_pages_[guest_address];
                    if (!page)
                    {
                        page = std::make_unique<mapped_page>();
                    }

                    if (page->host_page == nullptr)
                    {
                        auto backing = allocate_backing_memory(page_size);
                        page->owned_page = std::move(backing);
                        page->host_page = page->owned_page.get();
                    }

                    page->permissions = memory_permission::read;
                    const auto chunk = (std::min)(static_cast<size_t>(page_size), size - offset);
                    region.read_cb(offset, page->host_page, chunk);
                    this->ensure_virtual_mapping(guest_address, this->ensure_guest_physical_page(*page));
                }

                this->rebuild_mappings();
                this->mmio_regions_[address] = std::move(region);
            }
            void map_memory(uint64_t address, size_t size, memory_permission permissions) override
            {
                if (!is_page_aligned(address) || !is_page_aligned(size))
                {
                    throw std::runtime_error("KVM memory mappings must be page aligned");
                }

                bool can_batch_map = true;
                for (size_t offset = 0; offset < size; offset += page_size)
                {
                    const auto guest_address = address + offset;
                    const auto entry = this->mapped_pages_.find(guest_address);
                    if (entry != this->mapped_pages_.end() && entry->second && entry->second->host_page != nullptr)
                    {
                        can_batch_map = false;
                        break;
                    }
                }

                if (can_batch_map)
                {
                    auto backing = allocate_backing_memory(size);
                    auto* backing_base = backing.get();
                    for (size_t offset = 0; offset < size; offset += page_size)
                    {
                        const auto guest_address = address + offset;
                        auto& page = this->mapped_pages_[guest_address];
                        if (!page)
                        {
                            page = std::make_unique<mapped_page>();
                        }

                        page->owned_page = backing;
                        page->host_page = backing_base + offset;
                        page->permissions = permissions;
                        this->ensure_virtual_mapping(guest_address, this->ensure_guest_physical_page(*page));
                    }

                    this->rebuild_mappings();
                    return;
                }

                for (size_t offset = 0; offset < size; offset += page_size)
                {
                    const auto guest_address = address + offset;
                    auto& page = this->mapped_pages_[guest_address];
                    if (!page)
                    {
                        page = std::make_unique<mapped_page>();
                    }

                    if (page->host_page == nullptr)
                    {
                        auto backing = allocate_backing_memory(page_size);
                        page->owned_page = std::move(backing);
                        page->host_page = page->owned_page.get();
                    }

                    page->permissions = permissions;
                    this->ensure_virtual_mapping(guest_address, this->ensure_guest_physical_page(*page));
                }

                this->rebuild_mappings();
            }
            void map_host_memory(uint64_t address, size_t size, void* host_pointer, memory_permission permissions) override
            {
                if (!is_page_aligned(address) || !is_page_aligned(size))
                {
                    throw std::runtime_error("KVM host memory mappings must be page aligned");
                }
                if ((reinterpret_cast<uintptr_t>(host_pointer) % page_size) != 0)
                {
                    throw std::runtime_error("KVM host memory mappings require a page-aligned host pointer");
                }

                // Alias the guest pages directly onto the caller's host memory (e.g. a GPU-shared buffer).
                // owned_page stays null so unmap never frees it; the KVM memslot maps the guest physical
                // page straight onto the host page, so guest and host see it coherently.
                auto* host_base = static_cast<uint8_t*>(host_pointer);
                for (size_t offset = 0; offset < size; offset += page_size)
                {
                    const auto guest_address = address + offset;
                    auto& page = this->mapped_pages_[guest_address];
                    if (!page)
                    {
                        page = std::make_unique<mapped_page>();
                    }

                    page->owned_page = nullptr;
                    page->host_page = host_base + offset;
                    page->permissions = permissions;
                    this->ensure_virtual_mapping(guest_address, this->ensure_guest_physical_page(*page));
                }

                this->rebuild_mappings();
            }
            bool host_memory_aliasing_is_coherent() const override
            {
                // KVM aliases the host pages into the guest as write-back cacheable, but a device sharing the
                // same physical memory (e.g. a GPU reading a DXVK buffer) may map it write-combined. The
                // guest's cached writes are therefore not guaranteed visible without an explicit flush.
                return false;
            }
            void flush_host_memory_cache(const void* host_pointer, size_t size) override
            {
                flush_cache_line_range(host_pointer, size);
            }
            void unmap_memory(uint64_t address, size_t size) override
            {
                if (!is_page_aligned(address) || !is_page_aligned(size))
                {
                    throw std::runtime_error("KVM memory unmappings must be page aligned");
                }

                for (size_t offset = 0; offset < size; offset += page_size)
                {
                    const auto entry = this->mapped_pages_.find(address + offset);
                    if (entry == this->mapped_pages_.end())
                    {
                        continue;
                    }
                    if (entry->second && entry->second->physical_page)
                    {
                        this->gpa_pages_.erase(*entry->second->physical_page);
                    }
                    this->mapped_pages_.erase(entry);
                }

                // Drop any MMIO region whose backing pages were just removed. Regions are mapped and
                // unmapped wholesale, so erase by overlap rather than requiring an exact size match;
                // otherwise a stale region would keep routing exits to a callback over dead pages.
                for (auto it = this->mmio_regions_.begin(); it != this->mmio_regions_.end();)
                {
                    if (it->first >= address && it->first < address + size)
                    {
                        it = this->mmio_regions_.erase(it);
                    }
                    else
                    {
                        ++it;
                    }
                }

                this->rebuild_mappings();
            }
            void apply_memory_protection(uint64_t address, size_t size, memory_permission permissions) override
            {
                if (!is_page_aligned(address) || !is_page_aligned(size))
                {
                    throw std::runtime_error("KVM protection changes must be page aligned");
                }

                // Only presence and writability (read-only vs read-write) are projected into KVM memslots;
                // finer protection bits (NX, read-vs-execute) are enforced elsewhere and never change a memslot.
                // A protection change that leaves the memslot projection identical (the common case, e.g. the
                // game's frequent NtProtectVirtualMemory) needs no reconciliation, so skip the O(total mappings)
                // rebuild unless a page's memslot contribution actually changes.
                constexpr uint32_t absent = ~0u;
                bool memslot_change = false;
                for (size_t offset = 0; offset < size; offset += page_size)
                {
                    const auto it = this->mapped_pages_.find(address + offset);
                    if (it == this->mapped_pages_.end() || !it->second)
                    {
                        continue;
                    }

                    auto& page = *it->second;
                    const bool present = page.host_page != nullptr;
                    const auto old_key =
                        (present && page.permissions != memory_permission::none) ? this->to_kvm_map_flags(page.permissions) : absent;
                    const auto new_key = (present && permissions != memory_permission::none) ? this->to_kvm_map_flags(permissions) : absent;
                    if (old_key != new_key)
                    {
                        memslot_change = true;
                    }

                    page.permissions = permissions;
                }

                if (memslot_change)
                {
                    this->rebuild_mappings();
                }
            }

            void ensure_platform_support()
            {
                this->kvm_fd_.reset(::open("/dev/kvm", O_RDWR | O_CLOEXEC));
                if (!this->kvm_fd_)
                {
                    throw_errno("open(/dev/kvm)");
                }

                const auto api_version = ::ioctl(this->kvm_fd_.get(), KVM_GET_API_VERSION, 0);
                check_ioctl_result(api_version, "KVM_GET_API_VERSION");
                if (api_version != KVM_API_VERSION)
                {
                    throw std::runtime_error("Unexpected KVM API version");
                }

                this->max_memslots_ = ::ioctl(this->kvm_fd_.get(), KVM_CHECK_EXTENSION, KVM_CAP_NR_MEMSLOTS);
                if (this->max_memslots_ <= 0)
                {
                    this->max_memslots_ = 32;
                }

                this->readonly_mem_supported_ = ::ioctl(this->kvm_fd_.get(), KVM_CHECK_EXTENSION, KVM_CAP_READONLY_MEM) > 0;
                static std::atomic_bool readonly_mem_warning_emitted{false};
                if (!this->readonly_mem_supported_ && !readonly_mem_warning_emitted.exchange(true, std::memory_order_relaxed))
                {
                    std::fputs("[WARN] KVM_CAP_READONLY_MEM is unavailable; MMIO writes and read-only memory protections may not trap.\n",
                               stderr);
                }

                // The thread-switch register snapshot relies on KVM_GET_XSAVE/KVM_SET_XSAVE.
                if (::ioctl(this->kvm_fd_.get(), KVM_CHECK_EXTENSION, KVM_CAP_XSAVE) <= 0)
                {
                    throw std::runtime_error("KVM does not support the required KVM_CAP_XSAVE extension");
                }

                this->vcpu_mmap_size_ = static_cast<size_t>(::ioctl(this->kvm_fd_.get(), KVM_GET_VCPU_MMAP_SIZE, 0));
                if (this->vcpu_mmap_size_ < sizeof(kvm_run))
                {
                    throw std::runtime_error("KVM vCPU mmap size is invalid");
                }
            }
            void configure_partition()
            {
                this->vm_fd_.reset(::ioctl(this->kvm_fd_.get(), KVM_CREATE_VM, 0));
                check_ioctl_result(this->vm_fd_.get(), "KVM_CREATE_VM");
                (void)::ioctl(this->vm_fd_.get(), KVM_SET_TSS_ADDR, 0xfffbd000);
            }
            void configure_virtual_processor()
            {
                this->vcpu_fd_.reset(::ioctl(this->vm_fd_.get(), KVM_CREATE_VCPU, vp_index));
                check_ioctl_result(this->vcpu_fd_.get(), "KVM_CREATE_VCPU");

                this->run_ = static_cast<kvm_run*>(
                    ::mmap(nullptr, this->vcpu_mmap_size_, PROT_READ | PROT_WRITE, MAP_SHARED, this->vcpu_fd_.get(), 0));
                if (this->run_ == MAP_FAILED)
                {
                    this->run_ = nullptr;
                    throw_errno("mmap(KVM_RUN)");
                }

                this->initialize_cpuid();
            }
            void initialize_cpuid()
            {
                constexpr uint32_t cpuid_entries = 256;
                std::vector<std::byte> buffer(sizeof(kvm_cpuid2) + cpuid_entries * sizeof(kvm_cpuid_entry2));
                auto* cpuid = reinterpret_cast<kvm_cpuid2*>(buffer.data());
                cpuid->nent = cpuid_entries;
                check_ioctl_result(::ioctl(this->kvm_fd_.get(), KVM_GET_SUPPORTED_CPUID, cpuid), "KVM_GET_SUPPORTED_CPUID");
                check_ioctl_result(::ioctl(this->vcpu_fd_.get(), KVM_SET_CPUID2, cpuid), "KVM_SET_CPUID2");
            }
            void initialize_virtual_processor_state()
            {
                auto sregs = this->get_sregs();
                sregs.cs = make_segment(0x33, true, true);
                sregs.ss = make_segment(0x2B, false, true);
                sregs.ds = make_segment(0x2B, false, true);
                sregs.es = make_segment(0x2B, false, true);
                sregs.fs = make_segment(0x53, false, true);
                sregs.gs = make_segment(0x2B, false, true);
                sregs.cr0 = 0x80000033ull; // PE | MP | ET | NE | PG
                sregs.cr4 = 0x620ull;      // PAE | OSFXSR | OSXMMEXCPT
                sregs.cr3 = this->pml4_gpa_;
                sregs.efer = (1ull << 0) | (1ull << 8) | (1ull << 10); // SCE | LME | LMA
                this->set_sregs(sregs);

                auto regs = this->get_regs();
                regs.rflags = 0x2ull;
                this->set_regs(regs);

                kvm_fpu fpu{};
                fpu.fcw = 0x037Fu;
                fpu.fsw = 0;
                fpu.ftwx = 0xFF;
                fpu.mxcsr = 0x1F80u;
                this->set_fpu(fpu);

                this->set_msr(MSR_STAR, (0x23ull << 48) | (0x08ull << 32));
                this->set_msr(MSR_SYSCALL_MASK, 0);
            }
            void initialize_syscall_intercept_page()
            {
                this->syscall_hook_page_ = this->allocate_internal_page(true);
                auto* code = static_cast<uint8_t*>(this->mapped_pages_.at(this->syscall_hook_page_)->host_page);
                code[0] = 0xF4;
                this->set_msr(MSR_LSTAR, this->syscall_hook_page_);
            }
            void initialize_exception_handling()
            {
                this->exception_stub_page_ = this->allocate_internal_page(true);
                this->exception_idt_page_ = this->allocate_internal_page(false);
                this->exception_tss_page_ = this->allocate_internal_page(false);
                this->exception_stack_page_ = this->allocate_internal_page(false);

                // One HLT per vector; the faulting vector is derived from the trapping RIP.
                auto* stubs = static_cast<uint8_t*>(this->mapped_pages_.at(this->exception_stub_page_)->host_page);
                for (uint32_t vector = 0; vector < exception_vector_count; ++vector)
                {
                    stubs[vector * exception_stub_stride] = 0xF4;
                }

                // Trampoline used to return into a 32-bit compatibility-mode (WOW64) context past the per-vector
                // stubs. KVM_SET_SREGS sets segment descriptors but cannot switch the vCPU out of 64-bit mode,
                // so compat faults are returned through a real IRETQ which performs the hardware mode transition
                // from the frame's CS (and reloads SS from the GDT). DS/ES are not part of the IRET frame and a
                // 64-bit data-segment load on this host can leave their cached descriptor with G=0 (1 MB limit),
                // which faults once compat mode enforces the limit; reload them (KGDT64_R3_DATA 0x2B) from the
                // GDT here so the flat 4 GB descriptor persists across the IRETQ. Runs at CPL0; RAX is preserved.
                //   push rax; mov eax,0x2B; mov ds,ax; mov es,ax; pop rax; iretq
                constexpr uint32_t iretq_stub_offset = exception_vector_count * exception_stub_stride;
                static constexpr std::array<uint8_t, 13> iretq_trampoline_code = {0x50, 0xB8, 0x2B, 0x00, 0x00, 0x00, 0x8E,
                                                                                  0xD8, 0x8E, 0xC0, 0x58, 0x48, 0xCF};
                std::memcpy(stubs + iretq_stub_offset, iretq_trampoline_code.data(), iretq_trampoline_code.size());
                this->iretq_trampoline_ = this->exception_stub_page_ + iretq_stub_offset;

                // 64-bit interrupt gates (DPL 3 so software int instructions are also trapped) using IST1.
                auto* idt = static_cast<uint8_t*>(this->mapped_pages_.at(this->exception_idt_page_)->host_page);
                for (uint32_t vector = 0; vector < exception_vector_count; ++vector)
                {
                    const auto handler = this->exception_stub_page_ + vector * exception_stub_stride;
                    const uint64_t low = (handler & 0xFFFF) | (static_cast<uint64_t>(kernel_code_selector) << 16) |
                                         (static_cast<uint64_t>(exception_ist_index & 0x7) << 32) | (static_cast<uint64_t>(0xEE) << 40) |
                                         (((handler >> 16) & 0xFFFF) << 48);
                    const uint64_t high = (handler >> 32) & 0xFFFFFFFF;
                    std::memcpy(idt + vector * 16, &low, sizeof(low));
                    std::memcpy(idt + vector * 16 + 8, &high, sizeof(high));
                }

                // 64-bit TSS providing the stack the CPU switches to on a privilege-changing exception.
                const auto stack_top = this->exception_stack_page_ + page_size;
                auto* tss = static_cast<uint8_t*>(this->mapped_pages_.at(this->exception_tss_page_)->host_page);
                std::memcpy(tss + 0x04, &stack_top, sizeof(stack_top)); // RSP0
                std::memcpy(tss + 0x24, &stack_top, sizeof(stack_top)); // IST1
                const uint16_t io_map_base = 0x68;
                std::memcpy(tss + 0x66, &io_map_base, sizeof(io_map_base));

                auto sregs = this->get_sregs();
                sregs.idt.base = this->exception_idt_page_;
                sregs.idt.limit = exception_vector_count * 16 - 1;
                sregs.tr.selector = task_state_selector;
                sregs.tr.base = this->exception_tss_page_;
                sregs.tr.limit = 0x67;
                sregs.tr.type = 11; // busy 64-bit TSS
                sregs.tr.s = 0;
                sregs.tr.present = 1;
                sregs.tr.dpl = 0;
                sregs.tr.db = 0;
                sregs.tr.l = 0;
                sregs.tr.g = 0;
                sregs.tr.avl = 0;
                sregs.tr.unusable = 0;
                this->set_sregs(sregs);
            }
            void install_exception_gdt_entries()
            {
                if (this->exception_tss_page_ == 0)
                {
                    return;
                }

                const auto sregs = this->get_sregs();
                const auto gdt_base = sregs.gdt.base;
                const auto gdt_limit = static_cast<uint64_t>(sregs.gdt.limit);
                const auto tss_offset = static_cast<uint64_t>(task_state_selector);
                if (gdt_base == 0 || gdt_limit < tss_offset + sizeof(uint64_t) * 2 - 1)
                {
                    return;
                }

                uint64_t code_descriptor = 0x00AF9B000000FFFFull;
                if (!detail::access_memory(this->mapped_pages_, gdt_base + kernel_code_selector, &code_descriptor, sizeof(code_descriptor),
                                           true))
                {
                    throw std::runtime_error("Failed to install KVM exception code descriptor");
                }

                const auto base = this->exception_tss_page_;
                const uint32_t limit = tss_descriptor_limit;
                uint64_t tss_low = (limit & 0xFFFFull) | ((base & 0xFFFFFFull) << 16) | (0x8Bull << 40) |
                                   (((static_cast<uint64_t>(limit) >> 16) & 0xFull) << 48) | (((base >> 24) & 0xFFull) << 56);
                uint64_t tss_high = base >> 32;
                const auto descriptor_address = gdt_base + task_state_selector;
                if (!detail::access_memory(this->mapped_pages_, descriptor_address, &tss_low, sizeof(tss_low), true) ||
                    !detail::access_memory(this->mapped_pages_, descriptor_address + sizeof(tss_low), &tss_high, sizeof(tss_high), true))
                {
                    throw std::runtime_error("Failed to install KVM exception TSS descriptor");
                }
            }
            void initialize_long_mode_page_tables()
            {
                this->pml4_gpa_ = this->allocate_internal_page(false, false);
            }
            uint64_t allocate_internal_page(bool executable = false, bool map_into_guest = true)
            {
                auto backing = allocate_backing_memory(page_size);
                auto* raw_page = backing.get();

                const auto page_gpa = this->next_internal_gpa_;
                this->next_internal_gpa_ += page_size;

                auto page = std::make_unique<mapped_page>();
                page->owned_page = std::move(backing);
                page->host_page = raw_page;
                page->permissions = executable ? memory_permission::all : memory_permission::read_write;
                page->physical_page = page_gpa;

                this->mapped_pages_[page_gpa] = std::move(page);
                if (!this->gpa_pages_.emplace(page_gpa, this->mapped_pages_[page_gpa].get()).second)
                {
                    throw std::logic_error("Duplicate KVM guest physical page");
                }
                this->page_table_views_[page_gpa] = reinterpret_cast<uint64_t*>(raw_page);

                if (map_into_guest)
                {
                    this->ensure_virtual_mapping(page_gpa, page_gpa);
                }

                this->rebuild_mappings();
                return page_gpa;
            }
            uint64_t allocate_guest_physical_page()
            {
                const auto page_gpa = this->next_guest_physical_page_;
                if (page_gpa >= internal_page_table_base || internal_page_table_base - page_gpa < page_size)
                {
                    throw std::runtime_error("Exhausted KVM guest physical page range");
                }

                this->next_guest_physical_page_ += page_size;
                return page_gpa;
            }
            uint64_t ensure_guest_physical_page(mapped_page& page)
            {
                if (!page.physical_page)
                {
                    page.physical_page = this->allocate_guest_physical_page();
                    if (!this->gpa_pages_.emplace(*page.physical_page, &page).second)
                    {
                        throw std::logic_error("Duplicate KVM guest physical page");
                    }
                }

                return *page.physical_page;
            }
            void ensure_virtual_mapping(uint64_t guest_address, uint64_t physical_page)
            {
                detail::ensure_virtual_mapping(
                    this->page_table_views_, this->pml4_gpa_,
                    [this](const bool executable, const bool map_into_guest) {
                        return this->allocate_internal_page(executable, map_into_guest);
                    },
                    guest_address, physical_page);
            }
            // Defer the (O(total mappings)) memslot reconciliation. A single high-level memory operation can
            // allocate several page-table pages, each of which would otherwise trigger its own full rebuild,
            // making memory mapping quadratic. Mark the layout dirty here and flush it once before the next
            // KVM_RUN: the memslots are only consumed by the guest (the emulator accesses guest memory through
            // host pointers, not memslots), so they need only be current when the vCPU actually runs.
            void rebuild_mappings()
            {
                this->mappings_dirty_ = true;
            }
            void flush_dirty_mappings()
            {
                if (!this->mappings_dirty_)
                {
                    return;
                }

                this->mappings_dirty_ = false;
                this->synchronize_memslots();
            }
            void synchronize_memslots()
            {
                // Project mapped_pages_ onto the desired memslot layout (physically contiguous,
                // host-contiguous runs of pages with the same flags become one memslot), then reconcile it
                // against the slots that are already installed, only deleting/creating the ones that
                // actually change. Every KVM_SET_USER_MEMORY_REGION forces an NPT/TLB rebuild, so leaving
                // unchanged slots in place is what keeps a memory operation from costing O(total mappings)
                // each time.
                struct desired_page
                {
                    uint64_t gpa = 0;
                    uint8_t* host = nullptr;
                    uint32_t flags = 0;
                };

                // Iterate the GPA-sorted view directly: it is already ordered and deduplicated (unique
                // physical addresses), so no per-flush sort or std::map rebuild is needed even with
                // hundreds of thousands of mapped pages.
                std::vector<desired_page> pages{};
                pages.reserve(this->gpa_pages_.size());
                for (const auto& [gpa, page] : this->gpa_pages_)
                {
                    if (!page || page->host_page == nullptr || page->permissions == memory_permission::none)
                    {
                        continue;
                    }

                    pages.push_back(desired_page{
                        .gpa = gpa, .host = static_cast<uint8_t*>(page->host_page), .flags = this->to_kvm_map_flags(page->permissions)});
                }

                std::map<uint64_t, installed_memslot> desired{};
                for (size_t i = 0; i < pages.size();)
                {
                    const auto run_gpa = pages[i].gpa;
                    auto* const run_host_base = pages[i].host;
                    const auto run_flags = pages[i].flags;

                    size_t run_size = page_size;
                    size_t j = i + 1;
                    while (j < pages.size() && pages[j].gpa == run_gpa + run_size && pages[j].host == run_host_base + run_size &&
                           pages[j].flags == run_flags)
                    {
                        run_size += page_size;
                        ++j;
                    }

                    desired[run_gpa] = installed_memslot{.size = run_size, .host = run_host_base, .flags = run_flags};
                    i = j;
                }

                for (auto it = this->current_slots_.begin(); it != this->current_slots_.end();)
                {
                    const auto entry = desired.find(it->first);
                    if (entry != desired.end() && entry->second.size == it->second.size && entry->second.host == it->second.host &&
                        entry->second.flags == it->second.flags)
                    {
                        desired.erase(entry);
                        ++it;
                    }
                    else
                    {
                        this->delete_memslot(it->second.id);
                        it = this->current_slots_.erase(it);
                    }
                }

                for (const auto& [run_gpa, run] : desired)
                {
                    const auto slot = this->allocate_slot_id();
                    this->set_memslot(slot, run_gpa, run.size, run.host, run.flags);
                    this->current_slots_[run_gpa] = installed_memslot{.id = slot, .size = run.size, .host = run.host, .flags = run.flags};
                }
            }
            int allocate_slot_id()
            {
                if (!this->free_slot_ids_.empty())
                {
                    const auto slot = this->free_slot_ids_.back();
                    this->free_slot_ids_.pop_back();
                    return slot;
                }

                if (this->next_slot_id_ >= this->max_memslots_)
                {
                    throw std::runtime_error("Exhausted KVM memory slots");
                }

                return this->next_slot_id_++;
            }
            void set_memslot(int slot, uint64_t guest_address, size_t size, void* host_base, uint32_t flags)
            {
                kvm_userspace_memory_region region{};
                region.slot = static_cast<uint32_t>(slot);
                region.flags = flags;
                region.guest_phys_addr = guest_address;
                region.memory_size = size;
                region.userspace_addr = reinterpret_cast<uint64_t>(host_base);
                if (::ioctl(this->vm_fd_.get(), KVM_SET_USER_MEMORY_REGION, &region) < 0)
                {
                    std::ostringstream stream;
                    stream << "KVM_SET_USER_MEMORY_REGION failed for slot " << slot << " gpa 0x" << std::hex << guest_address << " size 0x"
                           << size << " host 0x" << reinterpret_cast<uintptr_t>(host_base) << " flags 0x" << flags << ": "
                           << std::strerror(errno);
                    throw std::runtime_error(stream.str());
                }
            }
            void delete_memslot(int slot)
            {
                kvm_userspace_memory_region region{};
                region.slot = static_cast<uint32_t>(slot);
                region.memory_size = 0;
                check_ioctl_result(::ioctl(this->vm_fd_.get(), KVM_SET_USER_MEMORY_REGION, &region), "KVM_SET_USER_MEMORY_REGION");
                this->free_slot_ids_.push_back(slot);
            }
            uint32_t to_kvm_map_flags(memory_permission permissions) const
            {
                if (permissions == memory_permission::none)
                {
                    return 0;
                }

                uint32_t flags = 0;
                if (this->readonly_mem_supported_ && (permissions & memory_permission::write) == memory_permission::none)
                {
                    flags |= KVM_MEM_READONLY;
                }

                return flags;
            }
            void refresh_mmio_pages()
            {
                for (auto& [base, region] : this->mmio_regions_)
                {
                    for (size_t offset = 0; offset < region.size; offset += page_size)
                    {
                        const auto it = this->mapped_pages_.find(base + offset);
                        if (it != this->mapped_pages_.end() && it->second && it->second->host_page != nullptr)
                        {
                            const auto chunk = (std::min)(static_cast<size_t>(page_size), region.size - offset);
                            region.read_cb(offset, it->second->host_page, chunk);
                        }
                    }
                }
            }

            bool handle_pre_run_instruction()
            {
                std::array<std::byte, 3> opcode{};
                const auto rip = this->read_instruction_pointer();
                if (!detail::access_memory(this->mapped_pages_, rip, opcode.data(), opcode.size(), false))
                {
                    return false;
                }

                if (opcode[0] == std::byte{0x0F} && opcode[1] == std::byte{0xA2})
                {
                    return this->handle_instruction_hook(x86_hookable_instructions::cpuid, 2);
                }

                if (opcode[0] == std::byte{0x0F} && opcode[1] == std::byte{0x31})
                {
                    return this->handle_instruction_hook(x86_hookable_instructions::rdtsc, 2);
                }

                if (opcode[0] == std::byte{0x0F} && opcode[1] == std::byte{0x01} && opcode[2] == std::byte{0xF9})
                {
                    return this->handle_instruction_hook(x86_hookable_instructions::rdtscp, 3);
                }

                if (opcode[0] == int3_opcode)
                {
                    return this->handle_breakpoint_instruction();
                }

                if (opcode[0] == std::byte{0x0F} && opcode[1] == std::byte{0x0B})
                {
                    return this->handle_invalid_instruction_hook();
                }

                return false;
            }
            bool handle_breakpoint_instruction()
            {
                const auto rip = this->read_instruction_pointer();
                bool handled = false;
                bool rip_changed = false;
                for (auto& [_, hook] : this->interrupt_hooks_)
                {
                    hook(static_cast<int>(breakpoint_interrupt));
                    handled = true;
                    rip_changed = rip_changed || this->read_instruction_pointer() != rip;
                }

                if (handled && !rip_changed && !this->stop_requested_)
                {
                    this->advance_rip(1);
                }

                return handled;
            }
            bool handle_instruction_hook(x86_hookable_instructions type, uint64_t instruction_size)
            {
                // Capture RIP before the callbacks so the post-callback comparison can tell whether a
                // callback redirected execution; only advance past the instruction if it did not.
                const auto rip = this->read_instruction_pointer();

                bool handled = false;
                bool skip = false;
                for (auto& [_, hook] : this->instruction_hooks_)
                {
                    if (hook.type != type)
                    {
                        continue;
                    }

                    handled = true;
                    if (hook.callback(0) == instruction_hook_continuation::skip_instruction)
                    {
                        skip = true;
                    }
                }

                if (handled && skip)
                {
                    if (this->read_instruction_pointer() == rip)
                    {
                        this->advance_rip(instruction_size);
                    }

                    return true;
                }

                return false;
            }
            bool handle_invalid_instruction_hook()
            {
                bool consumed = false;
                const auto rip = this->read_instruction_pointer();
                for (auto& [_, hook] : this->instruction_hooks_)
                {
                    if (hook.type != x86_hookable_instructions::invalid)
                    {
                        continue;
                    }

                    if (hook.callback(0) == instruction_hook_continuation::skip_instruction)
                    {
                        consumed = true;
                    }
                }

                if (consumed && this->read_instruction_pointer() == rip)
                {
                    this->advance_rip(2);
                }

                return consumed;
            }
            std::optional<std::pair<mmio_region*, uint64_t>> find_mmio_region_for_physical_address(uint64_t physical_address)
            {
                for (auto& [base, region] : this->mmio_regions_)
                {
                    for (size_t offset = 0; offset < region.size; offset += page_size)
                    {
                        const auto it = this->mapped_pages_.find(base + offset);
                        if (it == this->mapped_pages_.end() || !it->second || !it->second->physical_page)
                        {
                            continue;
                        }

                        const auto page_gpa = *it->second->physical_page;
                        if (physical_address >= page_gpa && physical_address < page_gpa + page_size)
                        {
                            return std::make_pair(&region, offset + (physical_address - page_gpa));
                        }
                    }
                }

                return std::nullopt;
            }
            std::optional<uint64_t> translate_guest_physical_address(uint64_t physical_address)
            {
                for (auto& [guest_page, page] : this->mapped_pages_)
                {
                    if (!page || !page->physical_page)
                    {
                        continue;
                    }

                    const auto page_gpa = *page->physical_page;
                    if (physical_address >= page_gpa && physical_address < page_gpa + page_size)
                    {
                        return guest_page + (physical_address - page_gpa);
                    }
                }

                return std::nullopt;
            }
            bool handle_mmio_exit()
            {
                const auto& mmio = this->run_->mmio;
                if (const auto mapping = this->find_mmio_region_for_physical_address(mmio.phys_addr))
                {
                    auto* const region = mapping->first;
                    const auto offset = mapping->second;
                    if (mmio.is_write)
                    {
                        region->write_cb(offset, mmio.data, mmio.len);
                    }
                    else
                    {
                        region->read_cb(offset, this->run_->mmio.data, mmio.len);
                    }

                    return true;
                }

                const auto translated_guest_address = this->translate_guest_physical_address(mmio.phys_addr);
                const auto violation_address = translated_guest_address.value_or(mmio.phys_addr);
                const auto violation_type = translated_guest_address ? memory_violation_type::protection : memory_violation_type::unmapped;
                const auto operation = mmio.is_write ? memory_operation::write : memory_operation::read;
                for (auto& [_, hook] : this->memory_violation_hooks_)
                {
                    const auto result = hook(violation_address, mmio.len, operation, violation_type);
                    if (result == memory_violation_continuation::resume || result == memory_violation_continuation::restart)
                    {
                        return true;
                    }
                }

                return false;
            }
            bool handle_exception(uint32_t exception, uint64_t error_code)
            {
                if (exception == invalid_opcode_interrupt && this->handle_invalid_instruction_hook())
                {
                    return true;
                }

                if (exception == 14 && !this->memory_violation_hooks_.empty())
                {
                    const auto fault_address = this->reg<uint64_t>(x86_register::cr2);
                    const auto operation = (error_code & 0x2) ? memory_operation::write : memory_operation::read;
                    const auto type = (error_code & 0x1) ? memory_violation_type::protection : memory_violation_type::unmapped;
                    for (auto& [_, hook] : this->memory_violation_hooks_)
                    {
                        const auto result = hook(fault_address, 1, operation, type);
                        if (result == memory_violation_continuation::resume || result == memory_violation_continuation::restart)
                        {
                            return true;
                        }
                    }
                }

                bool handled = false;
                for (auto& [_, hook] : this->interrupt_hooks_)
                {
                    hook(static_cast<int>(exception));
                    handled = true;
                }

                return handled;
            }
            bool handle_exception_trap(uint64_t stub_rip)
            {
                const auto vector = static_cast<uint32_t>((stub_rip - 1 - this->exception_stub_page_) / exception_stub_stride);

                // The CPU pushed the exception frame onto the IST stack; RSP now points at its base.
                auto regs = this->get_regs();
                auto frame_address = regs.rsp;

                uint64_t error_code = 0;
                if (exception_has_error_code(vector))
                {
                    this->read_memory(frame_address, &error_code, sizeof(error_code));
                    frame_address += sizeof(uint64_t);
                }

                struct exception_frame
                {
                    uint64_t rip;
                    uint64_t cs;
                    uint64_t rflags;
                    uint64_t rsp;
                    uint64_t ss;
                } frame{};
                this->read_memory(frame_address, &frame, sizeof(frame));

                // A 32-bit compatibility-mode (WOW64) fault must be resumed through a real IRETQ: KVM_SET_SREGS
                // applies segment descriptors but cannot switch the vCPU out of 64-bit mode. 64-bit contexts
                // are resumed directly from the reconstructed state below.
                const bool compat_mode = (static_cast<uint16_t>(frame.cs) | 3u) != 0x33;

                // Undo the exception entry so the emulator's handlers observe the faulting context, and
                // a resumed instruction re-executes from where it faulted.
                regs.rip = frame.rip;
                if (vector == breakpoint_interrupt)
                {
                    // int3 is a trap: the CPU pushes the address *after* the 0xCC. The emulator's
                    // exception dispatch (and Windows) reports the breakpoint at the int3 itself and
                    // expects RIP to point there (e.g. to recognise the int-2Dh debug-service pattern),
                    // so rewind one byte. Instruction-precise backends naturally report this address.
                    regs.rip -= 1;
                }
                regs.rsp = frame.rsp;
                regs.rflags = frame.rflags;
                this->set_regs(regs);

                auto sregs = this->get_sregs();
                if (!compat_mode)
                {
                    sregs.cs = make_segment(static_cast<uint16_t>(frame.cs), true, (frame.cs & 3) == 3);
                    sregs.ss = make_segment(static_cast<uint16_t>(frame.ss), false, (frame.ss & 3) == 3);
                }
                // Refresh DS/ES from their selectors too. A 64-bit `mov ds` on this host can leave a G=0
                // (1 MB) cached descriptor; once compatibility mode (WOW64) enforces the limit, a data access
                // above 1 MB faults. The CPU does not save DS/ES in the exception frame, so rebuild them from
                // the current selectors as flat 4 GB segments, matching what the GDT describes.
                if (sregs.ds.selector & ~3u)
                {
                    sregs.ds = make_segment(sregs.ds.selector, false, (sregs.ds.selector & 3) == 3);
                }
                if (sregs.es.selector & ~3u)
                {
                    sregs.es = make_segment(sregs.es.selector, false, (sregs.es.selector & 3) == 3);
                }
                this->set_sregs(sregs);

                if (!this->handle_exception(vector, error_code))
                {
                    return false;
                }

                if (compat_mode)
                {
                    // Re-arm the exception frame with the (possibly handler-adjusted) register state and return
                    // through the CPL0 IRETQ trampoline, which loads CS from the GDT and switches the vCPU back
                    // into 32-bit compatibility mode. CS/SS stay at the kernel stub segments so the trampoline
                    // runs at CPL0; the IRETQ transitions to the CPL3 user context. DS/ES (set above) persist.
                    const auto resumed = this->get_regs();
                    const exception_frame iret_frame{
                        .rip = resumed.rip,
                        .cs = frame.cs,
                        .rflags = resumed.rflags,
                        .rsp = resumed.rsp,
                        .ss = frame.ss,
                    };
                    this->write_memory(frame_address, &iret_frame, sizeof(iret_frame));

                    auto trampoline = resumed;
                    trampoline.rsp = frame_address;
                    trampoline.rip = this->iretq_trampoline_;
                    // Clear TF for the trampoline itself: if the faulting context had single-stepping
                    // enabled, each trampoline instruction would raise a #DB before IRETQ runs, and since
                    // every vector shares IST1 that nested trap would overwrite the iret_frame staged
                    // above. IRETQ still restores the guest's real TF from iret_frame.rflags.
                    trampoline.rflags &= ~(1ULL << 8);
                    this->set_regs(trampoline);
                }

                return true;
            }
            void clear_pending_exception_state()
            {
                // After the synthetic IDT has delivered an exception and we have rewound the vCPU to the
                // faulting context, KVM may still hold a pending exception/interrupt event. On AMD (SVM)
                // that stale event gets re-injected on the next entry, cascading into a triple fault;
                // Intel (VMX) happens to tolerate it. Drop the pending event explicitly. Mirrors the WHP
                // backend fix for AMD hosts (PR #808).
                kvm_vcpu_events events{};
                if (::ioctl(this->vcpu_fd_.get(), KVM_GET_VCPU_EVENTS, &events) < 0)
                {
                    return;
                }

                events.exception.injected = 0;
                events.exception.pending = 0;
                events.exception.has_error_code = 0;
                events.exception.error_code = 0;
                events.exception_has_payload = 0;
                events.exception_payload = 0;
                events.interrupt.injected = 0;
                events.interrupt.shadow = 0;
                events.nmi.injected = 0;
                events.nmi.pending = 0;

                (void)::ioctl(this->vcpu_fd_.get(), KVM_SET_VCPU_EVENTS, &events);
            }
            bool handle_debug_exit()
            {
                const auto rip = this->read_instruction_pointer();
                auto vector = 1;
                std::byte opcode{};
                if (detail::access_memory(this->mapped_pages_, rip, &opcode, sizeof(opcode), false) && opcode == int3_opcode)
                {
                    vector = static_cast<int>(breakpoint_interrupt);
                }
                else if (rip > 0 && detail::access_memory(this->mapped_pages_, rip - 1, &opcode, sizeof(opcode), false) &&
                         opcode == int3_opcode)
                {
                    vector = static_cast<int>(breakpoint_interrupt);
                }

                bool handled = false;
                for (auto& [_, hook] : this->interrupt_hooks_)
                {
                    hook(vector);
                    handled = true;
                }

                return handled;
            }
            std::optional<uint64_t> handle_syscall_halt()
            {
                if (!this->syscall_hook_)
                {
                    return std::nullopt;
                }

                auto regs = this->get_regs();
                auto sregs = this->get_sregs();

                const auto post_syscall_rcx = regs.rcx;
                const auto post_syscall_r10 = regs.r10;
                const auto saved_rflags = regs.r11;
                const auto pre_syscall_rip = post_syscall_rcx - syscall_instruction_size;

                regs.rip = pre_syscall_rip;
                regs.rcx = post_syscall_r10;
                regs.rflags = saved_rflags;
                sregs.cs = make_segment(0x33, true, true);
                sregs.ss = make_segment(0x2B, false, true);
                this->set_regs(regs);
                this->set_sregs(sregs);

                const auto continuation = this->syscall_hook_->callback(0);

                regs = this->get_regs();
                if (continuation != instruction_hook_continuation::finalized_instruction_pointer)
                {
                    if (continuation == instruction_hook_continuation::skip_instruction && regs.rip == pre_syscall_rip)
                    {
                        regs.rip = post_syscall_rcx;
                    }
                    else
                    {
                        regs.rip += syscall_instruction_size;
                    }
                }

                sregs = this->get_sregs();
                sregs.cs = make_segment(0x33, true, true);
                sregs.ss = make_segment(0x2B, false, true);
                this->set_regs(regs);
                this->set_sregs(sregs);
                return pre_syscall_rip;
            }
            void advance_rip(uint64_t amount)
            {
                auto regs = this->get_regs();
                regs.rip += amount;
                this->set_regs(regs);
            }

            emulator_hook* make_hook()
            {
                return reinterpret_cast<emulator_hook*>(this->next_hook_id_++);
            }

            uint64_t get_msr(uint32_t msr) const
            {
                alignas(kvm_msrs) std::array<std::byte, sizeof(kvm_msrs) + sizeof(kvm_msr_entry)> storage{};
                auto* msrs = reinterpret_cast<kvm_msrs*>(storage.data());
                auto* entry = reinterpret_cast<kvm_msr_entry*>(storage.data() + offsetof(kvm_msrs, entries));
                msrs->nmsrs = 1;
                entry->index = msr;

                const auto rc = ::ioctl(this->vcpu_fd_.get(), KVM_GET_MSRS, msrs);
                if (rc != 1)
                {
                    throw std::runtime_error("KVM_GET_MSRS failed");
                }

                return entry->data;
            }
            void set_msr(uint32_t msr, uint64_t value)
            {
                alignas(kvm_msrs) std::array<std::byte, sizeof(kvm_msrs) + sizeof(kvm_msr_entry)> storage{};
                auto* msrs = reinterpret_cast<kvm_msrs*>(storage.data());
                auto* entry = reinterpret_cast<kvm_msr_entry*>(storage.data() + offsetof(kvm_msrs, entries));
                msrs->nmsrs = 1;
                entry->index = msr;
                entry->data = value;

                const auto rc = ::ioctl(this->vcpu_fd_.get(), KVM_SET_MSRS, msrs);
                if (rc != 1)
                {
                    throw std::runtime_error("KVM_SET_MSRS failed");
                }
            }
            // The general and segment registers are accessed many times per exit (syscall arguments,
            // results, the run loop's RIP checks, ...). KVM_GET_REGS/KVM_SET_REGS fetch and store the
            // whole register file, so doing one ioctl per single-register access is the dominant cost.
            // Cache both register sets: read fetches once and serves subsequent reads from the cache,
            // write updates the cache and marks it dirty, and the run loop flushes dirty state once
            // before KVM_RUN and invalidates the cache afterwards (the guest may have changed it).
            kvm_regs get_regs() const
            {
                if (!this->regs_cache_valid_)
                {
                    check_ioctl_result(::ioctl(this->vcpu_fd_.get(), KVM_GET_REGS, &this->regs_cache_), "KVM_GET_REGS");
                    this->regs_cache_valid_ = true;
                }
                return this->regs_cache_;
            }
            void set_regs(const kvm_regs& regs)
            {
                this->regs_cache_ = regs;
                this->regs_cache_valid_ = true;
                this->regs_cache_dirty_ = true;
            }
            kvm_sregs get_sregs() const
            {
                if (!this->sregs_cache_valid_)
                {
                    check_ioctl_result(::ioctl(this->vcpu_fd_.get(), KVM_GET_SREGS, &this->sregs_cache_), "KVM_GET_SREGS");
                    this->sregs_cache_valid_ = true;
                }
                return this->sregs_cache_;
            }
            void set_sregs(const kvm_sregs& sregs)
            {
                this->sregs_cache_ = sregs;
                this->sregs_cache_valid_ = true;
                this->sregs_cache_dirty_ = true;
            }
            void flush_register_cache()
            {
                if (this->regs_cache_dirty_)
                {
                    check_ioctl_result(::ioctl(this->vcpu_fd_.get(), KVM_SET_REGS, &this->regs_cache_), "KVM_SET_REGS");
                    this->regs_cache_dirty_ = false;
                }
                if (this->sregs_cache_dirty_)
                {
                    check_ioctl_result(::ioctl(this->vcpu_fd_.get(), KVM_SET_SREGS, &this->sregs_cache_), "KVM_SET_SREGS");
                    this->sregs_cache_dirty_ = false;
                }
            }
            void invalidate_register_cache()
            {
                this->regs_cache_valid_ = false;
                this->sregs_cache_valid_ = false;
            }
            kvm_fpu get_fpu() const
            {
                kvm_fpu fpu{};
                check_ioctl_result(::ioctl(this->vcpu_fd_.get(), KVM_GET_FPU, &fpu), "KVM_GET_FPU");
                return fpu;
            }
            void set_fpu(const kvm_fpu& fpu)
            {
                auto mutable_fpu = fpu;
                check_ioctl_result(::ioctl(this->vcpu_fd_.get(), KVM_SET_FPU, &mutable_fpu), "KVM_SET_FPU");
            }
            xsave_area get_xsave() const
            {
                // Captures the full extended state (x87, SSE, and the AVX YMM upper halves), unlike
                // KVM_GET_FPU which only sees the legacy fxsave area. Required so a thread preempted
                // mid-AVX keeps its complete vector state across a context switch.
                xsave_area xsave{};
                check_ioctl_result(::ioctl(this->vcpu_fd_.get(), KVM_GET_XSAVE, xsave.data()), "KVM_GET_XSAVE");
                return xsave;
            }
            void set_xsave(const xsave_area& xsave)
            {
                auto mutable_xsave = xsave;
                check_ioctl_result(::ioctl(this->vcpu_fd_.get(), KVM_SET_XSAVE, mutable_xsave.data()), "KVM_SET_XSAVE");
            }
            kvm_debugregs get_debugregs() const
            {
                kvm_debugregs debugregs{};
                check_ioctl_result(::ioctl(this->vcpu_fd_.get(), KVM_GET_DEBUGREGS, &debugregs), "KVM_GET_DEBUGREGS");
                return debugregs;
            }
            void set_debugregs(const kvm_debugregs& debugregs)
            {
                auto mutable_debugregs = debugregs;
                check_ioctl_result(::ioctl(this->vcpu_fd_.get(), KVM_SET_DEBUGREGS, &mutable_debugregs), "KVM_SET_DEBUGREGS");
            }

            static __u64* get_gp_register_pointer(kvm_regs& regs, register_name name)
            {
                switch (name)
                {
                case register_name::rax:
                    return &regs.rax;
                case register_name::rbx:
                    return &regs.rbx;
                case register_name::rcx:
                    return &regs.rcx;
                case register_name::rdx:
                    return &regs.rdx;
                case register_name::rsi:
                    return &regs.rsi;
                case register_name::rdi:
                    return &regs.rdi;
                case register_name::rbp:
                    return &regs.rbp;
                case register_name::rsp:
                    return &regs.rsp;
                case register_name::rip:
                    return &regs.rip;
                case register_name::r8:
                    return &regs.r8;
                case register_name::r9:
                    return &regs.r9;
                case register_name::r10:
                    return &regs.r10;
                case register_name::r11:
                    return &regs.r11;
                case register_name::r12:
                    return &regs.r12;
                case register_name::r13:
                    return &regs.r13;
                case register_name::r14:
                    return &regs.r14;
                case register_name::r15:
                    return &regs.r15;
                case register_name::rflags:
                    return &regs.rflags;
                default:
                    throw std::runtime_error("Unsupported KVM GP register");
                }
            }
            static const __u64* get_gp_register_pointer(const kvm_regs& regs, register_name name)
            {
                return get_gp_register_pointer(const_cast<kvm_regs&>(regs), name);
            }
            static kvm_segment& get_segment_register(kvm_sregs& sregs, register_name name)
            {
                switch (name)
                {
                case register_name::cs:
                    return sregs.cs;
                case register_name::ss:
                    return sregs.ss;
                case register_name::ds:
                    return sregs.ds;
                case register_name::es:
                    return sregs.es;
                case register_name::fs:
                    return sregs.fs;
                case register_name::gs:
                    return sregs.gs;
                default:
                    throw std::runtime_error("Unsupported KVM segment register");
                }
            }
            static const kvm_segment& get_segment_register(const kvm_sregs& sregs, register_name name)
            {
                return get_segment_register(const_cast<kvm_sregs&>(sregs), name);
            }
            static kvm_dtable& get_table_register(kvm_sregs& sregs, register_name name)
            {
                switch (name)
                {
                case register_name::gdtr:
                    return sregs.gdt;
                case register_name::idtr:
                    return sregs.idt;
                default:
                    throw std::runtime_error("Unsupported KVM table register");
                }
            }
            static const kvm_dtable& get_table_register(const kvm_sregs& sregs, register_name name)
            {
                return get_table_register(const_cast<kvm_sregs&>(sregs), name);
            }
            static uint8_t* get_fp_register_pointer(kvm_fpu& fpu, register_name name)
            {
                const auto index = static_cast<int>(name) - static_cast<int>(register_name::fp0);
                if (index < 0 || index >= 8)
                {
                    throw std::runtime_error("Unsupported KVM FP register");
                }

                return fpu.fpr[index];
            }
            static const uint8_t* get_fp_register_pointer(const kvm_fpu& fpu, register_name name)
            {
                return get_fp_register_pointer(const_cast<kvm_fpu&>(fpu), name);
            }
            static uint8_t* get_xmm_register_pointer(kvm_fpu& fpu, register_name name)
            {
                const auto index = static_cast<int>(name) - static_cast<int>(register_name::xmm0);
                if (index < 0 || index >= 16)
                {
                    throw std::runtime_error("Unsupported KVM XMM register");
                }

                return fpu.xmm[index];
            }
            static const uint8_t* get_xmm_register_pointer(const kvm_fpu& fpu, register_name name)
            {
                return get_xmm_register_pointer(const_cast<kvm_fpu&>(fpu), name);
            }

            file_descriptor kvm_fd_{};
            file_descriptor vm_fd_{};
            file_descriptor vcpu_fd_{};
            size_t vcpu_mmap_size_ = 0;
            kvm_run* run_ = nullptr;

            mutable kvm_regs regs_cache_{};
            mutable kvm_sregs sregs_cache_{};
            mutable bool regs_cache_valid_ = false;
            mutable bool sregs_cache_valid_ = false;
            bool regs_cache_dirty_ = false;
            bool sregs_cache_dirty_ = false;

            struct installed_memslot
            {
                int id = -1;
                size_t size = 0;
                void* host = nullptr;
                uint32_t flags = 0;
            };

            int max_memslots_ = 0;
            bool readonly_mem_supported_ = false;
            int next_slot_id_ = 0;
            std::map<uint64_t, installed_memslot> current_slots_{};
            bool mappings_dirty_ = false;
            std::vector<int> free_slot_ids_{};
            std::map<uint64_t, std::unique_ptr<mapped_page>> mapped_pages_{};
            // GPA-sorted view of mapped_pages_ (guest physical address -> page), kept in sync so
            // synchronize_memslots can project in memslot order without re-sorting every flush.
            std::map<uint64_t, mapped_page*> gpa_pages_{};
            std::unordered_map<uint64_t, uint64_t*> page_table_views_{};
            uint64_t pml4_gpa_ = 0;
            uint64_t next_guest_physical_page_ = guest_physical_page_base;
            uint64_t next_internal_gpa_ = internal_page_table_base;
            std::atomic_bool stop_requested_ = false;
            std::atomic_bool run_active_ = false;
            std::atomic<pthread_t> vcpu_thread_{};
            int kick_signal_ = 0;
            uint64_t syscall_hook_page_ = 0;
            uint64_t exception_stub_page_ = 0;
            uint64_t iretq_trampoline_ = 0;
            uint64_t exception_idt_page_ = 0;
            uint64_t exception_tss_page_ = 0;
            uint64_t exception_stack_page_ = 0;
            size_t next_hook_id_ = 1;

            std::unordered_map<emulator_hook*, instruction_hook_entry> instruction_hooks_{};
            std::unordered_map<emulator_hook*, basic_block_hook_callback> basic_block_hooks_{};
            std::unordered_map<emulator_hook*, interrupt_hook_callback> interrupt_hooks_{};
            std::unordered_map<emulator_hook*, memory_violation_hook_callback> memory_violation_hooks_{};
            std::unordered_map<emulator_hook*, execution_hook_entry> memory_execution_hooks_{};
            std::unordered_map<emulator_hook*, memory_access_hook_entry> memory_read_hooks_{};
            std::unordered_map<emulator_hook*, memory_access_hook_entry> memory_write_hooks_{};
            std::map<uint64_t, mmio_region> mmio_regions_{};
            instruction_hook_entry* syscall_hook_ = nullptr;
        };

        kvm_segment make_segment(const uint16_t selector, const bool is_code, const bool is_user)
        {
            // A 64-bit code segment runs in long mode (L=1, D=0); 32-bit compatibility-mode code (the WOW64
            // selector 0x23) and all data segments use D=1, L=0. Deriving this from the selector is required
            // when reconstructing a faulting WOW64 context: hardcoding L=1 on a 32-bit code selector re-enters
            // the 32-bit code in 64-bit mode and misdecodes it. Windows x64 uses 0x33 for 64-bit user code.
            const bool long_mode_code = is_code && ((selector | 3) == 0x33);

            kvm_segment segment{};
            segment.base = 0;
            segment.limit = 0xFFFFF;
            segment.selector = selector;
            segment.type = is_code ? 0xB : 0x3;
            segment.present = 1;
            segment.dpl = is_user ? 3 : 0;
            segment.db = long_mode_code ? 0 : 1;
            segment.s = 1;
            segment.l = long_mode_code ? 1 : 0;
            segment.g = 1;
            segment.avl = 0;
            segment.unusable = 0;
            return segment;
        }

        register_mapping map_register(const x86_register reg)
        {
            switch (reg)
            {
            case x86_register::al:
            case x86_register::ah:
            case x86_register::ax:
            case x86_register::eax:
            case x86_register::rax:
                return {.name = register_name::rax, .kind = register_kind::reg64, .logical_size = sizeof(uint64_t)};
            case x86_register::bl:
            case x86_register::bh:
            case x86_register::bx:
            case x86_register::ebx:
            case x86_register::rbx:
                return {.name = register_name::rbx, .kind = register_kind::reg64, .logical_size = sizeof(uint64_t)};
            case x86_register::cl:
            case x86_register::ch:
            case x86_register::cx:
            case x86_register::ecx:
            case x86_register::rcx:
                return {.name = register_name::rcx, .kind = register_kind::reg64, .logical_size = sizeof(uint64_t)};
            case x86_register::dl:
            case x86_register::dh:
            case x86_register::dx:
            case x86_register::edx:
            case x86_register::rdx:
                return {.name = register_name::rdx, .kind = register_kind::reg64, .logical_size = sizeof(uint64_t)};
            case x86_register::si:
            case x86_register::sil:
            case x86_register::esi:
            case x86_register::rsi:
                return {.name = register_name::rsi, .kind = register_kind::reg64, .logical_size = sizeof(uint64_t)};
            case x86_register::di:
            case x86_register::dil:
            case x86_register::edi:
            case x86_register::rdi:
                return {.name = register_name::rdi, .kind = register_kind::reg64, .logical_size = sizeof(uint64_t)};
            case x86_register::bp:
            case x86_register::bpl:
            case x86_register::ebp:
            case x86_register::rbp:
                return {.name = register_name::rbp, .kind = register_kind::reg64, .logical_size = sizeof(uint64_t)};
            case x86_register::sp:
            case x86_register::spl:
            case x86_register::esp:
            case x86_register::rsp:
                return {.name = register_name::rsp, .kind = register_kind::reg64, .logical_size = sizeof(uint64_t)};
            case x86_register::ip:
            case x86_register::eip:
            case x86_register::rip:
                return {.name = register_name::rip, .kind = register_kind::reg64, .logical_size = sizeof(uint64_t)};
            case x86_register::r8:
            case x86_register::r8d:
            case x86_register::r8w:
            case x86_register::r8b:
                return {.name = register_name::r8, .kind = register_kind::reg64, .logical_size = sizeof(uint64_t)};
            case x86_register::r9:
            case x86_register::r9d:
            case x86_register::r9w:
            case x86_register::r9b:
                return {.name = register_name::r9, .kind = register_kind::reg64, .logical_size = sizeof(uint64_t)};
            case x86_register::r10:
            case x86_register::r10d:
            case x86_register::r10w:
            case x86_register::r10b:
                return {.name = register_name::r10, .kind = register_kind::reg64, .logical_size = sizeof(uint64_t)};
            case x86_register::r11:
            case x86_register::r11d:
            case x86_register::r11w:
            case x86_register::r11b:
                return {.name = register_name::r11, .kind = register_kind::reg64, .logical_size = sizeof(uint64_t)};
            case x86_register::r12:
            case x86_register::r12d:
            case x86_register::r12w:
            case x86_register::r12b:
                return {.name = register_name::r12, .kind = register_kind::reg64, .logical_size = sizeof(uint64_t)};
            case x86_register::r13:
            case x86_register::r13d:
            case x86_register::r13w:
            case x86_register::r13b:
                return {.name = register_name::r13, .kind = register_kind::reg64, .logical_size = sizeof(uint64_t)};
            case x86_register::r14:
            case x86_register::r14d:
            case x86_register::r14w:
            case x86_register::r14b:
                return {.name = register_name::r14, .kind = register_kind::reg64, .logical_size = sizeof(uint64_t)};
            case x86_register::r15:
            case x86_register::r15d:
            case x86_register::r15w:
            case x86_register::r15b:
                return {.name = register_name::r15, .kind = register_kind::reg64, .logical_size = sizeof(uint64_t)};
            case x86_register::flags:
            case x86_register::eflags:
            case x86_register::rflags:
                return {.name = register_name::rflags, .kind = register_kind::reg64, .logical_size = sizeof(uint64_t)};
            case x86_register::cs:
                return {.name = register_name::cs, .kind = register_kind::segment, .logical_size = sizeof(uint16_t)};
            case x86_register::ss:
                return {.name = register_name::ss, .kind = register_kind::segment, .logical_size = sizeof(uint16_t)};
            case x86_register::ds:
                return {.name = register_name::ds, .kind = register_kind::segment, .logical_size = sizeof(uint16_t)};
            case x86_register::es:
                return {.name = register_name::es, .kind = register_kind::segment, .logical_size = sizeof(uint16_t)};
            case x86_register::fs:
            case x86_register::fs_base:
                return {.name = register_name::fs, .kind = register_kind::segment, .logical_size = sizeof(uint16_t)};
            case x86_register::gs:
            case x86_register::gs_base:
                return {.name = register_name::gs, .kind = register_kind::segment, .logical_size = sizeof(uint16_t)};
            case x86_register::gdtr:
                return {.name = register_name::gdtr, .kind = register_kind::table, .logical_size = sizeof(kvm_dtable)};
            case x86_register::idtr:
                return {.name = register_name::idtr, .kind = register_kind::table, .logical_size = sizeof(kvm_dtable)};
            case x86_register::cr0:
                return {.name = register_name::cr0, .kind = register_kind::reg64, .logical_size = sizeof(uint64_t)};
            case x86_register::cr2:
                return {.name = register_name::cr2, .kind = register_kind::reg64, .logical_size = sizeof(uint64_t)};
            case x86_register::cr3:
                return {.name = register_name::cr3, .kind = register_kind::reg64, .logical_size = sizeof(uint64_t)};
            case x86_register::cr4:
                return {.name = register_name::cr4, .kind = register_kind::reg64, .logical_size = sizeof(uint64_t)};
            case x86_register::dr0:
                return {.name = register_name::dr0, .kind = register_kind::reg64, .logical_size = sizeof(uint64_t)};
            case x86_register::dr1:
                return {.name = register_name::dr1, .kind = register_kind::reg64, .logical_size = sizeof(uint64_t)};
            case x86_register::dr2:
                return {.name = register_name::dr2, .kind = register_kind::reg64, .logical_size = sizeof(uint64_t)};
            case x86_register::dr3:
                return {.name = register_name::dr3, .kind = register_kind::reg64, .logical_size = sizeof(uint64_t)};
            case x86_register::dr6:
                return {.name = register_name::dr6, .kind = register_kind::reg64, .logical_size = sizeof(uint64_t)};
            case x86_register::dr7:
                return {.name = register_name::dr7, .kind = register_kind::reg64, .logical_size = sizeof(uint64_t)};
            case x86_register::fpcw:
            case x86_register::fpsw:
            case x86_register::fptag:
                return {.name = register_name::fp_control_status, .kind = register_kind::fp_control, .logical_size = sizeof(uint16_t)};
            case x86_register::mxcsr:
                return {.name = register_name::xmm_control_status, .kind = register_kind::xmm_control, .logical_size = sizeof(uint32_t)};
            case x86_register::msr:
                return {.name = register_name::efer, .kind = register_kind::reg64, .logical_size = sizeof(uint64_t)};
            default:
                break;
            }

            if (reg >= x86_register::xmm0 && reg <= x86_register::xmm15)
            {
                return {.name = static_cast<register_name>(static_cast<int>(register_name::xmm0) +
                                                           (static_cast<int>(reg) - static_cast<int>(x86_register::xmm0))),
                        .kind = register_kind::reg128,
                        .logical_size = 16};
            }

            if (reg >= x86_register::st0 && reg <= x86_register::st7)
            {
                return {.name = static_cast<register_name>(static_cast<int>(register_name::fp0) +
                                                           (static_cast<int>(reg) - static_cast<int>(x86_register::st0))),
                        .kind = register_kind::fp,
                        .logical_size = 16};
            }

            throw std::runtime_error("Unsupported KVM register");
        }

    }

    std::unique_ptr<x86_64_emulator> create_x86_64_emulator()
    {
        return std::make_unique<kvm_x86_64_emulator>();
    }
} // namespace sogen::kvm
