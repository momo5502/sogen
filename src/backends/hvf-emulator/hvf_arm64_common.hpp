#pragma once

#include <array>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include <Hypervisor/Hypervisor.h>

#include <arch_emulator.hpp>
#include <hook_interface.hpp>
#include <memory_permission.hpp>

namespace sogen::hvf
{
    inline void hv_call(const hv_return_t result, const char* what)
    {
        if (result != HV_SUCCESS)
        {
            std::array<char, 11> code{};
            std::snprintf(code.data(), code.size(), "0x%08x", result);
            throw std::runtime_error(std::string(what) + " failed: " + code.data());
        }
    }

    // hv_vm_create() takes no handle and hv_vm_destroy() takes no argument: the VM belongs to the
    // process, and a second hv_vm_create() returns HV_BUSY. Emulator instances therefore share one VM
    // and partition the IPA space between themselves.
    class vm_reference
    {
      public:
        vm_reference()
        {
            const std::lock_guard lock{mutex()};
            if (references() == 0)
            {
                hv_call(hv_vm_create(nullptr), "hv_vm_create");
            }

            ++references();
        }

        ~vm_reference()
        {
            const std::lock_guard lock{mutex()};
            if (--references() == 0)
            {
                hv_vm_destroy();
            }
        }

        vm_reference(const vm_reference&) = delete;
        vm_reference& operator=(const vm_reference&) = delete;
        vm_reference(vm_reference&&) = delete;
        vm_reference& operator=(vm_reference&&) = delete;

      private:
        static std::mutex& mutex()
        {
            static std::mutex instance{};
            return instance;
        }

        static size_t& references()
        {
            static size_t count{};
            return count;
        }
    };

    constexpr uint64_t guest_page_size = 0x1000;
    constexpr uint64_t host_frame_size = 0x4000;
    constexpr uint64_t ipa_space_limit = 0x1000000000ULL;

    constexpr uint64_t pte_valid = 1ULL << 0;
    constexpr uint64_t pte_page = 1ULL << 1;
    constexpr uint64_t pte_attr_normal = 0ULL << 2;
    constexpr uint64_t pte_shareable = 3ULL << 8;
    constexpr uint64_t pte_access_flag = 1ULL << 10;
    constexpr uint64_t pte_pxn = 1ULL << 53;
    constexpr uint64_t pte_uxn = 1ULL << 54;
    constexpr uint64_t pte_address_mask = 0x0000FFFFFFFFF000ULL;

    constexpr uint64_t ap_el0_read_write = 1ULL << 6;
    constexpr uint64_t ap_el0_read_only = 3ULL << 6;

    // AP[2:1] has no write-without-read encoding, and an EL0 instruction fetch requires EL0 read
    // permission, so a write-only or execute-only request is widened rather than rejected.
    inline uint64_t leaf_attributes(const memory_permission permissions)
    {
        uint64_t attributes = pte_valid | pte_page | pte_attr_normal | pte_shareable | pte_access_flag | pte_pxn;
        attributes |= is_writable(permissions) ? ap_el0_read_write : ap_el0_read_only;

        if (!is_executable(permissions))
        {
            attributes |= pte_uxn;
        }

        return attributes;
    }

    constexpr uint32_t insn_ic_iallu = 0xD508751F;
    constexpr uint32_t insn_tlbi_vmalle1 = 0xD508871F;
    constexpr uint32_t insn_dsb_nsh = 0xD503379F;
    constexpr uint32_t insn_isb = 0xD5033FDF;
    constexpr uint32_t insn_eret = 0xD69F03E0;
    constexpr uint32_t insn_hvc_0 = 0xD4000002;
    constexpr uint32_t insn_brk_0 = 0xD4200000;

    constexpr uint32_t exception_class_svc = 0x15;
    constexpr uint32_t exception_class_hvc = 0x16;
    constexpr uint32_t exception_class_instruction_abort_lower = 0x20;
    constexpr uint32_t exception_class_data_abort_lower = 0x24;
    constexpr uint32_t exception_class_brk = 0x3C;
    constexpr uint32_t exception_class_software_step = 0x32;
    constexpr uint32_t exception_class_watchpoint = 0x34;

    // EL1h with DAIF masked, and the EL0 state the trampoline erets into.
    constexpr uint64_t cpsr_el1h = 0x3C5;
    constexpr uint64_t spsr_el0t = 0x3C0;
    constexpr uint64_t spsr_single_step = 1ULL << 21;

    constexpr uint64_t trapping_instruction_size = 4;

    // ID_AA64DFR0_EL1 on this silicon: 6 breakpoints, 4 watchpoints, 2 context comparators.
    constexpr size_t watchpoint_count = 4;

    // The watchpoint sysregs step by 8, not by 2: DBGWVR0 is 0x8006 and DBGWVR1 is 0x800e. Listed by
    // name rather than derived from the first, the same way the PAC keys had to be.
    inline constexpr std::array<hv_sys_reg_t, watchpoint_count> watchpoint_value_registers{HV_SYS_REG_DBGWVR0_EL1, HV_SYS_REG_DBGWVR1_EL1,
                                                                                           HV_SYS_REG_DBGWVR2_EL1, HV_SYS_REG_DBGWVR3_EL1};

    inline constexpr std::array<hv_sys_reg_t, watchpoint_count> watchpoint_control_registers{
        HV_SYS_REG_DBGWCR0_EL1, HV_SYS_REG_DBGWCR1_EL1, HV_SYS_REG_DBGWCR2_EL1, HV_SYS_REG_DBGWCR3_EL1};

    // Not a contiguous range, despite spanning 0xc108..0xc119: 0xc10c..0xc10f are not valid sysregs and
    // reading one returns HV_BAD_ARGUMENT. Verified against the framework header rather than assumed
    // from the endpoints.
    inline constexpr std::array<hv_sys_reg_t, 10> pac_key_registers{
        HV_SYS_REG_APIAKEYLO_EL1, HV_SYS_REG_APIAKEYHI_EL1, HV_SYS_REG_APIBKEYLO_EL1, HV_SYS_REG_APIBKEYHI_EL1, HV_SYS_REG_APDAKEYLO_EL1,
        HV_SYS_REG_APDAKEYHI_EL1, HV_SYS_REG_APDBKEYLO_EL1, HV_SYS_REG_APDBKEYHI_EL1, HV_SYS_REG_APGAKEYLO_EL1, HV_SYS_REG_APGAKEYHI_EL1,
    };

    struct execution_hook_entry
    {
        uint64_t address{};
        uint32_t original{};
        memory_execution_hook_callback callback{};
    };

    struct memory_access_hook_entry
    {
        uint64_t address{};
        size_t size{};
        bool is_write{};
        size_t watchpoint{};
        memory_access_hook_callback callback{};
    };

    // Registered for API compatibility and never invoked: guest code runs natively, so there is no
    // per-instruction or per-block instrumentation point to hang them on.
    struct inert_hook_entry
    {
        int tag{};
    };

    struct instruction_hook_entry
    {
        arm64_hookable_instructions type{};
        instruction_hook_callback callback{};
    };

    struct interrupt_hook_entry
    {
        interrupt_hook_callback callback{};
    };

    struct memory_violation_hook_entry
    {
        memory_violation_hook_callback callback{};
    };

    // A vCPU is bound to the thread that created it -- hv_vcpu_get_reg from anywhere else returns
    // HV_BAD_ARGUMENT -- and one thread may own only one vCPU, so a second hv_vcpu_create on the same
    // thread returns HV_BUSY. Both constraints are answered by giving every emulator instance its own
    // runner thread that creates, owns and destroys the vCPU, and executing work on it.
    class vcpu_thread
    {
      public:
        vcpu_thread()
        {
            std::promise<void> ready{};
            auto ready_future = ready.get_future();

            this->thread_ = std::thread{[this, &ready] { this->run(ready); }};
            ready_future.get();
        }

        ~vcpu_thread()
        {
            {
                const std::lock_guard lock{this->mutex_};
                this->shutting_down_ = true;
            }

            this->pending_.notify_one();
            this->thread_.join();
        }

        vcpu_thread(const vcpu_thread&) = delete;
        vcpu_thread& operator=(const vcpu_thread&) = delete;
        vcpu_thread(vcpu_thread&&) = delete;
        vcpu_thread& operator=(vcpu_thread&&) = delete;

        void execute(std::function<void(hv_vcpu_t, hv_vcpu_exit_t&)> work)
        {
            std::unique_lock lock{this->mutex_};
            this->work_ = std::move(work);
            this->pending_.notify_one();
            this->done_.wait(lock, [this] { return !this->work_; });

            if (this->error_)
            {
                std::rethrow_exception(std::exchange(this->error_, nullptr));
            }
        }

        hv_vcpu_t id() const
        {
            return this->vcpu_;
        }

      private:
        void run(std::promise<void>& ready)
        {
            hv_vcpu_exit_t* exit = nullptr;
            hv_call(hv_vcpu_create(&this->vcpu_, &exit, nullptr), "hv_vcpu_create");
            ready.set_value();

            std::unique_lock lock{this->mutex_};
            while (true)
            {
                this->pending_.wait(lock, [this] { return this->work_ || this->shutting_down_; });
                if (!this->work_)
                {
                    break;
                }

                lock.unlock();
                try
                {
                    this->work_(this->vcpu_, *exit);
                }
                catch (...)
                {
                    this->error_ = std::current_exception();
                }
                lock.lock();

                this->work_ = nullptr;
                this->done_.notify_one();
            }

            hv_vcpu_destroy(this->vcpu_);
        }

        hv_vcpu_t vcpu_{};
        std::thread thread_{};
        std::mutex mutex_{};
        std::condition_variable pending_{};
        std::condition_variable done_{};
        std::function<void(hv_vcpu_t, hv_vcpu_exit_t&)> work_{};
        std::exception_ptr error_{};
        bool shutting_down_{false};
    };
}
