#define HVF_ARM64_EMULATOR_IMPL
#include "hvf_arm64_emulator.hpp"

#include <Hypervisor/Hypervisor.h>
#include <Security/Security.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/sysctl.h>

#include <utils/finally.hpp>
#include <utils/object.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <exception>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace sogen::hvf
{
    namespace
    {
        // Guest pages are 4 KiB; Hypervisor.framework maps guest physical memory in host pages (16 KiB).
        constexpr uint64_t page_size = 0x1000;
        constexpr uint64_t block_size = 0x4000;
        constexpr uint64_t vector_gpa = 0xFFFF0000;
        // The exit page lives in its own unmapped stage-2 block, so the vectors' store to it always exits.
        constexpr uint64_t exit_gpa = vector_gpa + block_size;
        constexpr uint64_t resume_gpa = vector_gpa + 0x800;
        constexpr uint64_t resume_without_flush_gpa = resume_gpa + 0x20;
        constexpr uint64_t user_address_limit = 1ull << 48;
        constexpr uint64_t page_table_base = 0xFE000000;
        constexpr uint64_t page_table_size = 0x01000000;
        // Guest pages get physical addresses from this point up to the VM's IPA width, above the private
        // translation table and exception-vector ranges.
        constexpr uint64_t gpa_pool_base = 1ull << 32;
        constexpr uint64_t pool_chunk_size = 64ull << 20;
        constexpr uint64_t nzcv_mask = 0xF0000000;
        constexpr uint64_t software_step_bit = 1ull << 21;
        constexpr uint64_t user_vector_offset = 0x404;
        constexpr int instruction_abort_class = 0x20;
        constexpr int data_abort_class = 0x24;
        constexpr int system_register_trap_class = 0x18;
        constexpr int software_step_class = 0x32;
        constexpr int svc_class = 0x15;
        // MRS Xt of CNTVCT_EL0 and PMCCNTR_EL0 without the Rt field.
        constexpr uint32_t mrs_virtual_counter = 0xD53BE040;
        constexpr uint32_t mrs_self_synchronized_virtual_counter = 0xD53BE0C0;
        constexpr uint32_t mrs_cycle_counter = 0xD53B9D00;
        constexpr auto all_access = HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC;
        constexpr uint64_t output_address_mask = 0x0000FFFFFFFFF000ull;
        // Hook callbacks, and with them the whole syscall layer, run on a vCPU's thread; match the main thread's stack.
        constexpr size_t vcpu_thread_stack_size = 8ull << 20;
        // Faults retried in a row at one instruction before a permitted access is reported as a violation anyway.
        constexpr uint32_t maximum_fault_repairs = 64;

        arm64_register general_register(const uint32_t index)
        {
            switch (index)
            {
            case 29:
                return arm64_register::x29;
            case 30:
                return arm64_register::x30;
            case 31:
                return arm64_register::xzr;
            default:
                return static_cast<arm64_register>(static_cast<int>(arm64_register::x0) + static_cast<int>(index));
            }
        }

        memory_permission granted_user_access(const uint64_t descriptor)
        {
            if ((descriptor & 1) == 0)
            {
                return memory_permission::none;
            }
            auto access = memory_permission::none;
            if ((descriptor & (1ull << 6)) != 0)
            {
                access |= memory_permission::read;
                if ((descriptor & (1ull << 7)) == 0)
                {
                    access |= memory_permission::write;
                }
            }
            if ((descriptor & (1ull << 54)) == 0)
            {
                access |= memory_permission::exec;
            }
            return access;
        }

        // Whether a peer's TLB, still holding the previous descriptor, could allow what the new one denies.
        bool revokes_access(const uint64_t previous, const uint64_t descriptor)
        {
            if ((previous & 1) == 0)
            {
                return false;
            }
            if ((descriptor & 1) == 0 || (previous & output_address_mask) != (descriptor & output_address_mask))
            {
                return true;
            }
            return (granted_user_access(previous) & ~granted_user_access(descriptor)) != memory_permission::none;
        }

        // Write access implies read access at stage 1.
        bool permits(const memory_permission permissions, const memory_operation operation)
        {
            if (operation == memory_operation::exec)
            {
                return is_executable(permissions);
            }
            if (operation == memory_operation::write)
            {
                return is_writable(permissions);
            }
            return is_readable(permissions) || is_writable(permissions);
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

        void flush_instruction_cache(const void* host, const size_t size)
        {
            sys_icache_invalidate(const_cast<void*>(host), size);
        }

        std::byte* allocate_host(const size_t size)
        {
            auto* memory = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
            if (memory == MAP_FAILED)
            {
                throw std::runtime_error("mmap failed while backing HVF ARM64 memory");
            }
            return static_cast<std::byte*>(memory);
        }

        // hv_vm_map hands the whole 16 KiB host page to the VM. Executable memory there (MAP_JIT or code) makes the
        // page table monitor panic the machine on macOS 27 (SPTM VIOLATION_ILLEGAL_MAPPING_TYPE), so it is refused
        // before it reaches the hypervisor. Submaps (the shared cache) are descended to their real protection.
        bool is_plain_data(const uintptr_t block)
        {
            mach_vm_address_t next = block;
            while (next < block + block_size)
            {
                natural_t depth = 0;
                for (;;)
                {
                    mach_vm_address_t address = next;
                    mach_vm_size_t size{};
                    vm_region_submap_info_data_64_t info{};
                    mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
                    if (mach_vm_region_recurse(mach_task_self(), &address, &size, &depth, reinterpret_cast<vm_region_recurse_info_t>(&info),
                                               &count) != KERN_SUCCESS ||
                        address > next)
                    {
                        return false;
                    }
                    if (info.is_submap)
                    {
                        ++depth;
                        continue;
                    }
                    if ((info.protection & VM_PROT_EXECUTE) != 0)
                    {
                        return false;
                    }
                    next = address + size;
                    break;
                }
            }
            return true;
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
                    std::fprintf(stderr, "munmap failed during HVF ARM64 cleanup\n");
                }
            }

            host_allocation(const host_allocation&) = delete;
            host_allocation& operator=(const host_allocation&) = delete;
        };

        struct vm_handle
        {
            uint64_t gpa_limit{};

            vm_handle()
            {
                // The default IPA space (36 bits) is all macOS 11 and 12 offer; newer systems may allow more.
                uint32_t ipa_bits = 36;
                hv_return_t result{};
                if (__builtin_available(macOS 13.0, *))
                {
                    check_hv(hv_vm_config_get_max_ipa_size(&ipa_bits), "hv_vm_config_get_max_ipa_size");
                    auto* config = hv_vm_config_create();
                    const auto release = utils::finally([&] { os_release(config); });
                    check_hv(hv_vm_config_set_ipa_size(config, ipa_bits), "hv_vm_config_set_ipa_size");
                    result = hv_vm_create(config);
                }
                else
                {
                    result = hv_vm_create(nullptr);
                }
                if (result == HV_BUSY)
                {
                    throw std::runtime_error("Hypervisor.framework allows one VM per process; destroy the other HVF emulator first");
                }
                check_hv(result, "hv_vm_create");
                gpa_limit = ipa_bits >= 64 ? ~uint64_t{} : 1ull << ipa_bits;
            }

            ~vm_handle()
            {
                log_cleanup_failure(hv_vm_destroy(), "hv_vm_destroy");
            }

            vm_handle(const vm_handle&) = delete;
            vm_handle& operator=(const vm_handle&) = delete;
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
            memory_permission permissions{};
            std::shared_ptr<mmio_region> mmio{};
        };

        struct translation_table
        {
            uint64_t* entries{};
            uint64_t gpa{};
            uint32_t used{};
        };

        // A host-provided 16 KiB block shared by the guest pages that live in it.
        struct host_block
        {
            uint64_t gpa{};
            uint32_t references{};
        };

        struct owned_slot
        {
            std::byte* host{};
            uint64_t gpa{};
        };

        enum class register_kind : uint8_t
        {
            general,
            simd,
            system,
            zero,
        };

        struct register_mapping
        {
            register_kind kind{};
            uint32_t id{};
            size_t size{sizeof(uint64_t)};
            bool nzcv{};
        };

        constexpr hv_sys_reg_t debug_register(const hv_sys_reg_t first, const int index)
        {
            // DBGB/W{V,C}Rn_EL1 encode n in CRm, eight encodings apart.
            return static_cast<hv_sys_reg_t>(static_cast<uint32_t>(first) + 8 * static_cast<uint32_t>(index));
        }

        constexpr register_mapping system_register(const hv_sys_reg_t reg, const size_t size = sizeof(uint64_t))
        {
            return {.kind = register_kind::system, .id = static_cast<uint32_t>(reg), .size = size};
        }

        register_mapping map_register(const arm64_register reg)
        {
            const auto id = static_cast<int>(reg);
            if (reg >= arm64_register::bcr0 && reg <= arm64_register::bcr7)
            {
                return system_register(debug_register(HV_SYS_REG_DBGBCR0_EL1, id - static_cast<int>(arm64_register::bcr0)),
                                       sizeof(uint32_t));
            }
            if (reg >= arm64_register::bvr0 && reg <= arm64_register::bvr7)
            {
                return system_register(debug_register(HV_SYS_REG_DBGBVR0_EL1, id - static_cast<int>(arm64_register::bvr0)));
            }
            if (reg >= arm64_register::wcr0 && reg <= arm64_register::wcr1)
            {
                return system_register(debug_register(HV_SYS_REG_DBGWCR0_EL1, id - static_cast<int>(arm64_register::wcr0)),
                                       sizeof(uint32_t));
            }
            if (reg >= arm64_register::wvr0 && reg <= arm64_register::wvr1)
            {
                return system_register(debug_register(HV_SYS_REG_DBGWVR0_EL1, id - static_cast<int>(arm64_register::wvr0)));
            }
            if (reg >= arm64_register::x0 && reg <= arm64_register::x28)
            {
                return {.kind = register_kind::general, .id = static_cast<uint32_t>(HV_REG_X0 + id - static_cast<int>(arm64_register::x0))};
            }
            if (reg >= arm64_register::w0 && reg <= arm64_register::w30)
            {
                return {.kind = register_kind::general,
                        .id = static_cast<uint32_t>(HV_REG_X0 + id - static_cast<int>(arm64_register::w0)),
                        .size = sizeof(uint32_t)};
            }
            if (reg >= arm64_register::q0 && reg <= arm64_register::q31)
            {
                return {.kind = register_kind::simd,
                        .id = static_cast<uint32_t>(HV_SIMD_FP_REG_Q0 + id - static_cast<int>(arm64_register::q0)),
                        .size = 16};
            }
            if (reg >= arm64_register::v0 && reg <= arm64_register::v31)
            {
                return {.kind = register_kind::simd,
                        .id = static_cast<uint32_t>(HV_SIMD_FP_REG_Q0 + id - static_cast<int>(arm64_register::v0)),
                        .size = 16};
            }

            switch (reg)
            {
            case arm64_register::x29:
                return {.kind = register_kind::general, .id = HV_REG_FP};
            case arm64_register::x30:
                return {.kind = register_kind::general, .id = HV_REG_LR};
            case arm64_register::pc:
                return {.kind = register_kind::general, .id = HV_REG_PC};
            case arm64_register::sp:
                return system_register(HV_SYS_REG_SP_EL0);
            case arm64_register::wsp:
                return system_register(HV_SYS_REG_SP_EL0, sizeof(uint32_t));
            case arm64_register::xzr:
                return {.kind = register_kind::zero};
            case arm64_register::wzr:
                return {.kind = register_kind::zero, .size = sizeof(uint32_t)};
            case arm64_register::nzcv:
                return {.kind = register_kind::general, .id = HV_REG_CPSR, .size = sizeof(uint32_t), .nzcv = true};
            case arm64_register::pstate:
                return {.kind = register_kind::general, .id = HV_REG_CPSR, .size = sizeof(uint32_t)};
            case arm64_register::fpcr:
                return {.kind = register_kind::general, .id = HV_REG_FPCR, .size = sizeof(uint32_t)};
            case arm64_register::fpsr:
                return {.kind = register_kind::general, .id = HV_REG_FPSR, .size = sizeof(uint32_t)};
            case arm64_register::tpidr_el0:
                return system_register(HV_SYS_REG_TPIDR_EL0);
            case arm64_register::tpidrro_el0:
                return system_register(HV_SYS_REG_TPIDRRO_EL0);
            case arm64_register::midr_el1:
                return system_register(HV_SYS_REG_MIDR_EL1);
            case arm64_register::id_aa64isar0_el1:
                return system_register(HV_SYS_REG_ID_AA64ISAR0_EL1);
            case arm64_register::id_aa64isar1_el1:
                return system_register(HV_SYS_REG_ID_AA64ISAR1_EL1);
            default:
                throw std::invalid_argument("Unsupported HVF ARM64 register: " + std::to_string(id));
            }
        }

        bool is_identification_register(const register_mapping& mapping)
        {
            return mapping.kind == register_kind::system &&
                   (mapping.id == HV_SYS_REG_MIDR_EL1 || mapping.id == HV_SYS_REG_ID_AA64ISAR0_EL1 ||
                    mapping.id == HV_SYS_REG_ID_AA64ISAR1_EL1);
        }

        // The same registers, order and 16-byte slots as the WHP ARM64 backend's saved state.
        constexpr auto saved_registers = [] {
            std::array<register_mapping, 91> registers{};
            size_t index{};
            for (uint32_t reg = 0; reg < 31; ++reg)
            {
                registers[index++] = {.kind = register_kind::general, .id = HV_REG_X0 + reg};
            }
            for (uint32_t reg = 0; reg < 32; ++reg)
            {
                registers[index++] = {.kind = register_kind::simd, .id = HV_SIMD_FP_REG_Q0 + reg, .size = 16};
            }
            registers[index++] = {.kind = register_kind::general, .id = HV_REG_PC};
            registers[index++] = {.kind = register_kind::general, .id = HV_REG_CPSR};
            registers[index++] = system_register(HV_SYS_REG_SP_EL0);
            registers[index++] = system_register(HV_SYS_REG_TPIDR_EL0);
            registers[index++] = system_register(HV_SYS_REG_TPIDRRO_EL0);
            registers[index++] = {.kind = register_kind::general, .id = HV_REG_FPCR};
            registers[index++] = {.kind = register_kind::general, .id = HV_REG_FPSR};
            registers[index++] = system_register(HV_SYS_REG_CPACR_EL1);
            for (const auto first : {HV_SYS_REG_DBGBCR0_EL1, HV_SYS_REG_DBGBVR0_EL1})
            {
                for (int reg = 0; reg < 8; ++reg)
                {
                    registers[index++] = system_register(debug_register(first, reg));
                }
            }
            for (const auto first : {HV_SYS_REG_DBGWCR0_EL1, HV_SYS_REG_DBGWVR0_EL1})
            {
                for (int reg = 0; reg < 2; ++reg)
                {
                    registers[index++] = system_register(debug_register(first, reg));
                }
            }
            return registers;
        }();
        constexpr size_t saved_pstate_index = 64;

        [[noreturn]] emulator_hook* unsupported(const char* operation)
        {
            throw std::runtime_error(std::string("HVF ARM64 does not support ") + operation);
        }

        // Hypervisor.framework binds a vCPU to the thread that created it, while the scheduler drives each vCPU from
        // worker threads it recreates on every run. A vCPU of a multi-vCPU machine therefore lives on a host thread
        // of its own, and every operation on it runs there.
        class vcpu_thread
        {
          public:
            vcpu_thread()
            {
                pthread_attr_t attributes{};
                pthread_attr_init(&attributes);
                const auto release = utils::finally([&] { pthread_attr_destroy(&attributes); });
                pthread_attr_setstacksize(&attributes, vcpu_thread_stack_size);
                if (pthread_create(&thread_, &attributes, &vcpu_thread::entry, this) != 0)
                {
                    throw std::runtime_error("Failed to create an HVF ARM64 vCPU thread");
                }
            }

            ~vcpu_thread()
            {
                {
                    std::lock_guard lock(mutex_);
                    exit_ = true;
                }
                condition_.notify_all();
                pthread_join(thread_, nullptr);
            }

            vcpu_thread(const vcpu_thread&) = delete;
            vcpu_thread& operator=(const vcpu_thread&) = delete;
            vcpu_thread(vcpu_thread&&) = delete;
            vcpu_thread& operator=(vcpu_thread&&) = delete;

            pthread_t handle() const
            {
                return thread_;
            }

            void execute(const std::function<void()>& task)
            {
                const std::lock_guard caller(call_mutex_);
                std::unique_lock lock(mutex_);
                task_ = &task;
                condition_.notify_all();
                condition_.wait(lock, [&] { return task_ == nullptr; });
                if (error_)
                {
                    std::rethrow_exception(std::exchange(error_, nullptr));
                }
            }

          private:
            std::mutex call_mutex_{};
            std::mutex mutex_{};
            std::condition_variable condition_{};
            const std::function<void()>* task_{};
            std::exception_ptr error_{};
            bool exit_{};
            pthread_t thread_{};

            static void* entry(void* self)
            {
                static_cast<vcpu_thread*>(self)->serve();
                return nullptr;
            }

            void serve()
            {
                std::unique_lock lock(mutex_);
                for (;;)
                {
                    condition_.wait(lock, [&] { return exit_ || task_ != nullptr; });
                    if (!task_)
                    {
                        return;
                    }
                    const auto* task = task_;
                    lock.unlock();
                    std::exception_ptr error{};
                    try
                    {
                        (*task)();
                    }
                    catch (...)
                    {
                        error = std::current_exception();
                    }
                    lock.lock();
                    error_ = error;
                    task_ = nullptr;
                    condition_.notify_all();
                }
            }
        };

        class hvf_arm64_emulator;

        class hvf_vcpu final : public arm64_cpu
        {
          public:
            hvf_vcpu(hvf_arm64_emulator& machine, size_t index, bool own_thread);
            ~hvf_vcpu() override;

            hvf_vcpu(const hvf_vcpu&) = delete;
            hvf_vcpu& operator=(const hvf_vcpu&) = delete;
            hvf_vcpu(hvf_vcpu&&) = delete;
            hvf_vcpu& operator=(hvf_vcpu&&) = delete;

            size_t index() const override
            {
                return index_;
            }

            memory_interface& memory() override;
            const memory_interface& memory() const override;

            void start(const size_t count) override
            {
                on_owner([&] { run(count); });
            }

            void stop() override
            {
                std::lock_guard lock(run_mutex_);
                stop_requested_ = true;
                if (in_hypervisor_)
                {
                    check_hv(hv_vcpus_exit(&vcpu_, 1), "hv_vcpus_exit");
                }
            }

            size_t read_raw_register(const int reg, void* value, const size_t size) override
            {
                size_t result{};
                on_owner([&] { result = read_register_on_owner(reg, value, size); });
                return result;
            }

            size_t write_raw_register(const int reg, const void* value, const size_t size) override
            {
                size_t result{};
                on_owner([&] { result = write_register_on_owner(reg, value, size); });
                return result;
            }

            std::vector<std::byte> save_registers() const override
            {
                std::vector<std::byte> result{};
                on_owner([&] { result = save_registers_on_owner(); });
                return result;
            }

            void restore_registers(const std::vector<std::byte>& bytes) override
            {
                on_owner([&] { restore_registers_on_owner(bytes); });
            }

            bool has_violation() const override
            {
                return violation_;
            }

            bool supports_instruction_counting() const override
            {
                return true;
            }

            bool is_stop_thread_safe() const override
            {
                return true;
            }

            // Returns once this vCPU no longer executes with translations cached before the caller's table changes.
            void synchronize_translations()
            {
                std::unique_lock lock(run_mutex_);
                if (!in_hypervisor_)
                {
                    return;
                }
                const auto entry = entries_;
                check_hv(hv_vcpus_exit(&vcpu_, 1), "hv_vcpus_exit");
                run_condition_.wait(lock, [&] { return !in_hypervisor_ || entries_ != entry; });
            }

          private:
            hvf_arm64_emulator& machine_;
            size_t index_{};
            std::unique_ptr<vcpu_thread> thread_{};
            pthread_t owner_{};
            hv_vcpu_t vcpu_{};
            const hv_vcpu_exit_t* exit_{};
            bool created_{};
            mutable std::mutex run_mutex_{};
            std::condition_variable run_condition_{};
            bool running_{};
            bool in_hypervisor_{};
            bool stop_requested_{};
            uint64_t entries_{};
            bool violation_{};
            bool stepping_{};
            bool user_step_{};
            bool hook_only_exit_{};
            std::optional<uint64_t> stepped_page_{};
            std::pair<uint64_t, uint64_t> stepped_context_{};
            bool fault_repaired_{};
            uint64_t repaired_page_{};
            uint64_t repaired_pc_{};
            uint32_t repair_count_{};
            std::optional<uint64_t> flushed_generation_{};
            bool step_armed_{};

            template <typename Function>
            void on_owner(Function&& function) const
            {
                if (pthread_equal(pthread_self(), owner_))
                {
                    function();
                    return;
                }
                if (!thread_)
                {
                    throw std::logic_error("HVF ARM64 vCPUs can only be used from the thread that created the emulator");
                }
                {
                    std::lock_guard lock(run_mutex_);
                    if (running_)
                    {
                        throw std::logic_error("HVF ARM64 vCPU state cannot be accessed from another thread while it runs");
                    }
                }
                thread_->execute(std::function<void()>{std::ref(function)});
            }

            void create();
            void run(size_t count);
            bool enter_guest();
            void prepare_user_entry();
            void restore_user_view();
            void handle_exit();
            void handle_el2_system_register_trap(uint64_t syndrome, uint64_t pc);
            void report_exception(int exception_class);
            bool handle_abort(int exception_class, uint64_t esr, uint64_t return_pc, uint64_t user_pstate, bool user_step,
                              bool repeated_repair);
            bool handle_counter_read(uint64_t return_pc, uint64_t user_pstate, bool user_step);
            void arm_hooked_step();
            void disarm_hooked_step();
            void finish_hooked_step();
            bool absent_debug_register(const register_mapping& mapping) const;

            size_t read_register_on_owner(int reg, void* value, size_t size) const;
            size_t write_register_on_owner(int reg, const void* value, size_t size) const;
            std::vector<std::byte> save_registers_on_owner() const;
            void restore_registers_on_owner(const std::vector<std::byte>& bytes) const;

            void require_owner_thread() const
            {
                if (!pthread_equal(pthread_self(), owner_))
                {
                    throw std::logic_error("HVF ARM64 vCPUs can only be used from the thread that created them");
                }
            }

            uint64_t get_general(const hv_reg_t reg) const
            {
                require_owner_thread();
                uint64_t value{};
                check_hv(hv_vcpu_get_reg(vcpu_, reg, &value), "hv_vcpu_get_reg");
                return value;
            }

            void set_general(const hv_reg_t reg, const uint64_t value) const
            {
                require_owner_thread();
                check_hv(hv_vcpu_set_reg(vcpu_, reg, value), "hv_vcpu_set_reg");
            }

            uint64_t get_system(const hv_sys_reg_t reg) const
            {
                require_owner_thread();
                uint64_t value{};
                check_hv(hv_vcpu_get_sys_reg(vcpu_, reg, &value), "hv_vcpu_get_sys_reg");
                return value;
            }

            void set_system(const hv_sys_reg_t reg, const uint64_t value) const
            {
                require_owner_thread();
                check_hv(hv_vcpu_set_sys_reg(vcpu_, reg, value), "hv_vcpu_set_sys_reg");
            }

            std::array<uint64_t, 2> get_register(const register_mapping& mapping) const
            {
                std::array<uint64_t, 2> result{};
                switch (mapping.kind)
                {
                case register_kind::general:
                    result[0] = get_general(static_cast<hv_reg_t>(mapping.id));
                    break;
                case register_kind::system:
                    result[0] = get_system(static_cast<hv_sys_reg_t>(mapping.id));
                    break;
                case register_kind::simd: {
                    require_owner_thread();
                    hv_simd_fp_uchar16_t value{};
                    check_hv(hv_vcpu_get_simd_fp_reg(vcpu_, static_cast<hv_simd_fp_reg_t>(mapping.id), &value), "hv_vcpu_get_simd_fp_reg");
                    std::memcpy(result.data(), &value, sizeof(result));
                    break;
                }
                case register_kind::zero:
                    break;
                }
                return result;
            }

            void set_register(const register_mapping& mapping, const std::array<uint64_t, 2>& value) const
            {
                switch (mapping.kind)
                {
                case register_kind::general:
                    set_general(static_cast<hv_reg_t>(mapping.id), value[0]);
                    break;
                case register_kind::system:
                    set_system(static_cast<hv_sys_reg_t>(mapping.id), value[0]);
                    break;
                case register_kind::simd: {
                    require_owner_thread();
                    hv_simd_fp_uchar16_t vector{};
                    std::memcpy(&vector, value.data(), sizeof(vector));
                    check_hv(hv_vcpu_set_simd_fp_reg(vcpu_, static_cast<hv_simd_fp_reg_t>(mapping.id), vector), "hv_vcpu_set_simd_fp_reg");
                    break;
                }
                case register_kind::zero:
                    break;
                }
            }

            static void validate_register_buffer(const void* buffer, const size_t size, const size_t required)
            {
                if (!buffer || size < required)
                {
                    throw std::invalid_argument("HVF ARM64 register buffer is too small or null");
                }
            }
        };

        struct page_state
        {
            memory_permission permissions{};
            bool execution_hooked{};
        };

        class hvf_arm64_emulator final : public arm64_emulator
        {
          public:
            explicit hvf_arm64_emulator(const size_t vcpu_count)
            {
                uint32_t maximum_vcpus{};
                check_hv(hv_vm_get_max_vcpu_count(&maximum_vcpus), "hv_vm_get_max_vcpu_count");
                if (vcpu_count == 0 || vcpu_count > maximum_vcpus)
                {
                    throw std::invalid_argument("HVF ARM64 vCPU count must be between 1 and " + std::to_string(maximum_vcpus));
                }

                vector_page_ = std::make_unique<host_allocation>(block_size);
                // Complete preceding guest stores before the host inspects memory. STR XZR,[SP]
                // then faults at SP_EL1 without clobbering a user register.
                constexpr std::array<uint32_t, 2> report_exception{0xD5033F9F, 0xF90003FF};
                for (size_t offset = 0; offset < 0x800; offset += 0x80)
                {
                    std::memcpy(vector_page_->memory + offset, report_exception.data(), sizeof(report_exception));
                }
                // Enter the user's state, invalidating stage-1 translations first when the tables changed.
                constexpr std::array<uint32_t, 5> enter_user{0xD5033F9F, 0xD508831F, 0xD5033F9F, 0xD5033FDF, 0xD69F03E0};
                constexpr std::array<uint32_t, 3> enter_user_without_flush{0xD5033F9F, 0xD5033FDF, 0xD69F03E0};
                std::memcpy(vector_page_->memory + (resume_gpa - vector_gpa), enter_user.data(), sizeof(enter_user));
                std::memcpy(vector_page_->memory + (resume_without_flush_gpa - vector_gpa), enter_user_without_flush.data(),
                            sizeof(enter_user_without_flush));
                flush_instruction_cache(vector_page_->memory, block_size);
                check_hv(hv_vm_map(vector_page_->memory, vector_gpa, block_size, HV_MEMORY_READ | HV_MEMORY_EXEC), "hv_vm_map(vector)");

                table_pool_ = std::make_unique<host_allocation>(page_table_size);
                check_hv(hv_vm_map(table_pool_->memory, page_table_base, page_table_size, HV_MEMORY_READ | HV_MEMORY_WRITE),
                         "hv_vm_map(translation tables)");
                translation_tables_.emplace(std::pair{0u, uint64_t{0}}, allocate_table());
                map_translation(vector_gpa, false);
                map_translation(exit_gpa, false);

                vcpus_.reserve(vcpu_count);
                for (size_t i = 0; i < vcpu_count; ++i)
                {
                    vcpus_.push_back(std::make_unique<hvf_vcpu>(*this, i, vcpu_count > 1));
                }
            }

            ~hvf_arm64_emulator() override
            {
                // Hook callbacks may own objects that delete other hooks when destroyed; release every
                // container before any callback is destroyed.
                destroying_ = true;
                utils::reset_object_with_delayed_destruction(access_hooks_);
                utils::reset_object_with_delayed_destruction(execution_hooks_);
                utils::reset_object_with_delayed_destruction(violation_hooks_);
                utils::reset_object_with_delayed_destruction(interrupt_hooks_);
                utils::reset_object_with_delayed_destruction(counter_hook_);
                utils::reset_object_with_delayed_destruction(syscall_hook_);
                utils::reset_object_with_delayed_destruction(mmio_regions_);
            }

            hvf_arm64_emulator(const hvf_arm64_emulator&) = delete;
            hvf_arm64_emulator& operator=(const hvf_arm64_emulator&) = delete;
            hvf_arm64_emulator(hvf_arm64_emulator&&) = delete;
            hvf_arm64_emulator& operator=(hvf_arm64_emulator&&) = delete;

            std::string get_name() const override
            {
                return "hvf-arm64";
            }

            bool supports_multiple_vcpus() const override
            {
                return true;
            }

            size_t vcpu_count() const override
            {
                return vcpus_.size();
            }

            arm64_cpu& get_cpu(const size_t index) override
            {
                if (index >= vcpus_.size())
                {
                    throw std::out_of_range("Invalid vCPU index");
                }
                return *vcpus_[index];
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

            void flush_host_memory_cache(const void* host, const size_t size) override
            {
                flush_instruction_cache(host, size);
            }

            bool has_violation() const override
            {
                return vcpus_[0]->has_violation();
            }

            // The exception vectors and the exit page share the user address space.
            std::vector<host_reserved_range> reserved_host_ranges() const override
            {
                return {{.address = vector_gpa, .size = 2 * block_size}};
            }

            std::vector<host_reserved_range> reserved_host_ranges_in(const uint64_t address, const size_t size) const override
            {
                std::vector<host_reserved_range> result{};
                for (const auto& range : reserved_host_ranges())
                {
                    if (range.address < address + size && address < range.address + range.size)
                    {
                        result.push_back(range);
                    }
                }
                return result;
            }

            void start(const size_t count) override
            {
                vcpus_[0]->start(count);
            }

            void stop() override
            {
                vcpus_[0]->stop();
            }

            size_t read_raw_register(const int reg, void* value, const size_t size) override
            {
                return vcpus_[0]->read_raw_register(reg, value, size);
            }

            size_t write_raw_register(const int reg, const void* value, const size_t size) override
            {
                return vcpus_[0]->write_raw_register(reg, value, size);
            }

            std::vector<std::byte> save_registers() const override
            {
                return vcpus_[0]->save_registers();
            }

            void restore_registers(const std::vector<std::byte>& bytes) override
            {
                vcpus_[0]->restore_registers(bytes);
            }

            void serialize_state(utils::buffer_serializer& buffer, bool) const override
            {
                if (vcpus_.size() > 1)
                {
                    throw std::runtime_error("Multi-vCPU snapshots are not supported yet");
                }
                buffer.write_vector(save_registers());
            }

            void deserialize_state(utils::buffer_deserializer& buffer, bool) override
            {
                if (vcpus_.size() > 1)
                {
                    throw std::runtime_error("Multi-vCPU snapshots are not supported yet");
                }
                restore_registers(buffer.read_vector<std::byte>());
            }

            bool try_read_memory(const uint64_t address, void* data, const size_t size) const override
            {
                const std::shared_lock lock(state_mutex_);
                return access_memory(address, data, size, false);
            }

            void read_memory(const uint64_t address, void* data, const size_t size) const override
            {
                if (!try_read_memory(address, data, size))
                {
                    throw std::runtime_error("Reading unmapped HVF ARM64 guest memory at " + std::to_string(address));
                }
            }

            bool try_write_memory(const uint64_t address, const void* data, const size_t size) override
            {
                const std::shared_lock lock(state_mutex_);
                return access_memory(address, const_cast<void*>(data), size, true);
            }

            void write_memory(const uint64_t address, const void* data, const size_t size) override
            {
                if (!try_write_memory(address, data, size))
                {
                    throw std::runtime_error("Writing unmapped HVF ARM64 guest memory at " + std::to_string(address));
                }
            }

            emulator_hook* hook_instruction(const int instruction, instruction_hook_callback callback) override
            {
                const std::unique_lock lock(state_mutex_);
                if (instruction == static_cast<int>(arm64_hookable_instructions::read_counter) && callback)
                {
                    if (counter_hook_)
                    {
                        throw std::logic_error("HVF ARM64 supports one counter instruction hook");
                    }
                    counter_hook_ = std::make_unique<syscall_hook>(syscall_hook{std::move(callback)});
                    return reinterpret_cast<emulator_hook*>(counter_hook_.get());
                }
                if (instruction != static_cast<int>(arm64_hookable_instructions::svc) || !callback)
                {
                    throw std::invalid_argument("HVF ARM64 requires a nonempty SVC instruction hook");
                }
                if (syscall_hook_)
                {
                    throw std::logic_error("HVF ARM64 supports one SVC instruction hook");
                }
                syscall_hook_ = std::make_unique<syscall_hook>(syscall_hook{std::move(callback)});
                return reinterpret_cast<emulator_hook*>(syscall_hook_.get());
            }

            void delete_hook(emulator_hook* hook) override
            {
                if (destroying_)
                {
                    return;
                }
                // The removed hook is destroyed after the lock is released: its callback may own objects that
                // delete further hooks.
                std::shared_ptr<void> removed{};
                const std::unique_lock lock(state_mutex_);
                const auto take = [&](auto& container) {
                    const auto entry = std::ranges::find_if(
                        container, [&](const auto& candidate) { return reinterpret_cast<emulator_hook*>(candidate.get()) == hook; });
                    if (entry == container.end())
                    {
                        return false;
                    }
                    removed = std::shared_ptr<void>(std::move(*entry));
                    container.erase(entry);
                    return true;
                };
                if (syscall_hook_ && hook == reinterpret_cast<emulator_hook*>(syscall_hook_.get()))
                {
                    removed = std::shared_ptr<void>(std::move(syscall_hook_));
                    return;
                }
                if (counter_hook_ && hook == reinterpret_cast<emulator_hook*>(counter_hook_.get()))
                {
                    removed = std::shared_ptr<void>(std::move(counter_hook_));
                    return;
                }
                if (take(access_hooks_) || take(violation_hooks_) || take(interrupt_hooks_))
                {
                    return;
                }
                const auto execution = std::ranges::find_if(execution_hooks_, [&](const std::unique_ptr<execution_hook>& entry) {
                    return reinterpret_cast<emulator_hook*>(entry.get()) == hook;
                });
                if (execution == execution_hooks_.end())
                {
                    throw std::invalid_argument("Unknown HVF ARM64 hook");
                }
                const auto address = (*execution)->address;
                const auto size = (*execution)->size;
                removed = std::shared_ptr<void>(std::move(*execution));
                execution_hooks_.erase(execution);
                refresh_translations(address, size);
                publish_translations();
            }

            emulator_hook* hook_memory_execution(memory_execution_hook_callback) override
            {
                return unsupported("memory execution hooks");
            }

            emulator_hook* hook_memory_execution(const uint64_t address, memory_execution_hook_callback callback) override
            {
                return hook_memory_range_execution(address, 1, std::move(callback));
            }

            // Hooked pages lose EL0 execute permission at stage 1. The instruction abort runs the hooks,
            // and the instruction then executes once, single-stepped, before the page is protected again.
            emulator_hook* hook_memory_range_execution(const uint64_t address, const uint64_t size,
                                                       memory_execution_hook_callback callback) override
            {
                if (!size || !callback || address > UINT64_MAX - size)
                {
                    throw std::invalid_argument("HVF ARM64 execution hooks need a nonempty range and callback");
                }
                const std::unique_lock lock(state_mutex_);
                auto& hook = execution_hooks_.emplace_back(
                    std::make_unique<execution_hook>(execution_hook{.address = address, .size = size, .callback = std::move(callback)}));
                refresh_translations(address, size);
                publish_translations();
                return reinterpret_cast<emulator_hook*>(hook.get());
            }

            // As on WHP, data access hooks are accepted but not reported: trapping every access to a watched
            // page (the TEB, for example) would fault on nearly every instruction.
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
                    throw std::invalid_argument("HVF ARM64 requires a nonempty interrupt hook");
                }
                const std::unique_lock lock(state_mutex_);
                auto& hook = interrupt_hooks_.emplace_back(std::make_unique<interrupt_hook>(interrupt_hook{std::move(callback)}));
                return reinterpret_cast<emulator_hook*>(hook.get());
            }

            emulator_hook* hook_memory_violation(memory_violation_hook_callback callback) override
            {
                if (!callback)
                {
                    throw std::invalid_argument("HVF ARM64 requires a nonempty memory violation hook");
                }
                const std::unique_lock lock(state_mutex_);
                auto& hook = violation_hooks_.emplace_back(std::make_unique<violation_hook>(violation_hook{std::move(callback)}));
                return reinterpret_cast<emulator_hook*>(hook.get());
            }

            emulator_hook* hook_basic_block(basic_block_hook_callback) override
            {
                return unsupported("basic block hooks");
            }

            void share_host_memory(const uint64_t address, const size_t size, std::byte* const shared,
                                   const std::optional<memory_permission> permissions)
            {
                validate_range(address, size);
                std::vector<mapped_page> released{};
                {
                    const std::unique_lock lock(state_mutex_);
                    for (uint64_t current = address; current < address + size; current += page_size)
                    {
                        auto* host = shared + (current - address);
                        const auto existing = pages_.find(current);
                        if (existing != pages_.end() && permissions && existing->second.host == host && !existing->second.owned)
                        {
                            existing->second.permissions = *permissions;
                            set_translation(current, user_translation(current, existing->second));
                            continue;
                        }
                        if (existing != pages_.end())
                        {
                            clear_translation(current);
                            released.push_back(std::move(pages_.extract(existing).mapped()));
                        }
                        if (permissions)
                        {
                            mapped_page page{.host = host, .gpa = acquire_host_page(host), .permissions = *permissions};
                            auto& inserted = pages_.emplace(current, std::move(page)).first->second;
                            set_translation(current, user_translation(current, inserted));
                        }
                    }
                    publish_translations();
                    for (const auto& page : released)
                    {
                        release_page(page);
                    }
                }
            }

            void copy_host_memory(const uint64_t address, const size_t size, const memory_permission permissions)
            {
                {
                    const std::unique_lock lock(state_mutex_);
                    map_pages(address, size, nullptr, memory_permission::read_write);
                }
                write_memory(address, reinterpret_cast<const void*>(address), size);
                apply_memory_protection(address, size, permissions);
            }

            void map_vm_mmio(const uint64_t address, const size_t size, mmio_read_callback read_cb, mmio_write_callback write_cb)
            {
                map_mmio(address, size, std::move(read_cb), std::move(write_cb));
            }

            void unmap(const uint64_t address, const size_t size)
            {
                unmap_memory(address, size);
            }

            bool is_mapped(const uint64_t address) const
            {
                const std::shared_lock lock(state_mutex_);
                return pages_.contains(address & ~(page_size - 1));
            }

            void set_el0_exception_handler(hvf::el0_exception_handler handler)
            {
                el0_exception_handler_ = std::move(handler);
            }

          private:
            friend class hvf_vcpu;

            struct syscall_hook
            {
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

            // Host memory outlives the VM (declared first, destroyed last); the vCPUs (declared last) are destroyed
            // before the VM.
            std::vector<std::unique_ptr<host_allocation>> pool_chunks_{};
            std::unique_ptr<host_allocation> table_pool_{};
            std::unique_ptr<host_allocation> vector_page_{};
            vm_handle vm_{};
            size_t breakpoint_count_{};
            size_t watchpoint_count_{};
            // Guards everything below. Exits of different vCPUs inspect it concurrently, while the kernel lock
            // serializes the callers that change memory and hooks. Callbacks never run with it held.
            mutable std::shared_mutex state_mutex_{};
            std::mutex mmio_mutex_{};
            std::map<uint64_t, mapped_page> pages_{};
            std::map<std::pair<unsigned, uint64_t>, translation_table> translation_tables_{};
            uint64_t next_table_offset_{};
            std::vector<uint64_t> free_table_offsets_{};
            std::vector<uint64_t> retired_table_offsets_{};
            bool translations_revoked_{};
            uint64_t next_gpa_{gpa_pool_base};
            std::vector<uint64_t> free_block_gpas_{};
            std::vector<owned_slot> free_slots_{};
            std::vector<owned_slot> dirty_slots_{};
            std::map<uintptr_t, host_block> host_blocks_{};
            std::map<uint64_t, std::shared_ptr<mmio_region>> mmio_regions_{};
            std::atomic_bool has_mmio_{};
            std::unique_ptr<syscall_hook> syscall_hook_{};
            std::unique_ptr<syscall_hook> counter_hook_{};
            std::vector<std::unique_ptr<interrupt_hook>> interrupt_hooks_{};
            std::vector<std::unique_ptr<violation_hook>> violation_hooks_{};
            std::vector<std::unique_ptr<execution_hook>> execution_hooks_{};
            std::vector<std::unique_ptr<memory_access_hook_callback>> access_hooks_{};
            std::multiset<uint64_t> stepped_pages_{};
            // Bumped whenever a valid descriptor changes. A vCPU that last entered at an older generation may hold
            // stale translations and invalidates its TLB on the next entry; descriptors that were invalid are never
            // cached, so new mappings need no invalidation.
            std::atomic<uint64_t> translation_generation_{};
            // While a vCPU single-steps a hooked instruction its page is executable, so its peers stay out of the
            // guest until the step ends; otherwise they could run the instruction without the hook.
            std::mutex step_mutex_{};
            std::condition_variable step_condition_{};
            std::atomic<const hvf_vcpu*> stepping_vcpu_{};
            bool destroying_{};
            hvf::el0_exception_handler el0_exception_handler_{};
            std::vector<std::unique_ptr<hvf_vcpu>> vcpus_{};

            uint64_t translation_root() const
            {
                return translation_tables_.at({0, 0}).gpa;
            }

            std::optional<instruction_hook_callback> syscall_callback() const
            {
                const std::shared_lock lock(state_mutex_);
                return syscall_hook_ ? std::optional{syscall_hook_->callback} : std::nullopt;
            }

            std::optional<instruction_hook_callback> counter_callback() const
            {
                const std::shared_lock lock(state_mutex_);
                return counter_hook_ ? std::optional{counter_hook_->callback} : std::nullopt;
            }

            std::vector<interrupt_hook_callback> interrupt_callbacks() const
            {
                const std::shared_lock lock(state_mutex_);
                std::vector<interrupt_hook_callback> callbacks{};
                callbacks.reserve(interrupt_hooks_.size());
                for (const auto& hook : interrupt_hooks_)
                {
                    callbacks.push_back(hook->callback);
                }
                return callbacks;
            }

            bool has_interrupt_hooks() const
            {
                const std::shared_lock lock(state_mutex_);
                return !interrupt_hooks_.empty();
            }

            std::vector<memory_violation_hook_callback> violation_callbacks() const
            {
                const std::shared_lock lock(state_mutex_);
                std::vector<memory_violation_hook_callback> callbacks{};
                callbacks.reserve(violation_hooks_.size());
                for (const auto& hook : violation_hooks_)
                {
                    callbacks.push_back(hook->callback);
                }
                return callbacks;
            }

            std::vector<memory_execution_hook_callback> execution_callbacks(const uint64_t address) const
            {
                const std::shared_lock lock(state_mutex_);
                std::vector<memory_execution_hook_callback> callbacks{};
                for (const auto& hook : execution_hooks_)
                {
                    if (address >= hook->address && address - hook->address < hook->size)
                    {
                        callbacks.push_back(hook->callback);
                    }
                }
                return callbacks;
            }

            std::optional<page_state> inspect_page(const uint64_t address) const
            {
                const std::shared_lock lock(state_mutex_);
                const auto page = pages_.find(address);
                if (page == pages_.end())
                {
                    return std::nullopt;
                }
                return page_state{.permissions = page->second.permissions, .execution_hooked = has_execution_hook(address)};
            }

            bool is_execution_hooked(const uint64_t page) const
            {
                const std::shared_lock lock(state_mutex_);
                return has_execution_hook(page);
            }

            // A vCPU about to single-step a hooked instruction makes its page executable until the step ends.
            void begin_hooked_step(const uint64_t page)
            {
                const std::unique_lock lock(state_mutex_);
                stepped_pages_.insert(page);
                refresh_translation(page);
                publish_translations();
            }

            void end_hooked_step(const uint64_t page)
            {
                const std::unique_lock lock(state_mutex_);
                if (const auto entry = stepped_pages_.find(page); entry != stepped_pages_.end())
                {
                    stepped_pages_.erase(entry);
                }
                refresh_translation(page);
                publish_translations();
            }

            void pause_peers(const hvf_vcpu& self)
            {
                {
                    std::unique_lock lock(step_mutex_);
                    step_condition_.wait(lock, [&] { return stepping_vcpu_ == nullptr; });
                    stepping_vcpu_ = &self;
                }
                for (const auto& vcpu : vcpus_)
                {
                    if (vcpu.get() != &self)
                    {
                        vcpu->synchronize_translations();
                    }
                }
            }

            void resume_peers(const hvf_vcpu& self)
            {
                {
                    std::lock_guard lock(step_mutex_);
                    if (stepping_vcpu_ == &self)
                    {
                        stepping_vcpu_ = nullptr;
                    }
                }
                step_condition_.notify_all();
            }

            bool peer_is_stepping(const hvf_vcpu& self) const
            {
                const auto* stepping = stepping_vcpu_.load();
                return stepping != nullptr && stepping != &self;
            }

            void wait_for_peer_step(const hvf_vcpu& self)
            {
                std::unique_lock lock(step_mutex_);
                step_condition_.wait(lock, [&] { return !peer_is_stepping(self); });
            }

            void refresh_page(const uint64_t page)
            {
                const std::unique_lock lock(state_mutex_);
                refresh_translation(page);
                publish_translations();
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
                    if (next_table_offset_ >= page_table_size)
                    {
                        throw std::runtime_error("HVF ARM64 translation table space exhausted");
                    }
                    offset = next_table_offset_;
                    next_table_offset_ += page_size;
                }
                return {.entries = reinterpret_cast<uint64_t*>(table_pool_->memory + offset), .gpa = page_table_base + offset};
            }

            // Level n tables are keyed by the address bits above the range they map; the root is level 0.
            static std::pair<unsigned, uint64_t> table_key(const unsigned level, const uint64_t address)
            {
                return {level, level == 0 ? 0 : address >> (48 - 9 * level)};
            }

            static size_t table_index(const unsigned level, const uint64_t address)
            {
                return (address >> (39 - 9 * level)) & 0x1FF;
            }

            // Running vCPUs walk these tables concurrently, so every descriptor is replaced by one single-copy-atomic
            // store. A replacement that takes access away is published to them by publish_translations.
            void write_descriptor(uint64_t& slot, const uint64_t entry)
            {
                const auto previous = slot;
                std::atomic_ref{slot}.store(entry, std::memory_order_release);
                if ((previous & 1) != 0 && previous != entry)
                {
                    translation_generation_.fetch_add(1, std::memory_order_release);
                }
                if (revokes_access(previous, entry))
                {
                    translations_revoked_ = true;
                }
            }

            translation_table& leaf_table(const uint64_t address)
            {
                auto* table = &translation_tables_.at(table_key(0, address));
                for (unsigned level = 1; level <= 3; ++level)
                {
                    auto found = translation_tables_.find(table_key(level, address));
                    if (found == translation_tables_.end())
                    {
                        found = translation_tables_.emplace(table_key(level, address), allocate_table()).first;
                        write_descriptor(table->entries[table_index(level - 1, address)], found->second.gpa | 3);
                        ++table->used;
                    }
                    table = &found->second;
                }
                return *table;
            }

            void set_translation(const uint64_t address, const uint64_t entry)
            {
                if (!entry)
                {
                    clear_translation(address);
                    return;
                }
                auto& leaf = leaf_table(address);
                auto& slot = leaf.entries[table_index(3, address)];
                if (!slot)
                {
                    ++leaf.used;
                }
                write_descriptor(slot, entry);
            }

            // Tables left empty (all entries zero) return to the pool, so long-running guests that touch many
            // distinct regions do not exhaust it. A peer may still walk a detached table until the change is
            // published, so it is reused only afterwards.
            void clear_translation(const uint64_t address)
            {
                for (unsigned level = 3;; --level)
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
                    write_descriptor(slot, 0);
                    if (level == 0 || --table->second.used != 0)
                    {
                        return;
                    }
                    retired_table_offsets_.push_back(table->second.gpa - page_table_base);
                    translation_tables_.erase(table);
                }
            }

            // A vCPU entering after a change invalidates its TLB first, so a revoked translation only survives in
            // vCPUs running at the time of the change: they are forced out before the change takes effect for the
            // caller (the memory behind it may be reused right after). Translations that only grant more are not
            // forced; a vCPU that faults on a stale one retries instead (see handle_abort).
            void publish_translations()
            {
                if (!std::exchange(translations_revoked_, false))
                {
                    return;
                }
                for (const auto& vcpu : vcpus_)
                {
                    vcpu->synchronize_translations();
                }
                free_table_offsets_.insert(free_table_offsets_.end(), retired_table_offsets_.begin(), retired_table_offsets_.end());
                retired_table_offsets_.clear();
            }

            void map_translation(const uint64_t address, const bool user)
            {
                // Normal WB memory with AF and inner-shareable attributes.
                set_translation(address, address | 3 | (1ull << 10) | (3ull << 8) | (user ? (1ull << 6) : (1ull << 54)));
            }

            emulator_hook* add_access_hook(memory_access_hook_callback callback)
            {
                const std::unique_lock lock(state_mutex_);
                auto& hook = access_hooks_.emplace_back(std::make_unique<memory_access_hook_callback>(std::move(callback)));
                return reinterpret_cast<emulator_hook*>(hook.get());
            }

            bool has_execution_hook(const uint64_t page) const
            {
                return std::ranges::any_of(execution_hooks_, [&](const std::unique_ptr<execution_hook>& hook) {
                    return hook->address < page + page_size && page < hook->address + hook->size;
                });
            }

            // Stage 1 carries the EL0 permissions (stage 2 maps everything), so every user fault is a
            // synchronous abort with ESR/FAR.
            uint64_t user_translation(const uint64_t address, const mapped_page& page) const
            {
                const auto readable = is_readable(page.permissions) || is_writable(page.permissions);
                const auto executable = is_executable(page.permissions);
                if (!readable && !executable)
                {
                    return 0;
                }
                auto entry = page.gpa | 3 | (1ull << 10) | (3ull << 8) | (1ull << 53);
                if (readable)
                {
                    entry |= 1ull << 6;
                    if (!is_writable(page.permissions))
                    {
                        entry |= 1ull << 7;
                    }
                }
                if (!executable || (!stepped_pages_.contains(address) && has_execution_hook(address)))
                {
                    entry |= 1ull << 54;
                }
                return entry;
            }

            void refresh_translation(const uint64_t address)
            {
                const auto page = pages_.find(address);
                if (page != pages_.end())
                {
                    set_translation(address, user_translation(address, page->second));
                }
            }

            void refresh_translations(const uint64_t address, const uint64_t size)
            {
                const auto first = address & ~(page_size - 1);
                const auto end = address + size;
                for (auto page = pages_.lower_bound(first); page != pages_.end() && page->first < end; ++page)
                {
                    set_translation(page->first, user_translation(page->first, page->second));
                }
            }

            // A vCPU entering while a peer refreshes the pages skips its own refresh: the pages are brought up to
            // date as it enters either way.
            void refresh_mmio()
            {
                if (!has_mmio_.load(std::memory_order_relaxed))
                {
                    return;
                }
                const std::unique_lock writer(mmio_mutex_, std::try_to_lock);
                if (!writer.owns_lock())
                {
                    return;
                }
                const std::shared_lock lock(state_mutex_);
                for (const auto& region : mmio_regions_ | std::views::values)
                {
                    for (size_t offset = 0; offset < region->size; offset += page_size)
                    {
                        region->read(offset, pages_.at(region->address + offset).host, page_size);
                    }
                }
            }

            uint64_t allocate_gpa_range(const uint64_t size)
            {
                if (next_gpa_ > vm_.gpa_limit || size > vm_.gpa_limit - next_gpa_)
                {
                    throw std::runtime_error("HVF ARM64 guest physical address space is exhausted");
                }
                const auto gpa = next_gpa_;
                next_gpa_ += size;
                return gpa;
            }

            // Owned guest pages come from large chunks mapped once; a reused slot is zeroed, as new memory is.
            owned_slot allocate_owned_slot()
            {
                if (!free_slots_.empty())
                {
                    const auto slot = free_slots_.back();
                    free_slots_.pop_back();
                    return slot;
                }
                if (!dirty_slots_.empty())
                {
                    const auto slot = dirty_slots_.back();
                    dirty_slots_.pop_back();
                    std::memset(slot.host, 0, page_size);
                    return slot;
                }
                auto chunk = std::make_unique<host_allocation>(pool_chunk_size);
                const auto gpa = allocate_gpa_range(pool_chunk_size);
                check_hv(hv_vm_map(chunk->memory, gpa, pool_chunk_size, all_access), "hv_vm_map(pool)");
                for (uint64_t offset = pool_chunk_size; offset > page_size; offset -= page_size)
                {
                    free_slots_.push_back({.host = chunk->memory + offset - page_size, .gpa = gpa + offset - page_size});
                }
                const owned_slot slot{.host = chunk->memory, .gpa = gpa};
                pool_chunks_.push_back(std::move(chunk));
                return slot;
            }

            // Host-provided pages are mapped through the 16 KiB host page that contains them. The whole host page
            // is valid process memory, and stage 1 only exposes the guest page itself.
            uint64_t acquire_host_page(std::byte* host)
            {
                const auto address = reinterpret_cast<uintptr_t>(host);
                const auto block_address = address & ~static_cast<uintptr_t>(block_size - 1);
                auto block = host_blocks_.find(block_address);
                if (block == host_blocks_.end())
                {
                    if (!is_plain_data(block_address))
                    {
                        throw std::invalid_argument(
                            "HVF ARM64 host backing must be data memory: its 16 KiB host page holds executable memory or is unmapped");
                    }
                    uint64_t gpa{};
                    if (!free_block_gpas_.empty())
                    {
                        gpa = free_block_gpas_.back();
                        free_block_gpas_.pop_back();
                    }
                    else
                    {
                        gpa = allocate_gpa_range(block_size);
                    }
                    const auto result = hv_vm_map(reinterpret_cast<void*>(block_address), gpa, block_size, all_access);
                    if (result != HV_SUCCESS)
                    {
                        free_block_gpas_.push_back(gpa);
                        throw_hv(result, "hv_vm_map(host page)");
                    }
                    block = host_blocks_.emplace(block_address, host_block{.gpa = gpa}).first;
                }
                ++block->second.references;
                return block->second.gpa + (address - block_address);
            }

            void release_page(const mapped_page& page)
            {
                if (page.owned)
                {
                    dirty_slots_.push_back({.host = page.host, .gpa = page.gpa});
                    return;
                }
                const auto block_address = reinterpret_cast<uintptr_t>(page.host) & ~static_cast<uintptr_t>(block_size - 1);
                const auto block = host_blocks_.find(block_address);
                if (block == host_blocks_.end() || --block->second.references != 0)
                {
                    return;
                }
                const auto result = hv_vm_unmap(block->second.gpa, block_size);
                if (result != HV_SUCCESS)
                {
                    // Keep the guest physical block reserved; its host page stays mapped until the VM is destroyed.
                    log_cleanup_failure(result, "hv_vm_unmap(host page)");
                }
                else
                {
                    free_block_gpas_.push_back(block->second.gpa);
                }
                host_blocks_.erase(block);
            }

            static void validate_range(const uint64_t address, const size_t size)
            {
                if (!size || address % page_size != 0 || size % page_size != 0 || address >= user_address_limit ||
                    size > user_address_limit - address)
                {
                    throw std::invalid_argument("HVF ARM64 mappings must be nonempty, page-aligned, and within the 48-bit address space");
                }
                const auto end = address + size;
                if (address < exit_gpa + block_size && end > vector_gpa)
                {
                    throw std::invalid_argument("HVF ARM64 mapping overlaps the private exception region");
                }
            }

            void map_memory(const uint64_t address, const size_t size, const memory_permission permissions) override
            {
                const std::unique_lock lock(state_mutex_);
                map_pages(address, size, nullptr, permissions);
            }

            bool supports_host_memory_mapping() const override
            {
                return true;
            }

            bool map_host_memory(const uint64_t address, const size_t size, void* host, const memory_permission permissions) override
            {
                if (!host || reinterpret_cast<uintptr_t>(host) % page_size != 0)
                {
                    throw std::invalid_argument("HVF ARM64 host backing must be nonnull and page-aligned");
                }
                const std::unique_lock lock(state_mutex_);
                map_pages(address, size, static_cast<std::byte*>(host), permissions);
                return true;
            }

            void map_pages(const uint64_t address, const size_t size, std::byte* host, const memory_permission permissions)
            {
                validate_range(address, size);
                const auto existing = pages_.lower_bound(address);
                if (existing != pages_.end() && existing->first < address + size)
                {
                    throw std::invalid_argument("HVF ARM64 guest mapping overlaps an existing page");
                }
                std::map<uint64_t, mapped_page> pending{};
                auto rollback = utils::finally([&] {
                    for (const auto& [page_address, page] : pending)
                    {
                        clear_translation(page_address);
                    }
                    publish_translations();
                    for (const auto& page : pending | std::views::values)
                    {
                        release_page(page);
                    }
                });
                for (size_t offset = 0; offset < size; offset += page_size)
                {
                    mapped_page page{.permissions = permissions};
                    if (host)
                    {
                        page.host = host + offset;
                        page.gpa = acquire_host_page(page.host);
                    }
                    else
                    {
                        const auto slot = allocate_owned_slot();
                        page.host = slot.host;
                        page.gpa = slot.gpa;
                        page.owned = true;
                    }
                    if (is_executable(permissions))
                    {
                        flush_instruction_cache(page.host, page_size);
                    }
                    auto& inserted = pending.emplace(address + offset, std::move(page)).first->second;
                    set_translation(address + offset, user_translation(address + offset, inserted));
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
                        throw std::invalid_argument("HVF ARM64 range contains an unmapped page");
                    }
                }
            }

            void unmap_memory(const uint64_t address, const size_t size) override
            {
                const std::unique_lock lock(state_mutex_);
                require_mapped_range(address, size);
                std::vector<mapped_page> released{};
                for (uint64_t current = address; current < address + size; current += page_size)
                {
                    const auto page = pages_.extract(current);
                    clear_translation(current);
                    if (page.mapped().mmio)
                    {
                        mmio_regions_.erase(page.mapped().mmio->address);
                        has_mmio_ = !mmio_regions_.empty();
                    }
                    released.push_back(std::move(page.mapped()));
                }
                // The backing returns to the pools only once no vCPU can still reach it.
                publish_translations();
                for (const auto& page : released)
                {
                    release_page(page);
                }
            }

            void apply_memory_protection(const uint64_t address, const size_t size, const memory_permission permissions) override
            {
                const std::unique_lock lock(state_mutex_);
                require_mapped_range(address, size);
                for (uint64_t current = address; current < address + size; current += page_size)
                {
                    auto& page = pages_.at(current);
                    if (is_executable(permissions) && !is_executable(page.permissions))
                    {
                        flush_instruction_cache(page.host, page_size);
                    }
                    page.permissions = permissions;
                    set_translation(current, user_translation(current, page));
                }
                publish_translations();
            }

            // EL0 loads from an MMIO range read a read-only page that is refreshed from the read callback before
            // every guest entry, as on WHP ARM64. Host accesses go to the callbacks directly.
            void map_mmio(const uint64_t address, const size_t size, mmio_read_callback read_cb, mmio_write_callback write_cb) override
            {
                if (!read_cb)
                {
                    throw std::invalid_argument("HVF ARM64 MMIO requires a read callback");
                }
                const std::unique_lock lock(state_mutex_);
                map_pages(address, size, nullptr, memory_permission::read);
                auto region = std::make_shared<mmio_region>(
                    mmio_region{.address = address, .size = size, .read = std::move(read_cb), .write = std::move(write_cb)});
                for (uint64_t offset = 0; offset < size; offset += page_size)
                {
                    pages_.at(address + offset).mmio = region;
                }
                mmio_regions_.emplace(address, std::move(region));
                has_mmio_ = true;
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
                        if (is_executable(page.permissions))
                        {
                            flush_instruction_cache(host, count);
                        }
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

        hvf_vcpu::hvf_vcpu(hvf_arm64_emulator& machine, const size_t index, const bool own_thread)
            : machine_(machine),
              index_(index)
        {
            if (own_thread)
            {
                thread_ = std::make_unique<vcpu_thread>();
                owner_ = thread_->handle();
            }
            else
            {
                owner_ = pthread_self();
            }
            on_owner([this] { create(); });
        }

        hvf_vcpu::~hvf_vcpu()
        {
            if (!created_)
            {
                return;
            }
            const auto destroy = [this] { log_cleanup_failure(hv_vcpu_destroy(vcpu_), "hv_vcpu_destroy"); };
            if (thread_)
            {
                thread_->execute(destroy);
            }
            else if (pthread_equal(pthread_self(), owner_))
            {
                destroy();
            }
            else
            {
                std::fprintf(stderr, "HVF ARM64 emulator destroyed off its vCPU thread; the vCPU is released with the VM\n");
            }
        }

        memory_interface& hvf_vcpu::memory()
        {
            return machine_;
        }

        const memory_interface& hvf_vcpu::memory() const
        {
            return machine_;
        }

        void hvf_vcpu::create()
        {
            hv_vcpu_exit_t* exit{};
            check_hv(hv_vcpu_create(&vcpu_, &exit, nullptr), "hv_vcpu_create");
            exit_ = exit;
            created_ = true;
            check_hv(hv_vcpu_set_vtimer_mask(vcpu_, true), "hv_vcpu_set_vtimer_mask");
            const auto debug_features = get_system(HV_SYS_REG_ID_AA64DFR0_EL1);
            machine_.breakpoint_count_ = static_cast<size_t>(((debug_features >> 12) & 0xF) + 1);
            machine_.watchpoint_count_ = static_cast<size_t>(((debug_features >> 20) & 0xF) + 1);

            set_general(HV_REG_PC, 0);
            set_general(HV_REG_CPSR, 0x3C0);
            set_system(HV_SYS_REG_SP_EL1, exit_gpa);
            set_system(HV_SYS_REG_VBAR_EL1, vector_gpa);
            set_system(HV_SYS_REG_MAIR_EL1, 0xFF);
            set_system(HV_SYS_REG_TCR_EL1, 16ull | (1ull << 8) | (1ull << 10) | (3ull << 12) | (1ull << 23) | (5ull << 32));
            set_system(HV_SYS_REG_TTBR0_EL1, machine_.translation_root());
            set_system(HV_SYS_REG_SCTLR_EL1, 0x30D01805);
            set_system(HV_SYS_REG_CPACR_EL1, 3ull << 20);
            // EL0PCTEN keeps CNTFRQ_EL0 readable; CNTVCT_EL0 reads trap so the counter hook supplies them.
            set_system(HV_SYS_REG_CNTKCTL_EL1, 1);
        }

        void hvf_vcpu::run(const size_t count)
        {
            {
                std::lock_guard lock(run_mutex_);
                if (running_)
                {
                    throw std::logic_error("HVF ARM64 CPU is already running");
                }
                running_ = true;
                stop_requested_ = false;
                violation_ = false;
                stepping_ = count != 0;
            }
            auto finish = utils::finally([this] {
                disarm_hooked_step();
                std::lock_guard lock(run_mutex_);
                in_hypervisor_ = false;
                running_ = false;
                run_condition_.notify_all();
            });
            auto remaining = count;
            // A hooked instruction whose hooks already ran is stepped once with its page executable. If another
            // thread (TEB in X18) or PC resumes first, protect the page again so that context faults normally.
            if (stepped_page_ && (get_general(HV_REG_PC) != stepped_context_.first || get_general(HV_REG_X18) != stepped_context_.second))
            {
                finish_hooked_step();
            }

            for (;;)
            {
                if (!enter_guest())
                {
                    return;
                }
                const auto result = hv_vcpu_run(vcpu_);
                {
                    std::lock_guard lock(run_mutex_);
                    in_hypervisor_ = false;
                    ++entries_;
                }
                run_condition_.notify_all();
                check_hv(result, "hv_vcpu_run");
                if (exit_->reason == HV_EXIT_REASON_CANCELED)
                {
                    std::lock_guard lock(run_mutex_);
                    restore_user_view();
                    if (stop_requested_)
                    {
                        return;
                    }
                    // A cancellation that raced with an ordinary exit, or a peer's translation change, stays
                    // pending and cancels the next entry instead; nothing asked this run to stop, so resume.
                    continue;
                }
                if (exit_->reason == HV_EXIT_REASON_VTIMER_ACTIVATED)
                {
                    restore_user_view();
                    continue;
                }
                hook_only_exit_ = false;
                handle_exit();
                if (stepping_ && !hook_only_exit_ && --remaining == 0)
                {
                    return;
                }
            }
        }

        bool hvf_vcpu::enter_guest()
        {
            if (stepped_page_ && !step_armed_)
            {
                arm_hooked_step();
            }
            for (;;)
            {
                machine_.refresh_mmio();
                if (!step_armed_)
                {
                    machine_.wait_for_peer_step(*this);
                }
                std::lock_guard lock(run_mutex_);
                if (stop_requested_)
                {
                    return false;
                }
                // Checked together with in_hypervisor_, so a peer starting its step either sees this vCPU running
                // and forces it out, or this vCPU sees the step and waits.
                if (!step_armed_ && machine_.peer_is_stepping(*this))
                {
                    continue;
                }
                prepare_user_entry();
                in_hypervisor_ = true;
                return true;
            }
        }

        size_t hvf_vcpu::read_register_on_owner(const int reg, void* value, const size_t size) const
        {
            const auto mapping = map_register(static_cast<arm64_register>(reg));
            validate_register_buffer(value, size, mapping.size);
            std::array<uint64_t, 2> result{};
            if (mapping.kind != register_kind::zero && !absent_debug_register(mapping))
            {
                result = get_register(mapping);
                if (mapping.nzcv)
                {
                    result[0] &= nzcv_mask;
                }
            }
            std::memcpy(value, result.data(), mapping.size);
            return mapping.size;
        }

        size_t hvf_vcpu::write_register_on_owner(const int reg, const void* value, const size_t size) const
        {
            const auto mapping = map_register(static_cast<arm64_register>(reg));
            validate_register_buffer(value, size, mapping.size);
            if (is_identification_register(mapping))
            {
                throw std::invalid_argument("ARM64 identification registers are read-only");
            }
            if (absent_debug_register(mapping))
            {
                const auto* bytes = static_cast<const std::byte*>(value);
                if (std::any_of(bytes, bytes + mapping.size, [](const auto byte) { return byte != std::byte{}; }))
                {
                    throw std::invalid_argument("ARM64 context uses a debug register not implemented by the virtual processor");
                }
                return mapping.size;
            }
            if (mapping.kind == register_kind::zero)
            {
                return mapping.size;
            }
            std::array<uint64_t, 2> result{};
            std::memcpy(result.data(), value, mapping.size);
            if (mapping.nzcv)
            {
                result[0] = (get_general(HV_REG_CPSR) & ~nzcv_mask) | (result[0] & nzcv_mask);
            }
            else if (mapping.kind == register_kind::general && mapping.id == HV_REG_CPSR && (result[0] & 0x1F) != 0)
            {
                throw std::invalid_argument("HVF ARM64 only exposes an AArch64 EL0 CPU");
            }
            set_register(mapping, result);
            return mapping.size;
        }

        std::vector<std::byte> hvf_vcpu::save_registers_on_owner() const
        {
            std::array<std::array<uint64_t, 2>, saved_registers.size()> values{};
            for (size_t i = 0; i < values.size(); ++i)
            {
                if (!absent_debug_register(saved_registers[i]))
                {
                    values[i] = get_register(saved_registers[i]);
                }
            }
            std::vector<std::byte> bytes(sizeof(values));
            std::memcpy(bytes.data(), values.data(), bytes.size());
            return bytes;
        }

        void hvf_vcpu::restore_registers_on_owner(const std::vector<std::byte>& bytes) const
        {
            std::array<std::array<uint64_t, 2>, saved_registers.size()> values{};
            if (bytes.size() != sizeof(values))
            {
                throw std::invalid_argument("Invalid HVF ARM64 saved register size");
            }
            std::memcpy(values.data(), bytes.data(), bytes.size());
            if ((values[saved_pstate_index][0] & 0x1F) != 0)
            {
                throw std::invalid_argument("Saved HVF ARM64 CPU is not AArch64 EL0");
            }
            for (size_t i = 0; i < values.size(); ++i)
            {
                if (absent_debug_register(saved_registers[i]) && (values[i][0] != 0 || values[i][1] != 0))
                {
                    throw std::invalid_argument("Saved ARM64 registers exceed this virtual processor's debug capabilities");
                }
            }
            for (size_t i = 0; i < values.size(); ++i)
            {
                if (!absent_debug_register(saved_registers[i]))
                {
                    set_register(saved_registers[i], values[i]);
                }
            }
        }

        bool hvf_vcpu::absent_debug_register(const register_mapping& mapping) const
        {
            if (mapping.kind != register_kind::system)
            {
                return false;
            }
            // ARM64_NT_CONTEXT contains fixed maximum slots; absent hardware slots are inactive.
            for (const auto first : {HV_SYS_REG_DBGBCR0_EL1, HV_SYS_REG_DBGBVR0_EL1})
            {
                for (size_t index = 0; index < 16; ++index)
                {
                    if (mapping.id == debug_register(first, static_cast<int>(index)))
                    {
                        return index >= machine_.breakpoint_count_;
                    }
                }
            }
            for (const auto first : {HV_SYS_REG_DBGWCR0_EL1, HV_SYS_REG_DBGWVR0_EL1})
            {
                for (size_t index = 0; index < 16; ++index)
                {
                    if (mapping.id == debug_register(first, static_cast<int>(index)))
                    {
                        return index >= machine_.watchpoint_count_;
                    }
                }
            }
            return false;
        }

        // A pending hooked step makes its page executable only while this vCPU runs and its peers are paused.
        void hvf_vcpu::arm_hooked_step()
        {
            if (machine_.vcpus_.size() > 1)
            {
                machine_.pause_peers(*this);
            }
            machine_.begin_hooked_step(*stepped_page_);
            step_armed_ = true;
        }

        void hvf_vcpu::disarm_hooked_step()
        {
            if (!std::exchange(step_armed_, false))
            {
                return;
            }
            machine_.end_hooked_step(*stepped_page_);
            if (machine_.vcpus_.size() > 1)
            {
                machine_.resume_peers(*this);
            }
        }

        void hvf_vcpu::finish_hooked_step()
        {
            disarm_hooked_step();
            stepped_page_.reset();
        }

        void hvf_vcpu::prepare_user_entry()
        {
            const auto pstate = get_general(HV_REG_CPSR);
            user_step_ = (pstate & software_step_bit) != 0;
            auto user_pstate = pstate & ~software_step_bit;
            const auto step = stepping_ || stepped_page_.has_value() || user_step_;
            set_system(HV_SYS_REG_MDSCR_EL1, step ? (1ull << 15) | 1 : 0);
            if (step)
            {
                user_pstate = (user_pstate & ~(1ull << 9)) | software_step_bit;
            }
            set_system(HV_SYS_REG_ELR_EL1, get_general(HV_REG_PC));
            set_system(HV_SYS_REG_SPSR_EL1, user_pstate);
            // The invalidation is broadcast: an entry flushes the stale translations of every core this VM ran on,
            // whichever core this vCPU's host thread migrates to.
            const auto generation = machine_.translation_generation_.load(std::memory_order_acquire);
            const auto flush = flushed_generation_ != generation;
            flushed_generation_ = generation;
            set_general(HV_REG_PC, flush ? resume_gpa : resume_without_flush_gpa);
            set_general(HV_REG_CPSR, 0x3C5);
        }

        void hvf_vcpu::restore_user_view()
        {
            const auto pc = get_general(HV_REG_PC);
            if (pc >= vector_gpa && pc < vector_gpa + page_size && (get_general(HV_REG_CPSR) & 0x1F) == 5)
            {
                auto return_pc = get_system(HV_SYS_REG_ELR_EL1);
                if (pc < resume_gpa && ((get_system(HV_SYS_REG_ESR_EL1) >> 26) & 0x3F) == svc_class && return_pc >= 4)
                {
                    return_pc -= 4;
                }
                set_general(HV_REG_PC, return_pc);
                // Canceled before the ERET: a requested user step has not run yet, and neither may the invalidation.
                if (pc >= resume_gpa)
                {
                    flushed_generation_.reset();
                }
                const auto pending_step = pc >= resume_gpa && user_step_ ? software_step_bit : 0;
                set_general(HV_REG_CPSR, (get_system(HV_SYS_REG_SPSR_EL1) & ~software_step_bit) | pending_step);
            }
            else if (!user_step_)
            {
                // Canceled at EL0 after the ERET: PSTATE.SS may still hold the backend's own step request.
                const auto pstate = get_general(HV_REG_CPSR);
                if (pstate & software_step_bit)
                {
                    set_general(HV_REG_CPSR, pstate & ~software_step_bit);
                }
            }
        }

        void hvf_vcpu::handle_exit()
        {
            const auto repeated_repair = std::exchange(fault_repaired_, false);
            const auto pc = get_general(HV_REG_PC);
            const auto syndrome = exit_->exception.syndrome;
            if (exit_->reason == HV_EXIT_REASON_EXCEPTION && ((syndrome >> 26) & 0x3F) == system_register_trap_class &&
                (get_general(HV_REG_CPSR) & 0x1F) == 0)
            {
                handle_el2_system_register_trap(syndrome, pc);
                return;
            }
            const bool vector_exit = exit_->reason == HV_EXIT_REASON_EXCEPTION && ((syndrome >> 26) & 0x3F) == data_abort_class &&
                                     exit_->exception.physical_address == exit_gpa && pc >= vector_gpa && pc < vector_gpa + 0x800 &&
                                     (pc - vector_gpa) % 0x80 == 4 && (get_general(HV_REG_CPSR) & 0x1F) == 5;
            if (!vector_exit)
            {
                violation_ = true;
                std::ostringstream error{};
                error << "Unhandled HVF ARM64 exit " << static_cast<uint32_t>(exit_->reason) << " at PC 0x" << std::hex << pc
                      << ", ESR_EL2=0x" << syndrome << ", IPA 0x" << exit_->exception.physical_address;
                throw std::runtime_error(error.str());
            }

            const auto esr = get_system(HV_SYS_REG_ESR_EL1);
            const auto return_pc = get_system(HV_SYS_REG_ELR_EL1);
            const auto user_pstate = get_system(HV_SYS_REG_SPSR_EL1) & ~software_step_bit;
            const auto exception_class = static_cast<int>((esr >> 26) & 0x3F);
            const auto from_user = pc == vector_gpa + user_vector_offset && (user_pstate & 0x1F) == 0;
            // Any exit consumes a user step; only a completed step is reported.
            const auto user_step = std::exchange(user_step_, false);
            const auto hooked_step = stepped_page_.has_value();
            if (hooked_step)
            {
                // The hooked instruction has run (or raised its own exception); protect its page again.
                finish_hooked_step();
            }
            if (from_user && machine_.el0_exception_handler_)
            {
                set_general(HV_REG_CPSR, user_pstate);
                set_general(HV_REG_PC, return_pc);
                if (machine_.el0_exception_handler_(
                        *this, {.exception_class = exception_class, .syndrome = esr, .fault_address = get_system(HV_SYS_REG_FAR_EL1)}))
                {
                    return;
                }
            }
            if (from_user && exception_class == software_step_class && (stepping_ || hooked_step || user_step))
            {
                set_general(HV_REG_CPSR, user_pstate);
                set_general(HV_REG_PC, return_pc);
                if (user_step)
                {
                    if (!machine_.has_interrupt_hooks())
                    {
                        throw std::runtime_error("HVF ARM64 software step without an interrupt hook");
                    }
                    report_exception(static_cast<int>(arm64_exception_class::software_step));
                }
                return;
            }
            if (from_user && (exception_class == instruction_abort_class || exception_class == data_abort_class) &&
                handle_abort(exception_class, esr, return_pc, user_pstate, user_step, repeated_repair))
            {
                return;
            }
            if (from_user &&
                (exception_class == system_register_trap_class ||
                 exception_class == static_cast<int>(arm64_exception_class::undefined_instruction)) &&
                handle_counter_read(return_pc, user_pstate, user_step))
            {
                return;
            }
            if (from_user && machine_.has_interrupt_hooks() &&
                (exception_class == static_cast<int>(arm64_exception_class::undefined_instruction) ||
                 exception_class == static_cast<int>(arm64_exception_class::breakpoint_instruction)))
            {
                // ELR_EL1 holds the faulting instruction for both classes.
                set_general(HV_REG_CPSR, user_pstate);
                set_general(HV_REG_PC, return_pc);
                report_exception(exception_class);
                return;
            }
            if (!from_user || exception_class != svc_class || return_pc < 4)
            {
                violation_ = true;
                std::ostringstream error{};
                error << "Unhandled HVF ARM64 guest exception: ESR_EL1=0x" << std::hex << esr << ", ELR_EL1=0x" << return_pc
                      << ", FAR_EL1=0x" << get_system(HV_SYS_REG_FAR_EL1);
                restore_user_view();
                throw std::runtime_error(error.str());
            }
            uint32_t instruction{};
            machine_.read_memory(return_pc - 4, &instruction, sizeof(instruction));
            if ((instruction & 0xFFE0001F) != 0xD4000001 || ((instruction >> 5) & 0xFFFF) != (esr & 0xFFFF))
            {
                violation_ = true;
                restore_user_view();
                std::ostringstream error{};
                error << "HVF ARM64 SVC instruction does not match ESR_EL1: instruction=0x" << std::hex << instruction << ", ESR_EL1=0x"
                      << esr << ", ELR_EL1=0x" << return_pc;
                throw std::runtime_error(error.str());
            }

            // Hooks see the SVC address in PC, as x86 hooks see SYSCALL in RIP. Restore EL0
            // before invoking the callback so stop/save/register access never exposes EL1.
            set_general(HV_REG_CPSR, user_pstate);
            set_general(HV_REG_PC, return_pc - 4);
            const auto callback = machine_.syscall_callback();
            if (!callback)
            {
                throw std::runtime_error("HVF ARM64 guest issued SVC without a syscall hook");
            }
            const auto continuation = (*callback)(*this, (instruction >> 5) & 0xFFFF);
            switch (continuation)
            {
            case instruction_hook_continuation::skip_instruction:
                set_general(HV_REG_PC, get_general(HV_REG_PC) + 4);
                return;
            case instruction_hook_continuation::finalized_instruction_pointer:
                return;
            case instruction_hook_continuation::run_instruction:
                throw std::runtime_error("HVF ARM64 SVC hooks must handle the syscall (skip or finalized PC)");
            default:
                throw std::runtime_error("Invalid HVF ARM64 SVC hook continuation");
            }
        }

        // Hypervisor.framework traps some EL0 system register accesses (the PMU cycle counter) to EL2, so they
        // reach the host with the vCPU still in the user's state instead of through the EL1 vectors.
        void hvf_vcpu::handle_el2_system_register_trap(const uint64_t syndrome, const uint64_t pc)
        {
            const auto user_step = std::exchange(user_step_, false);
            if (stepped_page_)
            {
                finish_hooked_step();
            }
            set_general(HV_REG_CPSR, get_general(HV_REG_CPSR) & ~software_step_bit);

            // ISS: Op0[21:20] Op2[19:17] Op1[16:14] CRn[13:10] Rt[9:5] CRm[4:1] Direction[0] (1 = read).
            const auto iss = static_cast<uint32_t>(syndrome & 0x1FFFFFF);
            const auto key = iss & 0x3FFC1E;
            constexpr auto encode = [](const uint32_t op0, const uint32_t op1, const uint32_t crn, const uint32_t crm, const uint32_t op2) {
                return (op0 << 20) | (op2 << 17) | (op1 << 14) | (crn << 10) | (crm << 1);
            };
            const auto counter_read =
                (iss & 1) != 0 && (key == encode(3, 3, 9, 13, 0) || key == encode(3, 3, 14, 0, 2) || key == encode(3, 3, 14, 0, 6));
            const auto callback = counter_read ? machine_.counter_callback() : std::nullopt;
            if (callback)
            {
                switch ((*callback)(*this, static_cast<uint64_t>(general_register((iss >> 5) & 0x1F))))
                {
                case instruction_hook_continuation::skip_instruction:
                    set_general(HV_REG_PC, get_general(HV_REG_PC) + 4);
                    break;
                case instruction_hook_continuation::finalized_instruction_pointer:
                    break;
                default:
                    throw std::runtime_error("HVF ARM64 counter hooks must supply the value (skip or finalized PC)");
                }
                if (user_step && machine_.has_interrupt_hooks())
                {
                    report_exception(static_cast<int>(arm64_exception_class::software_step));
                }
                return;
            }
            if (!machine_.has_interrupt_hooks())
            {
                violation_ = true;
                std::ostringstream error{};
                error << "Unhandled HVF ARM64 system register trap: ESR_EL2=0x" << std::hex << syndrome << " at PC 0x" << pc;
                throw std::runtime_error(error.str());
            }
            // EL0 cannot access the register on Windows either; the instruction is undefined there.
            report_exception(static_cast<int>(arm64_exception_class::undefined_instruction));
        }

        void hvf_vcpu::report_exception(const int exception_class)
        {
            for (const auto& callback : machine_.interrupt_callbacks())
            {
                callback(*this, exception_class);
            }
        }

        bool hvf_vcpu::handle_abort(const int exception_class, const uint64_t esr, const uint64_t return_pc, const uint64_t user_pstate,
                                    const bool user_step, const bool repeated_repair)
        {
            const auto status = esr & 0x3C;
            const auto translation_fault = status == 0x04;
            const auto permission_fault = status == 0x0C;
            if (!translation_fault && !permission_fault)
            {
                return false;
            }

            const auto instruction_abort = exception_class == instruction_abort_class;
            const auto address = get_system(HV_SYS_REG_FAR_EL1);
            const auto page_address = address & ~(page_size - 1);
            const auto page = machine_.inspect_page(page_address);
            const auto hooked_execution =
                instruction_abort && permission_fault && page && is_executable(page->permissions) && page->execution_hooked;

            auto operation = memory_operation::exec;
            size_t size = sizeof(uint32_t);
            if (!instruction_abort)
            {
                operation = ((esr >> 6) & 1) != 0 ? memory_operation::write : memory_operation::read;
                size = ((esr >> 24) & 1) != 0 ? size_t{1} << ((esr >> 22) & 3) : 0;
            }

            // A peer may have just mapped the page or granted the access while this vCPU still held the old
            // translation. Re-entering flushes the TLB, so the instruction is retried; a fault that persists at
            // the same instruction is genuine.
            if (!hooked_execution && page && machine_.vcpus_.size() > 1)
            {
                if (permits(page->permissions, operation))
                {
                    if (!repeated_repair || repaired_page_ != page_address || repaired_pc_ != return_pc)
                    {
                        repaired_page_ = page_address;
                        repaired_pc_ = return_pc;
                        repair_count_ = 0;
                    }
                    if (++repair_count_ <= maximum_fault_repairs)
                    {
                        fault_repaired_ = true;
                        flushed_generation_.reset();
                        set_general(HV_REG_PC, return_pc);
                        set_general(HV_REG_CPSR, user_pstate | (user_step ? software_step_bit : 0));
                        hook_only_exit_ = true;
                        return true;
                    }
                }
            }

            const auto violation_callbacks =
                hooked_execution ? std::vector<memory_violation_hook_callback>{} : machine_.violation_callbacks();
            if (!hooked_execution && violation_callbacks.empty())
            {
                return false;
            }

            set_general(HV_REG_PC, return_pc);

            if (hooked_execution)
            {
                // The hooked instruction has not run yet, so a requested user step is still pending.
                set_general(HV_REG_CPSR, user_pstate | (user_step ? software_step_bit : 0));
                for (const auto& callback : machine_.execution_callbacks(return_pc))
                {
                    callback(*this, return_pc);
                }
                if (get_general(HV_REG_PC) == return_pc && machine_.is_execution_hooked(page_address))
                {
                    stepped_page_ = page_address;
                    stepped_context_ = {return_pc, get_general(HV_REG_X18)};
                    arm_hooked_step();
                }
                else
                {
                    machine_.refresh_page(page_address);
                }
                // No guest instruction completed, so the exit does not consume a step.
                hook_only_exit_ = true;
                return true;
            }

            set_general(HV_REG_CPSR, user_pstate);

            const auto type = translation_fault ? memory_violation_type::unmapped : memory_violation_type::protection;
            for (const auto& callback : violation_callbacks)
            {
                if (callback(*this, address, size, operation, type) == memory_violation_continuation::stop)
                {
                    violation_ = true;
                    std::ostringstream error{};
                    error << "HVF ARM64 memory violation at 0x" << std::hex << address << " was not handled (PC 0x" << return_pc << ")";
                    throw std::runtime_error(error.str());
                }
            }
            return true;
        }

        bool hvf_vcpu::handle_counter_read(const uint64_t return_pc, const uint64_t user_pstate, const bool user_step)
        {
            uint32_t instruction{};
            const auto callback = machine_.counter_callback();
            if (!callback || !machine_.try_read_memory(return_pc, &instruction, sizeof(instruction)))
            {
                return false;
            }
            const auto encoding = instruction & ~0x1Fu;
            if (encoding != mrs_virtual_counter && encoding != mrs_self_synchronized_virtual_counter && encoding != mrs_cycle_counter)
            {
                return false;
            }

            set_general(HV_REG_CPSR, user_pstate);
            set_general(HV_REG_PC, return_pc);
            switch ((*callback)(*this, static_cast<uint64_t>(general_register(instruction & 0x1F))))
            {
            case instruction_hook_continuation::skip_instruction:
                set_general(HV_REG_PC, get_general(HV_REG_PC) + 4);
                break;
            case instruction_hook_continuation::finalized_instruction_pointer:
                break;
            default:
                throw std::runtime_error("HVF ARM64 counter hooks must supply the value (skip or finalized PC)");
            }

            // The counter read completed the stepped instruction.
            if (user_step && machine_.has_interrupt_hooks())
            {
                report_exception(static_cast<int>(arm64_exception_class::software_step));
            }
            return true;
        }

        bool has_hypervisor_entitlement()
        {
            auto* task = SecTaskCreateFromSelf(kCFAllocatorDefault);
            if (!task)
            {
                return false;
            }
            const auto release_task = utils::finally([&] { CFRelease(task); });
            const auto* value = SecTaskCopyValueForEntitlement(task, CFSTR("com.apple.security.hypervisor"), nullptr);
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

    std::unique_ptr<arm64_emulator> create_arm64_emulator(const size_t vcpu_count)
    {
        return std::make_unique<hvf_arm64_emulator>(vcpu_count);
    }

    void share_host_memory(arm64_emulator& vm, const uint64_t address, const size_t size,
                           const std::optional<memory_permission> permissions)
    {
        dynamic_cast<hvf_arm64_emulator&>(vm).share_host_memory(address, size, reinterpret_cast<std::byte*>(address), permissions);
    }

    void share_host_memory(arm64_emulator& vm, const uint64_t address, const size_t size, void* host,
                           const std::optional<memory_permission> permissions)
    {
        dynamic_cast<hvf_arm64_emulator&>(vm).share_host_memory(address, size, static_cast<std::byte*>(host), permissions);
    }

    void copy_host_memory(arm64_emulator& vm, const uint64_t address, const size_t size, const memory_permission permissions)
    {
        dynamic_cast<hvf_arm64_emulator&>(vm).copy_host_memory(address, size, permissions);
    }

    void map_mmio(arm64_emulator& vm, const uint64_t address, const size_t size, mmio_read_callback read_cb, mmio_write_callback write_cb)
    {
        dynamic_cast<hvf_arm64_emulator&>(vm).map_vm_mmio(address, size, std::move(read_cb), std::move(write_cb));
    }

    void unmap(arm64_emulator& vm, const uint64_t address, const size_t size)
    {
        dynamic_cast<hvf_arm64_emulator&>(vm).unmap(address, size);
    }

    bool can_share_host_memory(const uint64_t address)
    {
        return is_plain_data(address & ~(block_size - 1));
    }

    bool is_mapped(arm64_emulator& vm, const uint64_t address)
    {
        return dynamic_cast<hvf_arm64_emulator&>(vm).is_mapped(address);
    }

    void set_el0_exception_handler(arm64_emulator& vm, el0_exception_handler handler)
    {
        dynamic_cast<hvf_arm64_emulator&>(vm).set_el0_exception_handler(std::move(handler));
    }
}
