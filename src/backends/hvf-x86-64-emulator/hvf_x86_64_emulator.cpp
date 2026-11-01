#define HVF_X86_64_EMULATOR_IMPL
#include "hvf_x86_64_emulator.hpp"

#include <Hypervisor/Hypervisor.h>
#include <Security/Security.h>
#include <cpuid.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <x86intrin.h>

#include <utils/finally.hpp>
#include <utils/object.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace sogen::hvf
{
    namespace
    {
        constexpr uint64_t page_size = 0x1000;
        constexpr uint64_t user_address_limit = 1ull << 47;
        // Translation tables and guest pages get guest physical addresses from separate pools; neither is mapped
        // into the guest's linear address space.
        constexpr uint64_t table_physical_base = 0x10000000;
        constexpr uint64_t table_pool_size = 0x04000000;
        constexpr uint64_t guest_physical_base = 1ull << 32;
        constexpr uint64_t pool_chunk_size = 64ull << 20;

        constexpr uint64_t pte_present = 1ull << 0;
        constexpr uint64_t pte_writable = 1ull << 1;
        constexpr uint64_t pte_user = 1ull << 2;
        constexpr uint64_t pte_address_mask = 0x000FFFFFFFFFF000ull;

        constexpr uint64_t cr0_pe = 1ull << 0;
        constexpr uint64_t cr0_mp = 1ull << 1;
        constexpr uint64_t cr0_et = 1ull << 4;
        constexpr uint64_t cr0_ne = 1ull << 5;
        constexpr uint64_t cr0_pg = 1ull << 31;
        constexpr uint64_t cr4_pae = 1ull << 5;
        constexpr uint64_t cr4_osfxsr = 1ull << 9;
        constexpr uint64_t cr4_osxmmexcpt = 1ull << 10;
        constexpr uint64_t cr4_osxsave = 1ull << 18;
        // EFER.SCE stays clear, so SYSCALL raises #UD at the instruction with every register intact; the exception
        // exit hands it to the syscall hook. No ring-0 entry point is needed.
        constexpr uint64_t efer_lme = 1ull << 8;
        constexpr uint64_t efer_lma = 1ull << 10;
        constexpr uint64_t rflags_reserved = 1ull << 1;

        constexpr uint16_t user_code64_selector = 0x33;
        constexpr uint64_t syscall_instruction_size = 2;
        constexpr uint64_t software_interrupt_size = 2;

        // VMX segment access rights: type[3:0] S[4] DPL[6:5] P[7] L[13] D/B[14] G[15] unusable[16].
        constexpr uint64_t ar_code64 = 0xA0FB;
        constexpr uint64_t ar_code32 = 0xC0FB;
        constexpr uint64_t ar_data = 0xC0F3;
        constexpr uint64_t ar_unusable = 1ull << 16;
        constexpr uint64_t ar_busy_tss64 = 0x8B;

        // VM-exit interruption information: vector[7:0] type[10:8] error code valid[11].
        constexpr uint64_t interruption_vector_mask = 0xFF;
        constexpr uint64_t interruption_has_error_code = 1ull << 11;

        // EPT violation exit qualification.
        constexpr uint64_t ept_access_write = 1ull << 1;
        constexpr uint64_t ept_access_fetch = 1ull << 2;
        constexpr uint64_t ept_linear_address_valid = 1ull << 7;

        constexpr uint32_t vmx_reason_rdtscp = 51;

        constexpr int divide_error = 0;
        constexpr int debug_exception = 1;
        constexpr int breakpoint_exception = 3;
        constexpr int invalid_opcode = 6;
        constexpr int general_protection = 13;
        constexpr int page_fault = 14;
        constexpr int x87_floating_point = 16;
        constexpr int simd_floating_point = 19;

        constexpr size_t fpstate_size = 0x1000;
        constexpr size_t fxsave_fcw = 0;
        constexpr size_t fxsave_fsw = 2;
        constexpr size_t fxsave_ftw = 4;
        constexpr size_t fxsave_mxcsr = 24;
        constexpr size_t fxsave_st0 = 32;
        constexpr size_t fxsave_xmm0 = 160;

        enum class segment : uint32_t
        {
            es,
            cs,
            ss,
            ds,
            fs,
            gs,
            ldtr,
            tr,
        };
        constexpr size_t segment_count = 8;

        uint32_t selector_field(const segment seg)
        {
            return VMCS_GUEST_ES + 2 * static_cast<uint32_t>(seg);
        }

        uint32_t base_field(const segment seg)
        {
            return VMCS_GUEST_ES_BASE + 2 * static_cast<uint32_t>(seg);
        }

        uint32_t limit_field(const segment seg)
        {
            return VMCS_GUEST_ES_LIMIT + 2 * static_cast<uint32_t>(seg);
        }

        uint32_t access_rights_field(const segment seg)
        {
            return VMCS_GUEST_ES_AR + 2 * static_cast<uint32_t>(seg);
        }

        [[noreturn]] void throw_hv(const hv_return_t result, const char* action)
        {
            std::ostringstream message{};
            message << action << " failed with hv_return_t 0x" << std::hex << static_cast<uint32_t>(result);
            throw std::runtime_error(message.str());
        }

        void check_hv(const hv_return_t result, const char* action)
        {
            if (result != HV_SUCCESS)
            {
                throw_hv(result, action);
            }
        }

        void log_cleanup_failure(const hv_return_t result, const char* action) noexcept
        {
            if (result != HV_SUCCESS)
            {
                std::fprintf(stderr, "%s failed during cleanup: hv_return_t 0x%08X\n", action, static_cast<uint32_t>(result));
            }
        }

        uint64_t read_capability(const hv_vmx_capability_t field)
        {
            uint64_t value{};
            check_hv(hv_vmx_read_capability(field, &value), "hv_vmx_read_capability");
            return value;
        }

        // A VMX control capability holds the bits that must be one in its low half and the bits that may be one in
        // its high half.
        uint64_t adjust_controls(const hv_vmx_capability_t field, const uint64_t requested)
        {
            const auto capability = read_capability(field);
            const auto adjusted = (requested | (capability & 0xFFFFFFFF)) & (capability >> 32);
            if ((adjusted & requested) != requested)
            {
                std::ostringstream message{};
                message << "The host does not support the VMX controls 0x" << std::hex << (requested & ~adjusted) << " of capability "
                        << std::dec << static_cast<int>(field);
                throw std::runtime_error(message.str());
            }
            return adjusted;
        }

        std::byte* allocate_host(const size_t size)
        {
            auto* memory = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
            if (memory == MAP_FAILED)
            {
                throw std::runtime_error("mmap failed while backing HVF x86-64 memory");
            }
            return static_cast<std::byte*>(memory);
        }

        struct host_allocation
        {
            std::byte* memory{};
            size_t size{};

            explicit host_allocation(const size_t length)
                : memory(allocate_host(length)),
                  size(length)
            {
            }

            ~host_allocation()
            {
                if (memory && munmap(memory, size) != 0)
                {
                    std::fprintf(stderr, "munmap failed during HVF x86-64 cleanup\n");
                }
            }

            host_allocation(const host_allocation&) = delete;
            host_allocation& operator=(const host_allocation&) = delete;
            host_allocation(host_allocation&&) = delete;
            host_allocation& operator=(host_allocation&&) = delete;
        };

        struct vm_handle
        {
            vm_handle()
            {
                const auto result = hv_vm_create(HV_VM_DEFAULT);
                if (result == HV_BUSY)
                {
                    throw std::runtime_error("Hypervisor.framework allows one VM per process; destroy the other HVF emulator first");
                }
                check_hv(result, "hv_vm_create");
            }

            ~vm_handle()
            {
                log_cleanup_failure(hv_vm_destroy(), "hv_vm_destroy");
            }

            vm_handle(const vm_handle&) = delete;
            vm_handle& operator=(const vm_handle&) = delete;
            vm_handle(vm_handle&&) = delete;
            vm_handle& operator=(vm_handle&&) = delete;
        };

        struct mmio_region
        {
            uint64_t address{};
            size_t size{};
            mmio_read_callback read{};
            mmio_write_callback write{};
        };

        struct mapped_page
        {
            std::byte* host{};
            uint64_t gpa{};
            bool owned{};
            // Pages without any access are absent from the EPT rather than mapped with no permissions.
            bool present{};
            memory_permission permissions{};
            std::shared_ptr<mmio_region> mmio{};
        };

        struct translation_table
        {
            uint64_t* entries{};
            uint64_t gpa{};
            uint32_t used{};
        };

        enum class register_kind : uint8_t
        {
            general,
            segment_selector,
            segment_base,
            control,
            debug,
            table,
            x87,
            x87_control,
            mxcsr,
            xmm,
            efer,
        };

        struct register_mapping
        {
            register_kind kind{};
            uint32_t id{};
        };

        std::optional<hv_x86_reg_t> general_register(const x86_register reg)
        {
            switch (reg)
            {
            case x86_register::al:
            case x86_register::ah:
            case x86_register::ax:
            case x86_register::eax:
            case x86_register::rax:
                return HV_X86_RAX;
            case x86_register::bl:
            case x86_register::bh:
            case x86_register::bx:
            case x86_register::ebx:
            case x86_register::rbx:
                return HV_X86_RBX;
            case x86_register::cl:
            case x86_register::ch:
            case x86_register::cx:
            case x86_register::ecx:
            case x86_register::rcx:
                return HV_X86_RCX;
            case x86_register::dl:
            case x86_register::dh:
            case x86_register::dx:
            case x86_register::edx:
            case x86_register::rdx:
                return HV_X86_RDX;
            case x86_register::sil:
            case x86_register::si:
            case x86_register::esi:
            case x86_register::rsi:
                return HV_X86_RSI;
            case x86_register::dil:
            case x86_register::di:
            case x86_register::edi:
            case x86_register::rdi:
                return HV_X86_RDI;
            case x86_register::bpl:
            case x86_register::bp:
            case x86_register::ebp:
            case x86_register::rbp:
                return HV_X86_RBP;
            case x86_register::spl:
            case x86_register::sp:
            case x86_register::esp:
            case x86_register::rsp:
                return HV_X86_RSP;
            case x86_register::ip:
            case x86_register::eip:
            case x86_register::rip:
                return HV_X86_RIP;
            case x86_register::flags:
            case x86_register::eflags:
            case x86_register::rflags:
                return HV_X86_RFLAGS;
            case x86_register::r8b:
            case x86_register::r8w:
            case x86_register::r8d:
            case x86_register::r8:
                return HV_X86_R8;
            case x86_register::r9b:
            case x86_register::r9w:
            case x86_register::r9d:
            case x86_register::r9:
                return HV_X86_R9;
            case x86_register::r10b:
            case x86_register::r10w:
            case x86_register::r10d:
            case x86_register::r10:
                return HV_X86_R10;
            case x86_register::r11b:
            case x86_register::r11w:
            case x86_register::r11d:
            case x86_register::r11:
                return HV_X86_R11;
            case x86_register::r12b:
            case x86_register::r12w:
            case x86_register::r12d:
            case x86_register::r12:
                return HV_X86_R12;
            case x86_register::r13b:
            case x86_register::r13w:
            case x86_register::r13d:
            case x86_register::r13:
                return HV_X86_R13;
            case x86_register::r14b:
            case x86_register::r14w:
            case x86_register::r14d:
            case x86_register::r14:
                return HV_X86_R14;
            case x86_register::r15b:
            case x86_register::r15w:
            case x86_register::r15d:
            case x86_register::r15:
                return HV_X86_R15;
            default:
                return std::nullopt;
            }
        }

        register_mapping map_register(const x86_register reg)
        {
            if (const auto general = general_register(reg))
            {
                return {.kind = register_kind::general, .id = static_cast<uint32_t>(*general)};
            }
            if (reg >= x86_register::xmm0 && reg <= x86_register::xmm15)
            {
                return {.kind = register_kind::xmm, .id = static_cast<uint32_t>(reg) - static_cast<uint32_t>(x86_register::xmm0)};
            }
            if (reg >= x86_register::st0 && reg <= x86_register::st7)
            {
                return {.kind = register_kind::x87, .id = static_cast<uint32_t>(reg) - static_cast<uint32_t>(x86_register::st0)};
            }

            switch (reg)
            {
            case x86_register::es:
                return {.kind = register_kind::segment_selector, .id = static_cast<uint32_t>(segment::es)};
            case x86_register::cs:
                return {.kind = register_kind::segment_selector, .id = static_cast<uint32_t>(segment::cs)};
            case x86_register::ss:
                return {.kind = register_kind::segment_selector, .id = static_cast<uint32_t>(segment::ss)};
            case x86_register::ds:
                return {.kind = register_kind::segment_selector, .id = static_cast<uint32_t>(segment::ds)};
            case x86_register::fs:
                return {.kind = register_kind::segment_selector, .id = static_cast<uint32_t>(segment::fs)};
            case x86_register::gs:
                return {.kind = register_kind::segment_selector, .id = static_cast<uint32_t>(segment::gs)};
            case x86_register::fs_base:
                return {.kind = register_kind::segment_base, .id = static_cast<uint32_t>(segment::fs)};
            case x86_register::gs_base:
                return {.kind = register_kind::segment_base, .id = static_cast<uint32_t>(segment::gs)};
            case x86_register::cr0:
                return {.kind = register_kind::control, .id = 0};
            case x86_register::cr2:
                return {.kind = register_kind::control, .id = 2};
            case x86_register::cr3:
                return {.kind = register_kind::control, .id = 3};
            case x86_register::cr4:
                return {.kind = register_kind::control, .id = 4};
            case x86_register::dr0:
                return {.kind = register_kind::debug, .id = HV_X86_DR0};
            case x86_register::dr1:
                return {.kind = register_kind::debug, .id = HV_X86_DR1};
            case x86_register::dr2:
                return {.kind = register_kind::debug, .id = HV_X86_DR2};
            case x86_register::dr3:
                return {.kind = register_kind::debug, .id = HV_X86_DR3};
            case x86_register::dr6:
                return {.kind = register_kind::debug, .id = HV_X86_DR6};
            case x86_register::dr7:
                return {.kind = register_kind::debug, .id = HV_X86_DR7};
            case x86_register::gdtr:
                return {.kind = register_kind::table, .id = 0};
            case x86_register::idtr:
                return {.kind = register_kind::table, .id = 1};
            case x86_register::fpcw:
                return {.kind = register_kind::x87_control, .id = fxsave_fcw};
            case x86_register::fpsw:
                return {.kind = register_kind::x87_control, .id = fxsave_fsw};
            case x86_register::fptag:
                return {.kind = register_kind::x87_control, .id = fxsave_ftw};
            case x86_register::mxcsr:
                return {.kind = register_kind::mxcsr};
            case x86_register::msr:
                return {.kind = register_kind::efer};
            default:
                throw std::invalid_argument("Unsupported HVF x86-64 register: " + std::to_string(static_cast<int>(reg)));
            }
        }

        struct gp_access
        {
            size_t offset{};
            size_t width{sizeof(uint64_t)};
            bool zero_extend{};
        };

        gp_access classify_general_access(const x86_register reg)
        {
            switch (reg)
            {
            case x86_register::ah:
            case x86_register::bh:
            case x86_register::ch:
            case x86_register::dh:
                return {.offset = 1, .width = 1};
            case x86_register::al:
            case x86_register::bl:
            case x86_register::cl:
            case x86_register::dl:
            case x86_register::sil:
            case x86_register::dil:
            case x86_register::bpl:
            case x86_register::spl:
            case x86_register::r8b:
            case x86_register::r9b:
            case x86_register::r10b:
            case x86_register::r11b:
            case x86_register::r12b:
            case x86_register::r13b:
            case x86_register::r14b:
            case x86_register::r15b:
                return {.width = 1};
            case x86_register::ax:
            case x86_register::bx:
            case x86_register::cx:
            case x86_register::dx:
            case x86_register::si:
            case x86_register::di:
            case x86_register::bp:
            case x86_register::sp:
            case x86_register::ip:
            case x86_register::flags:
            case x86_register::r8w:
            case x86_register::r9w:
            case x86_register::r10w:
            case x86_register::r11w:
            case x86_register::r12w:
            case x86_register::r13w:
            case x86_register::r14w:
            case x86_register::r15w:
                return {.width = 2};
            case x86_register::eax:
            case x86_register::ebx:
            case x86_register::ecx:
            case x86_register::edx:
            case x86_register::esi:
            case x86_register::edi:
            case x86_register::ebp:
            case x86_register::esp:
            case x86_register::eip:
            case x86_register::eflags:
            case x86_register::r8d:
            case x86_register::r9d:
            case x86_register::r10d:
            case x86_register::r11d:
            case x86_register::r12d:
            case x86_register::r13d:
            case x86_register::r14d:
            case x86_register::r15d:
                return {.width = 4, .zero_extend = true};
            default:
                return {};
            }
        }

        // Cached segment state the CPU would load for a user selector. Bases are kept by the caller: Windows sets
        // FS and GS bases on their own, and every other user segment is flat.
        struct segment_descriptor
        {
            uint64_t limit{};
            uint64_t access_rights{};
        };

        segment_descriptor user_segment(const segment seg, const uint16_t selector)
        {
            if (seg == segment::cs)
            {
                return {.limit = 0xFFFFFFFF, .access_rights = (selector | 3) == user_code64_selector ? ar_code64 : ar_code32};
            }
            if ((selector & ~3u) == 0 && seg != segment::ss)
            {
                return {.limit = 0, .access_rights = ar_unusable};
            }
            return {.limit = 0xFFFFFFFF, .access_rights = ar_data};
        }

        struct segment_state
        {
            uint64_t selector{};
            uint64_t base{};
            uint64_t limit{};
            uint64_t access_rights{};
        };

        struct register_snapshot
        {
            std::array<uint64_t, HV_X86_R15 + 1> general{};
            std::array<segment_state, segment_count> segments{};
            uint64_t gdtr_base{};
            uint64_t gdtr_limit{};
            uint64_t idtr_base{};
            uint64_t idtr_limit{};
            uint64_t cr0{};
            uint64_t cr2{};
            uint64_t cr3{};
            uint64_t cr4{};
            uint64_t efer{};
            std::array<uint64_t, 6> debug{};
            uint64_t xcr0{};
            alignas(64) std::array<std::byte, fpstate_size> fpstate{};
        };

        constexpr std::array<hv_x86_reg_t, 6> debug_registers{HV_X86_DR0, HV_X86_DR1, HV_X86_DR2, HV_X86_DR3, HV_X86_DR6, HV_X86_DR7};

        [[noreturn]] emulator_hook* unsupported(const char* operation)
        {
            throw std::runtime_error(std::string("HVF x86-64 does not support ") + operation);
        }

        __attribute__((target("xsave"))) uint64_t host_xcr0()
        {
            uint32_t eax{};
            uint32_t ebx{};
            uint32_t ecx{};
            uint32_t edx{};
            __cpuid(1, eax, ebx, ecx, edx);
            if ((ecx & bit_OSXSAVE) == 0)
            {
                return 0;
            }
            return _xgetbv(0);
        }

        class hvf_x86_64_emulator final : public x86_64_emulator
        {
          public:
            hvf_x86_64_emulator()
                : owner_(pthread_self())
            {
                table_pool_ = std::make_unique<host_allocation>(table_pool_size);
                check_hv(hv_vm_map(table_pool_->memory, table_physical_base, table_pool_size, HV_MEMORY_READ | HV_MEMORY_WRITE),
                         "hv_vm_map(translation tables)");
                translation_tables_.emplace(std::pair{4u, uint64_t{0}}, allocate_table());

                check_hv(hv_vcpu_create(&vcpu_, HV_VCPU_DEFAULT), "hv_vcpu_create");
                vcpu_created_ = true;
                initialize_controls();
                initialize_guest_state();
            }

            ~hvf_x86_64_emulator() override
            {
                // Hook callbacks may own objects that delete other hooks when destroyed; release every
                // container before any callback is destroyed.
                destroying_ = true;
                utils::reset_object_with_delayed_destruction(instruction_hooks_);
                utils::reset_object_with_delayed_destruction(execution_hooks_);
                utils::reset_object_with_delayed_destruction(violation_hooks_);
                utils::reset_object_with_delayed_destruction(interrupt_hooks_);
                utils::reset_object_with_delayed_destruction(access_hooks_);
                utils::reset_object_with_delayed_destruction(mmio_regions_);
                if (vcpu_created_)
                {
                    if (!pthread_equal(pthread_self(), owner_))
                    {
                        std::fprintf(stderr, "HVF x86-64 emulator destroyed off its vCPU thread; the vCPU is released with the VM\n");
                    }
                    else
                    {
                        log_cleanup_failure(hv_vcpu_destroy(vcpu_), "hv_vcpu_destroy");
                    }
                }
            }

            hvf_x86_64_emulator(const hvf_x86_64_emulator&) = delete;
            hvf_x86_64_emulator& operator=(const hvf_x86_64_emulator&) = delete;
            hvf_x86_64_emulator(hvf_x86_64_emulator&&) = delete;
            hvf_x86_64_emulator& operator=(hvf_x86_64_emulator&&) = delete;

            std::string get_name() const override
            {
                return "hvf-x86-64";
            }

            bool supports_multiple_vcpus() const override
            {
                return false;
            }

            bool supports_instruction_counting() const override
            {
                return true;
            }

            bool supports_global_memory_execution_hooks() const override
            {
                return false;
            }

            bool is_stop_thread_safe() const override
            {
                return true;
            }

            bool has_violation() const override
            {
                return violation_;
            }

            void start(const size_t count) override
            {
                require_owner_thread();
                {
                    std::lock_guard lock(run_mutex_);
                    if (running_)
                    {
                        throw std::logic_error("HVF x86-64 CPU is already running");
                    }
                    running_ = true;
                    stop_requested_ = false;
                    violation_ = false;
                }
                const auto finish = utils::finally([this] {
                    std::lock_guard lock(run_mutex_);
                    in_hypervisor_ = false;
                    running_ = false;
                });

                // A hooked instruction whose hooks already ran is stepped once with its page executable. If another
                // thread (TEB in the GS base) or RIP resumes first, protect the page again.
                if (stepped_page_ &&
                    (read_general(HV_X86_RIP) != stepped_context_.first || read_vmcs(base_field(segment::gs)) != stepped_context_.second))
                {
                    finish_hooked_step();
                }

                auto remaining = count;
                for (;;)
                {
                    refresh_mmio();
                    {
                        std::lock_guard lock(run_mutex_);
                        if (stop_requested_)
                        {
                            return;
                        }
                        prepare_entry(count != 0);
                        in_hypervisor_ = true;
                    }
                    const auto result = hv_vcpu_run(vcpu_);
                    {
                        std::lock_guard lock(run_mutex_);
                        in_hypervisor_ = false;
                    }
                    check_hv(result, "hv_vcpu_run");

                    completed_instruction_ = true;
                    handle_exit();
                    if (count != 0 && completed_instruction_ && --remaining == 0)
                    {
                        return;
                    }
                }
            }

            void stop() override
            {
                std::lock_guard lock(run_mutex_);
                stop_requested_ = true;
                if (in_hypervisor_)
                {
                    check_hv(hv_vcpu_interrupt(&vcpu_, 1), "hv_vcpu_interrupt");
                }
            }

            size_t read_raw_register(const int reg, void* value, const size_t size) override
            {
                const auto x86_reg = static_cast<x86_register>(reg);
                const auto mapping = map_register(x86_reg);
                std::array<std::byte, 16> buffer{};
                size_t width = sizeof(uint64_t);

                switch (mapping.kind)
                {
                case register_kind::general: {
                    const auto access = classify_general_access(x86_reg);
                    const auto full = read_general(static_cast<hv_x86_reg_t>(mapping.id)) >> (access.offset * 8);
                    width = access.width;
                    std::memcpy(buffer.data(), &full, sizeof(full));
                    break;
                }
                case register_kind::segment_selector: {
                    const auto selector = read_vmcs(selector_field(static_cast<segment>(mapping.id)));
                    width = sizeof(uint16_t);
                    std::memcpy(buffer.data(), &selector, sizeof(selector));
                    break;
                }
                case register_kind::segment_base: {
                    const auto base = read_vmcs(base_field(static_cast<segment>(mapping.id)));
                    std::memcpy(buffer.data(), &base, sizeof(base));
                    break;
                }
                case register_kind::control: {
                    const auto control = read_control(mapping.id);
                    std::memcpy(buffer.data(), &control, sizeof(control));
                    break;
                }
                case register_kind::debug: {
                    const auto debug = read_general(static_cast<hv_x86_reg_t>(mapping.id));
                    std::memcpy(buffer.data(), &debug, sizeof(debug));
                    break;
                }
                case register_kind::table: {
                    // Matches descriptor_table_register's in-memory layout: 64-bit base, then the limit.
                    const auto base = read_vmcs(mapping.id == 0 ? VMCS_GUEST_GDTR_BASE : VMCS_GUEST_IDTR_BASE);
                    const auto limit = static_cast<uint32_t>(read_vmcs(mapping.id == 0 ? VMCS_GUEST_GDTR_LIMIT : VMCS_GUEST_IDTR_LIMIT));
                    width = sizeof(uint64_t) + sizeof(uint32_t);
                    std::memcpy(buffer.data(), &base, sizeof(base));
                    std::memcpy(buffer.data() + sizeof(base), &limit, sizeof(limit));
                    break;
                }
                case register_kind::x87:
                case register_kind::xmm: {
                    const auto state = read_fpstate();
                    const auto offset = (mapping.kind == register_kind::x87 ? fxsave_st0 : fxsave_xmm0) + 16 * mapping.id;
                    width = 16;
                    std::memcpy(buffer.data(), state.data() + offset, width);
                    break;
                }
                case register_kind::x87_control: {
                    const auto state = read_fpstate();
                    width = sizeof(uint16_t);
                    if (mapping.id == fxsave_ftw)
                    {
                        // The FXSAVE image holds the abridged tag byte, as every other backend reports it.
                        const auto tag = static_cast<uint16_t>(state[fxsave_ftw]);
                        std::memcpy(buffer.data(), &tag, sizeof(tag));
                    }
                    else
                    {
                        std::memcpy(buffer.data(), state.data() + mapping.id, width);
                    }
                    break;
                }
                case register_kind::mxcsr: {
                    const auto state = read_fpstate();
                    width = sizeof(uint32_t);
                    std::memcpy(buffer.data(), state.data() + fxsave_mxcsr, width);
                    break;
                }
                case register_kind::efer: {
                    const auto efer = read_vmcs(VMCS_GUEST_IA32_EFER);
                    std::memcpy(buffer.data(), &efer, sizeof(efer));
                    break;
                }
                }

                std::memcpy(value, buffer.data(), std::min(size, width));
                return size;
            }

            size_t write_raw_register(const int reg, const void* value, const size_t size) override
            {
                const auto x86_reg = static_cast<x86_register>(reg);
                const auto mapping = map_register(x86_reg);
                std::array<std::byte, 16> buffer{};
                std::memcpy(buffer.data(), value, std::min(size, buffer.size()));
                uint64_t incoming{};
                std::memcpy(&incoming, buffer.data(), sizeof(incoming));

                switch (mapping.kind)
                {
                case register_kind::general: {
                    const auto id = static_cast<hv_x86_reg_t>(mapping.id);
                    const auto access = classify_general_access(x86_reg);
                    auto current = read_general(id);
                    if (access.zero_extend)
                    {
                        current = static_cast<uint32_t>(incoming);
                    }
                    else if (access.width == sizeof(uint64_t))
                    {
                        current = incoming;
                    }
                    else
                    {
                        const auto mask = ((1ull << (access.width * 8)) - 1) << (access.offset * 8);
                        current = (current & ~mask) | ((incoming << (access.offset * 8)) & mask);
                    }
                    if (id == HV_X86_RFLAGS)
                    {
                        current |= rflags_reserved;
                    }
                    write_general(id, current);
                    break;
                }
                case register_kind::segment_selector:
                    load_user_segment(static_cast<segment>(mapping.id), static_cast<uint16_t>(incoming));
                    break;
                case register_kind::segment_base:
                    write_vmcs(base_field(static_cast<segment>(mapping.id)), incoming);
                    break;
                case register_kind::control:
                    write_control(mapping.id, incoming);
                    break;
                case register_kind::debug:
                    write_general(static_cast<hv_x86_reg_t>(mapping.id), incoming);
                    break;
                case register_kind::table: {
                    uint32_t limit{};
                    std::memcpy(&limit, buffer.data() + sizeof(uint64_t), sizeof(limit));
                    write_vmcs(mapping.id == 0 ? VMCS_GUEST_GDTR_BASE : VMCS_GUEST_IDTR_BASE, incoming);
                    write_vmcs(mapping.id == 0 ? VMCS_GUEST_GDTR_LIMIT : VMCS_GUEST_IDTR_LIMIT, limit);
                    break;
                }
                case register_kind::x87:
                case register_kind::xmm: {
                    auto state = read_fpstate();
                    const auto offset = (mapping.kind == register_kind::x87 ? fxsave_st0 : fxsave_xmm0) + 16 * mapping.id;
                    std::memcpy(state.data() + offset, buffer.data(), 16);
                    write_fpstate(state);
                    break;
                }
                case register_kind::x87_control: {
                    auto state = read_fpstate();
                    if (mapping.id == fxsave_ftw)
                    {
                        state[fxsave_ftw] = buffer[0];
                    }
                    else
                    {
                        std::memcpy(state.data() + mapping.id, buffer.data(), sizeof(uint16_t));
                    }
                    write_fpstate(state);
                    break;
                }
                case register_kind::mxcsr: {
                    auto state = read_fpstate();
                    std::memcpy(state.data() + fxsave_mxcsr, buffer.data(), sizeof(uint32_t));
                    write_fpstate(state);
                    break;
                }
                case register_kind::efer:
                    write_efer(incoming);
                    break;
                }
                return size;
            }

            std::vector<std::byte> save_registers() const override
            {
                register_snapshot snapshot{};
                for (uint32_t reg = 0; reg < snapshot.general.size(); ++reg)
                {
                    snapshot.general[reg] = read_general(static_cast<hv_x86_reg_t>(reg));
                }
                for (size_t index = 0; index < segment_count; ++index)
                {
                    const auto seg = static_cast<segment>(index);
                    snapshot.segments[index] = {.selector = read_vmcs(selector_field(seg)),
                                                .base = read_vmcs(base_field(seg)),
                                                .limit = read_vmcs(limit_field(seg)),
                                                .access_rights = read_vmcs(access_rights_field(seg))};
                }
                snapshot.gdtr_base = read_vmcs(VMCS_GUEST_GDTR_BASE);
                snapshot.gdtr_limit = read_vmcs(VMCS_GUEST_GDTR_LIMIT);
                snapshot.idtr_base = read_vmcs(VMCS_GUEST_IDTR_BASE);
                snapshot.idtr_limit = read_vmcs(VMCS_GUEST_IDTR_LIMIT);
                snapshot.cr0 = read_control(0);
                snapshot.cr2 = read_control(2);
                snapshot.cr3 = read_control(3);
                snapshot.cr4 = read_control(4);
                snapshot.efer = read_vmcs(VMCS_GUEST_IA32_EFER);
                for (size_t index = 0; index < debug_registers.size(); ++index)
                {
                    snapshot.debug[index] = read_general(debug_registers[index]);
                }
                snapshot.xcr0 = read_general(HV_X86_XCR0);
                snapshot.fpstate = read_fpstate();

                std::vector<std::byte> bytes(sizeof(snapshot));
                std::memcpy(bytes.data(), &snapshot, sizeof(snapshot));
                return bytes;
            }

            void restore_registers(const std::vector<std::byte>& bytes) override
            {
                register_snapshot snapshot{};
                if (bytes.size() != sizeof(snapshot))
                {
                    throw std::invalid_argument("Invalid HVF x86-64 saved register size");
                }
                std::memcpy(&snapshot, bytes.data(), sizeof(snapshot));

                for (uint32_t reg = 0; reg < snapshot.general.size(); ++reg)
                {
                    write_general(static_cast<hv_x86_reg_t>(reg), snapshot.general[reg]);
                }
                for (size_t index = 0; index < segment_count; ++index)
                {
                    const auto seg = static_cast<segment>(index);
                    const auto& state = snapshot.segments[index];
                    write_vmcs(selector_field(seg), state.selector);
                    write_vmcs(base_field(seg), state.base);
                    write_vmcs(limit_field(seg), state.limit);
                    write_vmcs(access_rights_field(seg), state.access_rights);
                }
                write_vmcs(VMCS_GUEST_GDTR_BASE, snapshot.gdtr_base);
                write_vmcs(VMCS_GUEST_GDTR_LIMIT, snapshot.gdtr_limit);
                write_vmcs(VMCS_GUEST_IDTR_BASE, snapshot.idtr_base);
                write_vmcs(VMCS_GUEST_IDTR_LIMIT, snapshot.idtr_limit);
                write_control(0, snapshot.cr0);
                write_control(2, snapshot.cr2);
                write_control(3, snapshot.cr3);
                write_control(4, snapshot.cr4);
                write_efer(snapshot.efer);
                for (size_t index = 0; index < debug_registers.size(); ++index)
                {
                    write_general(debug_registers[index], snapshot.debug[index]);
                }
                write_general(HV_X86_XCR0, snapshot.xcr0);
                write_fpstate(snapshot.fpstate);
            }

            void serialize_state(utils::buffer_serializer& buffer, bool) const override
            {
                buffer.write_vector(save_registers());
            }

            void deserialize_state(utils::buffer_deserializer& buffer, bool) override
            {
                restore_registers(buffer.read_vector<std::byte>());
            }

            bool read_descriptor_table(const int reg, descriptor_table_register& table) override
            {
                if (reg != static_cast<int>(x86_register::gdtr) && reg != static_cast<int>(x86_register::idtr))
                {
                    return false;
                }
                const auto gdt = reg == static_cast<int>(x86_register::gdtr);
                table.base = read_vmcs(gdt ? VMCS_GUEST_GDTR_BASE : VMCS_GUEST_IDTR_BASE);
                table.limit = static_cast<uint32_t>(read_vmcs(gdt ? VMCS_GUEST_GDTR_LIMIT : VMCS_GUEST_IDTR_LIMIT));
                return true;
            }

            void set_segment_base(const x86_register base, const pointer_type value) override
            {
                write_vmcs(base_field(segment_of(base)), value);
            }

            pointer_type get_segment_base(const x86_register base) override
            {
                return read_vmcs(base_field(segment_of(base)));
            }

            void load_gdt(const pointer_type address, const uint32_t limit) override
            {
                write_vmcs(VMCS_GUEST_GDTR_BASE, address);
                write_vmcs(VMCS_GUEST_GDTR_LIMIT, limit);
            }

            bool try_read_memory(const uint64_t address, void* data, const size_t size) const override
            {
                return access_memory(address, data, size, false);
            }

            void read_memory(const uint64_t address, void* data, const size_t size) const override
            {
                if (!try_read_memory(address, data, size))
                {
                    throw std::runtime_error("Reading unmapped HVF x86-64 guest memory at " + std::to_string(address));
                }
            }

            bool try_write_memory(const uint64_t address, const void* data, const size_t size) override
            {
                return access_memory(address, const_cast<void*>(data), size, true);
            }

            void write_memory(const uint64_t address, const void* data, const size_t size) override
            {
                if (!try_write_memory(address, data, size))
                {
                    throw std::runtime_error("Writing unmapped HVF x86-64 guest memory at " + std::to_string(address));
                }
            }

            emulator_hook* hook_instruction(const int instruction, instruction_hook_callback callback) override
            {
                if (!callback)
                {
                    throw std::invalid_argument("HVF x86-64 requires a nonempty instruction hook");
                }
                auto& hook = instruction_hooks_.emplace_back(std::make_unique<instruction_hook>(
                    instruction_hook{.type = static_cast<x86_hookable_instructions>(instruction), .callback = std::move(callback)}));
                return reinterpret_cast<emulator_hook*>(hook.get());
            }

            void delete_hook(emulator_hook* hook) override
            {
                if (destroying_)
                {
                    return;
                }
                const auto erase = [&](auto& container) {
                    const auto entry = std::ranges::find_if(
                        container, [&](const auto& candidate) { return reinterpret_cast<emulator_hook*>(candidate.get()) == hook; });
                    if (entry == container.end())
                    {
                        return false;
                    }
                    container.erase(entry);
                    return true;
                };
                if (erase(instruction_hooks_) || erase(access_hooks_) || erase(violation_hooks_) || erase(interrupt_hooks_))
                {
                    return;
                }
                const auto execution = std::ranges::find_if(execution_hooks_, [&](const std::unique_ptr<execution_hook>& entry) {
                    return reinterpret_cast<emulator_hook*>(entry.get()) == hook;
                });
                if (execution == execution_hooks_.end())
                {
                    throw std::invalid_argument("Unknown HVF x86-64 hook");
                }
                const auto address = (*execution)->address;
                const auto size = (*execution)->size;
                execution_hooks_.erase(execution);
                refresh_permissions(address, size);
            }

            emulator_hook* hook_memory_execution(memory_execution_hook_callback) override
            {
                return unsupported("memory execution hooks");
            }

            emulator_hook* hook_memory_execution(const uint64_t address, memory_execution_hook_callback callback) override
            {
                return hook_memory_range_execution(address, 1, std::move(callback));
            }

            // Hooked pages lose execute permission in the EPT. The violation runs the hooks, and the instruction then
            // executes once under the monitor trap flag before the page is protected again.
            emulator_hook* hook_memory_range_execution(const uint64_t address, const uint64_t size,
                                                       memory_execution_hook_callback callback) override
            {
                if (!size || !callback || address > UINT64_MAX - size)
                {
                    throw std::invalid_argument("HVF x86-64 execution hooks need a nonempty range and callback");
                }
                auto& hook = execution_hooks_.emplace_back(
                    std::make_unique<execution_hook>(execution_hook{.address = address, .size = size, .callback = std::move(callback)}));
                refresh_permissions(address, size);
                return reinterpret_cast<emulator_hook*>(hook.get());
            }

            // As on WHP and HVF ARM64, data access hooks are accepted but not reported.
            emulator_hook* hook_memory_read(uint64_t, uint64_t, memory_access_hook_callback callback) override
            {
                return add_access_hook(std::move(callback));
            }

            emulator_hook* hook_memory_write(uint64_t, uint64_t, memory_access_hook_callback callback) override
            {
                return add_access_hook(std::move(callback));
            }

            emulator_hook* hook_interrupt(interrupt_hook_callback callback) override
            {
                if (!callback)
                {
                    throw std::invalid_argument("HVF x86-64 requires a nonempty interrupt hook");
                }
                auto& hook = interrupt_hooks_.emplace_back(std::make_unique<interrupt_hook>(interrupt_hook{std::move(callback)}));
                return reinterpret_cast<emulator_hook*>(hook.get());
            }

            emulator_hook* hook_memory_violation(memory_violation_hook_callback callback) override
            {
                if (!callback)
                {
                    throw std::invalid_argument("HVF x86-64 requires a nonempty memory violation hook");
                }
                auto& hook = violation_hooks_.emplace_back(std::make_unique<violation_hook>(violation_hook{std::move(callback)}));
                return reinterpret_cast<emulator_hook*>(hook.get());
            }

            emulator_hook* hook_basic_block(basic_block_hook_callback) override
            {
                return unsupported("basic block hooks");
            }

          private:
            struct instruction_hook
            {
                x86_hookable_instructions type{};
                instruction_hook_callback callback;
            };

            struct interrupt_hook
            {
                interrupt_hook_callback callback;
            };

            struct violation_hook
            {
                memory_violation_hook_callback callback;
            };

            struct execution_hook
            {
                uint64_t address{};
                uint64_t size{};
                memory_execution_hook_callback callback;
            };

            // Host memory outlives the VM (declared first, destroyed last); the vCPU is destroyed in the
            // destructor body, before the VM.
            std::vector<std::unique_ptr<host_allocation>> pool_chunks_{};
            std::unique_ptr<host_allocation> table_pool_{};
            vm_handle vm_{};
            pthread_t owner_{};
            hv_vcpuid_t vcpu_{};
            bool vcpu_created_{};
            uint64_t cr0_fixed0_{};
            uint64_t cr0_fixed1_{};
            uint64_t cr4_fixed0_{};
            uint64_t cr4_fixed1_{};
            uint64_t primary_controls_{};
            uint64_t applied_primary_controls_{};
            std::map<uint64_t, mapped_page> pages_{};
            std::map<std::pair<unsigned, uint64_t>, translation_table> translation_tables_{};
            uint64_t next_table_offset_{};
            std::vector<uint64_t> free_table_offsets_{};
            uint64_t translation_generation_{};
            std::optional<uint64_t> flushed_generation_{};
            uint64_t next_gpa_{guest_physical_base};
            std::vector<uint64_t> free_gpas_{};
            std::vector<std::byte*> free_hosts_{};
            std::vector<std::byte*> dirty_hosts_{};
            std::map<uint64_t, std::shared_ptr<mmio_region>> mmio_regions_{};
            std::vector<std::unique_ptr<instruction_hook>> instruction_hooks_{};
            std::vector<std::unique_ptr<interrupt_hook>> interrupt_hooks_{};
            std::vector<std::unique_ptr<violation_hook>> violation_hooks_{};
            std::vector<std::unique_ptr<execution_hook>> execution_hooks_{};
            std::vector<std::unique_ptr<memory_access_hook_callback>> access_hooks_{};
            std::optional<uint64_t> stepped_page_{};
            std::pair<uint64_t, uint64_t> stepped_context_{};
            bool completed_instruction_{};
            bool destroying_{};
            std::mutex run_mutex_{};
            bool running_{};
            bool in_hypervisor_{};
            bool stop_requested_{};
            bool violation_{};

            // Hypervisor.framework binds a vCPU to the thread that created it.
            void require_owner_thread() const
            {
                if (!pthread_equal(pthread_self(), owner_))
                {
                    throw std::logic_error("HVF x86-64 vCPUs can only be used from the thread that created the emulator");
                }
            }

            uint64_t read_general(const hv_x86_reg_t reg) const
            {
                require_owner_thread();
                uint64_t value{};
                check_hv(hv_vcpu_read_register(vcpu_, reg, &value), "hv_vcpu_read_register");
                return value;
            }

            void write_general(const hv_x86_reg_t reg, const uint64_t value) const
            {
                require_owner_thread();
                check_hv(hv_vcpu_write_register(vcpu_, reg, value), "hv_vcpu_write_register");
            }

            uint64_t read_vmcs(const uint32_t field) const
            {
                require_owner_thread();
                uint64_t value{};
                check_hv(hv_vmx_vcpu_read_vmcs(vcpu_, field, &value), "hv_vmx_vcpu_read_vmcs");
                return value;
            }

            void write_vmcs(const uint32_t field, const uint64_t value) const
            {
                require_owner_thread();
                check_hv(hv_vmx_vcpu_write_vmcs(vcpu_, field, value), "hv_vmx_vcpu_write_vmcs");
            }

            std::array<std::byte, fpstate_size> read_fpstate() const
            {
                require_owner_thread();
                alignas(64) std::array<std::byte, fpstate_size> state{};
                check_hv(hv_vcpu_read_fpstate(vcpu_, state.data(), state.size()), "hv_vcpu_read_fpstate");
                return state;
            }

            void write_fpstate(std::array<std::byte, fpstate_size> state) const
            {
                require_owner_thread();
                check_hv(hv_vcpu_write_fpstate(vcpu_, state.data(), state.size()), "hv_vcpu_write_fpstate");
            }

            static segment segment_of(const x86_register reg)
            {
                switch (reg)
                {
                case x86_register::es:
                    return segment::es;
                case x86_register::cs:
                    return segment::cs;
                case x86_register::ss:
                    return segment::ss;
                case x86_register::ds:
                    return segment::ds;
                case x86_register::fs:
                case x86_register::fs_base:
                    return segment::fs;
                case x86_register::gs:
                case x86_register::gs_base:
                    return segment::gs;
                default:
                    throw std::invalid_argument("Unsupported HVF x86-64 segment register");
                }
            }

            void load_user_segment(const segment seg, const uint16_t selector) const
            {
                const auto descriptor = user_segment(seg, selector);
                write_vmcs(selector_field(seg), selector);
                write_vmcs(limit_field(seg), descriptor.limit);
                write_vmcs(access_rights_field(seg), descriptor.access_rights);
                if (seg != segment::fs && seg != segment::gs)
                {
                    write_vmcs(base_field(seg), 0);
                }
            }

            // The guest sees the CR0 and CR4 values it last wrote; VMX operation forces some bits (CR4.VMXE, for
            // example) that every guest read is shadowed from.
            uint64_t read_control(const uint32_t index) const
            {
                switch (index)
                {
                case 0:
                    return read_vmcs(VMCS_CTRL_CR0_SHADOW);
                case 2:
                    return read_general(HV_X86_CR2);
                case 3:
                    return read_vmcs(VMCS_GUEST_CR3);
                case 4:
                    return read_vmcs(VMCS_CTRL_CR4_SHADOW);
                default:
                    throw std::invalid_argument("Unsupported HVF x86-64 control register");
                }
            }

            void write_control(const uint32_t index, const uint64_t value) const
            {
                switch (index)
                {
                case 0:
                    write_vmcs(VMCS_CTRL_CR0_SHADOW, value);
                    write_vmcs(VMCS_GUEST_CR0, (value | cr0_fixed0_) & cr0_fixed1_);
                    break;
                case 2:
                    write_general(HV_X86_CR2, value);
                    break;
                case 3:
                    write_vmcs(VMCS_GUEST_CR3, value);
                    break;
                case 4:
                    write_vmcs(VMCS_CTRL_CR4_SHADOW, value);
                    write_vmcs(VMCS_GUEST_CR4, (value | cr4_fixed0_) & cr4_fixed1_);
                    break;
                default:
                    throw std::invalid_argument("Unsupported HVF x86-64 control register");
                }
            }

            void write_efer(const uint64_t efer) const
            {
                write_vmcs(VMCS_GUEST_IA32_EFER, efer);
                auto entry = read_vmcs(VMCS_CTRL_VMENTRY_CONTROLS);
                entry = (efer & efer_lma) != 0 ? entry | VMENTRY_GUEST_IA32E : entry & ~static_cast<uint64_t>(VMENTRY_GUEST_IA32E);
                write_vmcs(VMCS_CTRL_VMENTRY_CONTROLS, entry);
            }

            void initialize_controls()
            {
                cr0_fixed0_ = read_capability(HV_VMX_CAP_CR0_FIXED0);
                cr0_fixed1_ = read_capability(HV_VMX_CAP_CR0_FIXED1);
                cr4_fixed0_ = read_capability(HV_VMX_CAP_CR4_FIXED0);
                cr4_fixed1_ = read_capability(HV_VMX_CAP_CR4_FIXED1);

                write_vmcs(VMCS_CTRL_PIN_BASED, adjust_controls(HV_VMX_CAP_PINBASED, 0));
                primary_controls_ = adjust_controls(HV_VMX_CAP_PROCBASED, CPU_BASED_SECONDARY_CTLS);
                applied_primary_controls_ = primary_controls_;
                write_vmcs(VMCS_CTRL_CPU_BASED, primary_controls_);
                // Without "enable RDTSCP" the instruction raises #UD in the guest.
                write_vmcs(VMCS_CTRL_CPU_BASED2, adjust_controls(HV_VMX_CAP_PROCBASED2, CPU_BASED2_RDTSCP));
                // Hypervisor.framework owns the host state; only the guest's EFER is loaded on entry.
                write_vmcs(VMCS_CTRL_VMENTRY_CONTROLS, adjust_controls(HV_VMX_CAP_ENTRY, VMENTRY_LOAD_EFER));
                write_vmcs(VMCS_CTRL_VMEXIT_CONTROLS, adjust_controls(HV_VMX_CAP_EXIT, 0));
                // The guest runs only user code and has no IDT: every exception exits at its faulting context.
                write_vmcs(VMCS_CTRL_EXC_BITMAP, 0xFFFFFFFF);
                write_vmcs(VMCS_CTRL_PF_ERROR_MASK, 0);
                write_vmcs(VMCS_CTRL_PF_ERROR_MATCH, 0);
                write_vmcs(VMCS_CTRL_CR0_MASK, ~uint64_t{});
                write_vmcs(VMCS_CTRL_CR4_MASK, ~uint64_t{});
            }

            void initialize_guest_state()
            {
                write_control(0, cr0_pe | cr0_mp | cr0_et | cr0_ne | cr0_pg);
                const auto xcr0 = host_xcr0();
                write_control(4, cr4_pae | cr4_osfxsr | cr4_osxmmexcpt | (xcr0 != 0 ? cr4_osxsave : 0));
                write_control(3, translation_tables_.at(table_key(4, 0)).gpa);
                write_efer(efer_lme | efer_lma);
                if (xcr0 != 0)
                {
                    // x87, SSE and AVX state; larger components would need their own enablement in the guest.
                    write_general(HV_X86_XCR0, xcr0 & 0x7);
                }

                load_user_segment(segment::cs, user_code64_selector);
                for (const auto seg : {segment::ss, segment::ds, segment::es, segment::fs, segment::gs})
                {
                    load_user_segment(seg, 0x2B);
                    write_vmcs(base_field(seg), 0);
                }
                write_vmcs(selector_field(segment::ldtr), 0);
                write_vmcs(base_field(segment::ldtr), 0);
                write_vmcs(limit_field(segment::ldtr), 0);
                write_vmcs(access_rights_field(segment::ldtr), ar_unusable);
                // VM entry requires a busy 64-bit TSS, although no privilege change ever consults it.
                write_vmcs(selector_field(segment::tr), 0);
                write_vmcs(base_field(segment::tr), 0);
                write_vmcs(limit_field(segment::tr), 0x67);
                write_vmcs(access_rights_field(segment::tr), ar_busy_tss64);
                write_vmcs(VMCS_GUEST_GDTR_BASE, 0);
                write_vmcs(VMCS_GUEST_GDTR_LIMIT, 0);
                write_vmcs(VMCS_GUEST_IDTR_BASE, 0);
                write_vmcs(VMCS_GUEST_IDTR_LIMIT, 0);

                write_vmcs(VMCS_GUEST_LINK_POINTER, ~uint64_t{});
                write_vmcs(VMCS_GUEST_IA32_DEBUGCTL, 0);
                write_vmcs(VMCS_GUEST_DR7, 0x400);
                write_vmcs(VMCS_GUEST_ACTIVITY_STATE, 0);
                write_vmcs(VMCS_GUEST_INTERRUPTIBILITY, 0);
                write_vmcs(VMCS_GUEST_DEBUG_EXC, 0);
                write_vmcs(VMCS_GUEST_SYSENTER_ESP, 0);
                write_vmcs(VMCS_GUEST_SYSENTER_EIP, 0);
                write_vmcs(VMCS_GUEST_IA32_SYSENTER_CS, 0);

                write_general(HV_X86_RIP, 0);
                write_general(HV_X86_RFLAGS, rflags_reserved);

                auto state = read_fpstate();
                constexpr uint16_t default_fcw = 0x037F;
                constexpr uint32_t default_mxcsr = 0x1F80;
                std::memcpy(state.data() + fxsave_fcw, &default_fcw, sizeof(default_fcw));
                std::memcpy(state.data() + fxsave_mxcsr, &default_mxcsr, sizeof(default_mxcsr));
                write_fpstate(state);
            }

            bool has_instruction_hook(const x86_hookable_instructions type) const
            {
                return std::ranges::any_of(instruction_hooks_, [&](const auto& hook) { return hook->type == type; });
            }

            std::vector<instruction_hook_callback> instruction_callbacks(const x86_hookable_instructions type) const
            {
                std::vector<instruction_hook_callback> callbacks{};
                for (const auto& hook : instruction_hooks_)
                {
                    if (hook->type == type)
                    {
                        callbacks.push_back(hook->callback);
                    }
                }
                return callbacks;
            }

            void prepare_entry(const bool counting)
            {
                auto controls = primary_controls_;
                if (has_instruction_hook(x86_hookable_instructions::rdtsc) || has_instruction_hook(x86_hookable_instructions::rdtscp))
                {
                    controls |= CPU_BASED_RDTSC;
                }
                if (counting || stepped_page_)
                {
                    controls |= CPU_BASED_MTF;
                }
                if (controls != applied_primary_controls_)
                {
                    write_vmcs(VMCS_CTRL_CPU_BASED, controls);
                    applied_primary_controls_ = controls;
                }
                if (flushed_generation_ != translation_generation_)
                {
                    check_hv(hv_vcpu_invalidate_tlb(vcpu_), "hv_vcpu_invalidate_tlb");
                    flushed_generation_ = translation_generation_;
                }
            }

            uint64_t instruction_pointer() const
            {
                return read_general(HV_X86_RIP);
            }

            void advance_rip(const uint64_t amount) const
            {
                write_general(HV_X86_RIP, instruction_pointer() + amount);
            }

            void handle_exit()
            {
                const auto reason = static_cast<uint32_t>(read_vmcs(VMCS_RO_EXIT_REASON) & 0xFFFF);
                if (reason == VMX_REASON_IRQ)
                {
                    // A kick or a host interrupt: the guest instruction did not complete.
                    completed_instruction_ = false;
                    return;
                }
                // The hooked instruction has run (or raised its own exception); protect its page again.
                if (stepped_page_)
                {
                    finish_hooked_step();
                }
                switch (reason)
                {
                case VMX_REASON_MTF:
                    return;
                case VMX_REASON_EXC_NMI:
                    handle_exception();
                    return;
                case VMX_REASON_EPT_VIOLATION:
                    handle_ept_violation();
                    return;
                case VMX_REASON_CPUID:
                    handle_instruction_exit(x86_hookable_instructions::cpuid);
                    return;
                case VMX_REASON_RDTSC:
                    handle_instruction_exit(x86_hookable_instructions::rdtsc);
                    return;
                case vmx_reason_rdtscp:
                    handle_instruction_exit(x86_hookable_instructions::rdtscp);
                    return;
                case VMX_REASON_TRIPLE_FAULT:
                    violation_ = true;
                    throw std::runtime_error("HVF x86-64 guest triple-faulted at RIP 0x" + to_hex(instruction_pointer()));
                case VMX_REASON_VMENTRY_GUEST:
                    violation_ = true;
                    throw std::runtime_error("HVF x86-64 VM entry failed: invalid guest state (qualification 0x" +
                                             to_hex(read_vmcs(VMCS_RO_EXIT_QUALIFIC)) + ")");
                default:
                    violation_ = true;
                    throw std::runtime_error("Unhandled HVF x86-64 exit reason " + std::to_string(reason) + " at RIP 0x" +
                                             to_hex(instruction_pointer()));
                }
            }

            static std::string to_hex(const uint64_t value)
            {
                std::ostringstream stream{};
                stream << std::hex << value;
                return stream.str();
            }

            void apply_default_instruction(const x86_hookable_instructions type) const
            {
                switch (type)
                {
                case x86_hookable_instructions::cpuid: {
                    uint32_t eax{};
                    uint32_t ebx{};
                    uint32_t ecx{};
                    uint32_t edx{};
                    __cpuid_count(static_cast<uint32_t>(read_general(HV_X86_RAX)), static_cast<uint32_t>(read_general(HV_X86_RCX)), eax,
                                  ebx, ecx, edx);
                    write_general(HV_X86_RAX, eax);
                    write_general(HV_X86_RBX, ebx);
                    write_general(HV_X86_RCX, ecx);
                    write_general(HV_X86_RDX, edx);
                    break;
                }
                case x86_hookable_instructions::rdtsc:
                case x86_hookable_instructions::rdtscp: {
                    uint32_t aux{};
                    const auto tsc = type == x86_hookable_instructions::rdtscp ? __rdtscp(&aux) : __rdtsc();
                    write_general(HV_X86_RAX, static_cast<uint32_t>(tsc));
                    write_general(HV_X86_RDX, static_cast<uint32_t>(tsc >> 32));
                    if (type == x86_hookable_instructions::rdtscp)
                    {
                        write_general(HV_X86_RCX, aux);
                    }
                    break;
                }
                default:
                    break;
                }
            }

            void handle_instruction_exit(const x86_hookable_instructions type)
            {
                const auto length = read_vmcs(VMCS_RO_VMEXIT_INSTR_LEN);
                bool skip = false;
                for (const auto& callback : instruction_callbacks(type))
                {
                    if (callback(*this, 0) == instruction_hook_continuation::skip_instruction)
                    {
                        skip = true;
                    }
                }
                if (!skip)
                {
                    apply_default_instruction(type);
                }
                advance_rip(length);
            }

            void report_interrupt(const int vector)
            {
                std::vector<interrupt_hook_callback> callbacks{};
                callbacks.reserve(interrupt_hooks_.size());
                for (const auto& hook : interrupt_hooks_)
                {
                    callbacks.push_back(hook->callback);
                }
                if (callbacks.empty())
                {
                    violation_ = true;
                    throw std::runtime_error("Unhandled HVF x86-64 guest exception " + std::to_string(vector) + " at RIP 0x" +
                                             to_hex(instruction_pointer()));
                }
                for (const auto& callback : callbacks)
                {
                    callback(*this, vector);
                }
            }

            bool report_violation(const uint64_t address, const size_t size, const memory_operation operation,
                                  const memory_violation_type type)
            {
                std::vector<memory_violation_hook_callback> callbacks{};
                callbacks.reserve(violation_hooks_.size());
                for (const auto& hook : violation_hooks_)
                {
                    callbacks.push_back(hook->callback);
                }
                for (const auto& callback : callbacks)
                {
                    const auto result = callback(*this, address, size, operation, type);
                    if (result == memory_violation_continuation::resume || result == memory_violation_continuation::restart)
                    {
                        return true;
                    }
                }
                return false;
            }

            // Hooks see RIP at the SYSCALL instruction; skipping it resumes right after.
            void handle_syscall()
            {
                const auto rip = instruction_pointer();
                const auto callbacks = instruction_callbacks(x86_hookable_instructions::syscall);
                if (callbacks.empty())
                {
                    report_interrupt(invalid_opcode);
                    return;
                }
                const auto continuation = callbacks.front()(*this, 0);
                if (continuation == instruction_hook_continuation::finalized_instruction_pointer)
                {
                    return;
                }
                const auto current = instruction_pointer();
                if (continuation == instruction_hook_continuation::skip_instruction && current == rip)
                {
                    write_general(HV_X86_RIP, rip + syscall_instruction_size);
                }
                else
                {
                    write_general(HV_X86_RIP, current + syscall_instruction_size);
                }
            }

            bool handle_invalid_instruction()
            {
                const auto rip = instruction_pointer();
                bool consumed = false;
                for (const auto& callback : instruction_callbacks(x86_hookable_instructions::invalid))
                {
                    if (callback(*this, 0) == instruction_hook_continuation::skip_instruction)
                    {
                        consumed = true;
                    }
                }
                if (consumed && instruction_pointer() == rip)
                {
                    advance_rip(2);
                }
                return consumed;
            }

            // The guest has no IDT, so INT n faults with #GP while fetching its gate; it reaches the interrupt hook
            // with its own vector instead.
            bool handle_software_interrupt()
            {
                const auto rip = instruction_pointer();
                std::array<uint8_t, 2> opcode{};
                if (!try_read_memory(rip, opcode.data(), opcode.size()) || opcode[0] != 0xCD)
                {
                    return false;
                }
                report_interrupt(opcode[1]);
                if (instruction_pointer() == rip && !stop_requested_)
                {
                    advance_rip(software_interrupt_size);
                }
                return true;
            }

            void handle_exception()
            {
                const auto information = read_vmcs(VMCS_RO_VMEXIT_IRQ_INFO);
                const auto vector = static_cast<int>(information & interruption_vector_mask);
                const auto error_code = (information & interruption_has_error_code) != 0 ? read_vmcs(VMCS_RO_VMEXIT_IRQ_ERROR) : 0;

                switch (vector)
                {
                case invalid_opcode: {
                    std::array<uint8_t, 2> opcode{};
                    const auto readable = try_read_memory(instruction_pointer(), opcode.data(), opcode.size());
                    if (readable && opcode[0] == 0x0F && opcode[1] == 0x05)
                    {
                        handle_syscall();
                        return;
                    }
                    if (readable && opcode[0] == 0x0F && opcode[1] == 0x0B && handle_invalid_instruction())
                    {
                        return;
                    }
                    break;
                }
                case general_protection:
                    if (handle_software_interrupt())
                    {
                        return;
                    }
                    break;
                case breakpoint_exception: {
                    // The exit leaves RIP at the INT3; hooks that do not redirect execution continue after it.
                    const auto rip = instruction_pointer();
                    report_interrupt(breakpoint_exception);
                    if (instruction_pointer() == rip && !stop_requested_)
                    {
                        advance_rip(read_vmcs(VMCS_RO_VMEXIT_INSTR_LEN));
                    }
                    return;
                }
                case page_fault: {
                    // The exit does not load CR2; the faulting address is the exit qualification.
                    const auto address = read_vmcs(VMCS_RO_EXIT_QUALIFIC);
                    write_general(HV_X86_CR2, address);
                    const auto operation = (error_code & (1u << 4)) != 0   ? memory_operation::exec
                                           : (error_code & (1u << 1)) != 0 ? memory_operation::write
                                                                           : memory_operation::read;
                    const auto type = (error_code & 1u) != 0 ? memory_violation_type::protection : memory_violation_type::unmapped;
                    if (report_violation(address, 1, operation, type))
                    {
                        return;
                    }
                    break;
                }
                case x87_floating_point:
                case simd_floating_point: {
                    // Windows keeps floating-point exceptions masked; a guest that unmasked them gets the masked
                    // defaults back and reruns the instruction, as on WHP.
                    auto state = read_fpstate();
                    uint32_t mxcsr{};
                    uint16_t fcw{};
                    uint16_t fsw{};
                    std::memcpy(&mxcsr, state.data() + fxsave_mxcsr, sizeof(mxcsr));
                    std::memcpy(&fcw, state.data() + fxsave_fcw, sizeof(fcw));
                    std::memcpy(&fsw, state.data() + fxsave_fsw, sizeof(fsw));
                    mxcsr = (mxcsr | 0x1F80u) & ~0x3Fu;
                    fcw = static_cast<uint16_t>(fcw | 0x3Fu);
                    fsw = static_cast<uint16_t>(fsw & ~0x80FFu);
                    std::memcpy(state.data() + fxsave_mxcsr, &mxcsr, sizeof(mxcsr));
                    std::memcpy(state.data() + fxsave_fcw, &fcw, sizeof(fcw));
                    std::memcpy(state.data() + fxsave_fsw, &fsw, sizeof(fsw));
                    write_fpstate(state);
                    completed_instruction_ = false;
                    return;
                }
                case divide_error:
                case debug_exception:
                default:
                    break;
                }

                report_interrupt(vector);
            }

            void handle_ept_violation()
            {
                const auto qualification = read_vmcs(VMCS_RO_EXIT_QUALIFIC);
                const auto physical = read_vmcs(VMCS_GUEST_PHYSICAL_ADDRESS);
                const auto rip = instruction_pointer();
                const auto operation = (qualification & ept_access_fetch) != 0   ? memory_operation::exec
                                       : (qualification & ept_access_write) != 0 ? memory_operation::write
                                                                                 : memory_operation::read;

                std::optional<uint64_t> address{};
                if ((qualification & ept_linear_address_valid) != 0)
                {
                    address = read_vmcs(VMCS_RO_GUEST_LIN_ADDR);
                }
                else
                {
                    address = virtual_address_of(physical);
                }

                const auto page_address = address.value_or(0) & ~(page_size - 1);
                const auto page = address ? pages_.find(page_address) : pages_.end();
                if (operation == memory_operation::exec && page != pages_.end() && is_executable(page->second.permissions) &&
                    has_execution_hook(page_address))
                {
                    std::vector<memory_execution_hook_callback> callbacks{};
                    for (const auto& hook : execution_hooks_)
                    {
                        if (rip >= hook->address && rip - hook->address < hook->size)
                        {
                            callbacks.push_back(hook->callback);
                        }
                    }
                    for (const auto& callback : callbacks)
                    {
                        callback(*this, rip);
                    }
                    if (instruction_pointer() == rip && has_execution_hook(page_address))
                    {
                        stepped_page_ = page_address;
                        stepped_context_ = {rip, read_vmcs(base_field(segment::gs))};
                        apply_page_permissions(page_address, page->second);
                    }
                    // No guest instruction completed, so the exit does not consume a step.
                    completed_instruction_ = false;
                    return;
                }

                const auto type = page != pages_.end() ? memory_violation_type::protection : memory_violation_type::unmapped;
                if (report_violation(address.value_or(physical), 1, operation, type))
                {
                    return;
                }

                violation_ = true;
                std::ostringstream error{};
                error << "HVF x86-64 memory violation at 0x" << std::hex << address.value_or(physical) << " was not handled (RIP 0x" << rip
                      << ")";
                throw std::runtime_error(error.str());
            }

            std::optional<uint64_t> virtual_address_of(const uint64_t physical) const
            {
                const auto physical_page = physical & ~(page_size - 1);
                for (const auto& [address, page] : pages_)
                {
                    if (page.gpa == physical_page)
                    {
                        return address + (physical - physical_page);
                    }
                }
                return std::nullopt;
            }

            void finish_hooked_step()
            {
                const auto page_address = *stepped_page_;
                stepped_page_.reset();
                const auto page = pages_.find(page_address);
                if (page != pages_.end())
                {
                    apply_page_permissions(page_address, page->second);
                }
            }

            emulator_hook* add_access_hook(memory_access_hook_callback callback)
            {
                auto& hook = access_hooks_.emplace_back(std::make_unique<memory_access_hook_callback>(std::move(callback)));
                return reinterpret_cast<emulator_hook*>(hook.get());
            }

            bool has_execution_hook(const uint64_t page) const
            {
                return std::ranges::any_of(execution_hooks_, [&](const std::unique_ptr<execution_hook>& hook) {
                    return hook->address < page + page_size && page < hook->address + hook->size;
                });
            }

            // Stage 2 carries the page permissions; the guest's own tables map every page present, writable and
            // user-accessible. EPT has no write-only pages, so writable pages are readable too.
            hv_memory_flags_t ept_permissions(const uint64_t address, const mapped_page& page) const
            {
                hv_memory_flags_t flags{};
                if (is_readable(page.permissions) || is_writable(page.permissions) || is_executable(page.permissions))
                {
                    flags |= HV_MEMORY_READ;
                }
                if (is_writable(page.permissions))
                {
                    flags |= HV_MEMORY_WRITE;
                }
                if (is_executable(page.permissions) && (stepped_page_ == address || !has_execution_hook(address)))
                {
                    flags |= HV_MEMORY_EXEC;
                }
                return flags;
            }

            void apply_page_permissions(const uint64_t address, mapped_page& page) const
            {
                const auto flags = ept_permissions(address, page);
                if (flags == 0)
                {
                    if (page.present)
                    {
                        check_hv(hv_vm_unmap(page.gpa, page_size), "hv_vm_unmap");
                        page.present = false;
                    }
                }
                else if (page.present)
                {
                    check_hv(hv_vm_protect(page.gpa, page_size, flags), "hv_vm_protect");
                }
                else
                {
                    check_hv(hv_vm_map(page.host, page.gpa, page_size, flags), "hv_vm_map");
                    page.present = true;
                }
            }

            void refresh_permissions(const uint64_t address, const uint64_t size)
            {
                const auto first = address & ~(page_size - 1);
                const auto end = address + size;
                for (auto page = pages_.lower_bound(first); page != pages_.end() && page->first < end; ++page)
                {
                    apply_page_permissions(page->first, page->second);
                }
            }

            void refresh_mmio()
            {
                for (const auto& region : mmio_regions_ | std::views::values)
                {
                    for (size_t offset = 0; offset < region->size; offset += page_size)
                    {
                        region->read(offset, pages_.at(region->address + offset).host, page_size);
                    }
                }
            }

            translation_table allocate_table()
            {
                uint64_t offset{};
                if (!free_table_offsets_.empty())
                {
                    offset = free_table_offsets_.back();
                    free_table_offsets_.pop_back();
                }
                else
                {
                    if (next_table_offset_ >= table_pool_size)
                    {
                        throw std::runtime_error("HVF x86-64 translation table space exhausted");
                    }
                    offset = next_table_offset_;
                    next_table_offset_ += page_size;
                }
                return {.entries = reinterpret_cast<uint64_t*>(table_pool_->memory + offset), .gpa = table_physical_base + offset};
            }

            // Level n tables are keyed by the address bits above the range they map; the PML4 is level 4.
            static std::pair<unsigned, uint64_t> table_key(const unsigned level, const uint64_t address)
            {
                return {level, level == 4 ? 0 : address >> (12 + 9 * level)};
            }

            static size_t table_index(const unsigned level, const uint64_t address)
            {
                return (address >> (3 + 9 * level)) & 0x1FF;
            }

            translation_table& leaf_table(const uint64_t address)
            {
                auto* table = &translation_tables_.at(table_key(4, address));
                for (unsigned level = 3; level >= 1; --level)
                {
                    auto found = translation_tables_.find(table_key(level, address));
                    if (found == translation_tables_.end())
                    {
                        found = translation_tables_.emplace(table_key(level, address), allocate_table()).first;
                        table->entries[table_index(level + 1, address)] = found->second.gpa | pte_present | pte_writable | pte_user;
                        ++table->used;
                    }
                    table = &found->second;
                }
                return *table;
            }

            void set_translation(const uint64_t address, const uint64_t gpa)
            {
                auto& leaf = leaf_table(address);
                auto& slot = leaf.entries[table_index(1, address)];
                if (!slot)
                {
                    ++leaf.used;
                }
                else if ((slot & pte_address_mask) != gpa)
                {
                    ++translation_generation_;
                }
                slot = gpa | pte_present | pte_writable | pte_user;
            }

            // Tables left empty return to the pool, so long-running guests that touch many distinct regions do not
            // exhaust it.
            void clear_translation(const uint64_t address)
            {
                for (unsigned level = 1; level <= 4; ++level)
                {
                    const auto table = translation_tables_.find(table_key(level, address));
                    if (table == translation_tables_.end())
                    {
                        return;
                    }
                    auto& slot = table->second.entries[table_index(level, address)];
                    if (!slot)
                    {
                        return;
                    }
                    slot = 0;
                    ++translation_generation_;
                    if (level == 4 || --table->second.used != 0)
                    {
                        return;
                    }
                    free_table_offsets_.push_back(table->second.gpa - table_physical_base);
                    translation_tables_.erase(table);
                }
            }

            uint64_t allocate_gpa()
            {
                if (!free_gpas_.empty())
                {
                    const auto gpa = free_gpas_.back();
                    free_gpas_.pop_back();
                    return gpa;
                }
                const auto gpa = next_gpa_;
                next_gpa_ += page_size;
                return gpa;
            }

            // Owned guest pages come from large host chunks; a reused page is zeroed, as new memory is.
            std::byte* allocate_owned_page()
            {
                if (!free_hosts_.empty())
                {
                    auto* host = free_hosts_.back();
                    free_hosts_.pop_back();
                    return host;
                }
                if (!dirty_hosts_.empty())
                {
                    auto* host = dirty_hosts_.back();
                    dirty_hosts_.pop_back();
                    std::memset(host, 0, page_size);
                    return host;
                }
                auto chunk = std::make_unique<host_allocation>(pool_chunk_size);
                for (uint64_t offset = pool_chunk_size; offset > page_size; offset -= page_size)
                {
                    free_hosts_.push_back(chunk->memory + offset - page_size);
                }
                auto* host = chunk->memory;
                pool_chunks_.push_back(std::move(chunk));
                return host;
            }

            void release_page(const mapped_page& page)
            {
                if (page.present)
                {
                    const auto result = hv_vm_unmap(page.gpa, page_size);
                    if (result != HV_SUCCESS)
                    {
                        // Keep the guest physical page reserved; its host page stays mapped until the VM is destroyed.
                        log_cleanup_failure(result, "hv_vm_unmap");
                        return;
                    }
                }
                free_gpas_.push_back(page.gpa);
                if (page.owned)
                {
                    dirty_hosts_.push_back(page.host);
                }
            }

            static void validate_range(const uint64_t address, const size_t size)
            {
                if (!size || address % page_size != 0 || size % page_size != 0 || address >= user_address_limit ||
                    size > user_address_limit - address)
                {
                    throw std::invalid_argument("HVF x86-64 mappings must be nonempty, page-aligned, and within the user address space");
                }
            }

            void map_memory(const uint64_t address, const size_t size, const memory_permission permissions) override
            {
                map_pages(address, size, nullptr, permissions);
            }

            bool map_host_memory(const uint64_t address, const size_t size, void* host, const memory_permission permissions) override
            {
                if (!host || reinterpret_cast<uintptr_t>(host) % page_size != 0)
                {
                    throw std::invalid_argument("HVF x86-64 host backing must be nonnull and page-aligned");
                }
                map_pages(address, size, static_cast<std::byte*>(host), permissions);
                return true;
            }

            void map_pages(const uint64_t address, const size_t size, std::byte* host, const memory_permission permissions)
            {
                validate_range(address, size);
                const auto existing = pages_.lower_bound(address);
                if (existing != pages_.end() && existing->first < address + size)
                {
                    throw std::invalid_argument("HVF x86-64 guest mapping overlaps an existing page");
                }
                std::map<uint64_t, mapped_page> pending{};
                auto rollback = utils::finally([&] {
                    for (const auto& [page_address, page] : pending)
                    {
                        clear_translation(page_address);
                        release_page(page);
                    }
                });
                for (size_t offset = 0; offset < size; offset += page_size)
                {
                    mapped_page page{.host = host ? host + offset : allocate_owned_page(),
                                     .gpa = allocate_gpa(),
                                     .owned = host == nullptr,
                                     .permissions = permissions};
                    auto& inserted = pending.emplace(address + offset, std::move(page)).first->second;
                    apply_page_permissions(address + offset, inserted);
                    set_translation(address + offset, inserted.gpa);
                }
                pages_.merge(pending);
                rollback.cancel();
            }

            void require_mapped_range(const uint64_t address, const size_t size) const
            {
                validate_range(address, size);
                for (uint64_t page = address; page < address + size; page += page_size)
                {
                    if (!pages_.contains(page))
                    {
                        throw std::invalid_argument("HVF x86-64 range contains an unmapped page");
                    }
                }
            }

            void unmap_memory(const uint64_t address, const size_t size) override
            {
                require_mapped_range(address, size);
                for (uint64_t current = address; current < address + size; current += page_size)
                {
                    const auto page = pages_.extract(current);
                    clear_translation(current);
                    if (page.mapped().mmio)
                    {
                        mmio_regions_.erase(page.mapped().mmio->address);
                    }
                    if (stepped_page_ == current)
                    {
                        stepped_page_.reset();
                    }
                    release_page(page.mapped());
                }
            }

            void apply_memory_protection(const uint64_t address, const size_t size, const memory_permission permissions) override
            {
                require_mapped_range(address, size);
                for (uint64_t current = address; current < address + size; current += page_size)
                {
                    auto& page = pages_.at(current);
                    page.permissions = permissions;
                    apply_page_permissions(current, page);
                }
            }

            // Guest loads from an MMIO range read a read-only page that is refreshed from the read callback before
            // every guest entry. Guest stores fault like stores to any read-only page: decoding them would need an
            // instruction emulator. Host accesses go to the callbacks directly.
            void map_mmio(const uint64_t address, const size_t size, mmio_read_callback read_cb, mmio_write_callback write_cb) override
            {
                if (!read_cb)
                {
                    throw std::invalid_argument("HVF x86-64 MMIO requires a read callback");
                }
                map_pages(address, size, nullptr, memory_permission::read);
                auto region = std::make_shared<mmio_region>(
                    mmio_region{.address = address, .size = size, .read = std::move(read_cb), .write = std::move(write_cb)});
                for (uint64_t offset = 0; offset < size; offset += page_size)
                {
                    pages_.at(address + offset).mmio = region;
                }
                mmio_regions_.emplace(address, std::move(region));
            }

            bool access_memory(const uint64_t address, void* data, const size_t size, const bool write) const
            {
                if (!size)
                {
                    return true;
                }
                if (!data || address >= user_address_limit || size > user_address_limit - address)
                {
                    return false;
                }
                const auto end = address + size;
                for (auto page = address & ~(page_size - 1); page < end; page += page_size)
                {
                    if (!pages_.contains(page))
                    {
                        return false;
                    }
                }
                auto current = address;
                auto* bytes = static_cast<std::byte*>(data);
                while (current < end)
                {
                    const auto offset = current % page_size;
                    const auto& page = pages_.at(current - offset);
                    auto* host = page.host + offset;
                    const auto count = static_cast<size_t>(std::min(page_size - offset, end - current));
                    if (page.mmio)
                    {
                        const auto mmio_offset = current - page.mmio->address;
                        if (!write)
                        {
                            page.mmio->read(mmio_offset, bytes, count);
                        }
                        else if (page.mmio->write)
                        {
                            page.mmio->write(mmio_offset, bytes, count);
                        }
                    }
                    else if (write)
                    {
                        std::memcpy(host, bytes, count);
                    }
                    else
                    {
                        std::memcpy(bytes, host, count);
                    }
                    bytes += count;
                    current += count;
                }
                return true;
            }
        };

        bool has_hypervisor_entitlement()
        {
            auto* task = SecTaskCreateFromSelf(kCFAllocatorDefault);
            if (!task)
            {
                return false;
            }
            const auto release_task = utils::finally([&] { CFRelease(task); });
            auto* value = SecTaskCopyValueForEntitlement(task, CFSTR("com.apple.security.hypervisor"), nullptr);
            if (!value)
            {
                return false;
            }
            const auto release_value = utils::finally([&] { CFRelease(value); });
            return CFGetTypeID(value) == CFBooleanGetTypeID() && CFBooleanGetValue(static_cast<CFBooleanRef>(value));
        }
    }

    bool is_available()
    {
        int supported{};
        size_t size = sizeof(supported);
        if (sysctlbyname("kern.hv_support", &supported, &size, nullptr, 0) != 0 || !supported)
        {
            return false;
        }
        return has_hypervisor_entitlement();
    }

    std::unique_ptr<x86_64_emulator> create_x86_64_emulator(const size_t vcpu_count)
    {
        if (vcpu_count != 1)
        {
            throw std::invalid_argument("HVF x86-64 requires exactly one vCPU");
        }
        return std::make_unique<hvf_x86_64_emulator>();
    }
}
