#define HVF_EMULATOR_IMPL

#include "hvf_arm64_emulator.hpp"

#include "hvf_arm64_common.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <map>
#include <set>
#include <optional>
#include <utility>
#include <sys/mman.h>

namespace sogen::hvf
{
    namespace
    {
        constexpr uint64_t NZCV_MASK = 0xF0000000ULL;
        constexpr uint32_t hvf_state_version = 1;

        // pacia/pacib/pacda/pacdb x0, x1. The generic key has no pointer-signing form.
        uint32_t pac_instruction_for(const arm64_pauth_key key)
        {
            switch (key)
            {
            case arm64_pauth_key::instruction_a:
                return 0xDAC10020;
            case arm64_pauth_key::instruction_b:
                return 0xDAC10420;
            case arm64_pauth_key::data_a:
                return 0xDAC10820;
            case arm64_pauth_key::data_b:
                return 0xDAC10C20;
            default:
                return 0;
            }
        }

        std::string format_hex(const uint64_t value)
        {
            std::array<char, 24> buffer{};
            const auto count = std::snprintf(buffer.data(), buffer.size(), "0x%llx", static_cast<unsigned long long>(value));
            return count > 0 ? std::string(buffer.data(), static_cast<size_t>(count)) : std::string{"0x0"};
        }

        struct register_file
        {
            std::array<uint64_t, 31> x{};
            uint64_t pc{};
            uint64_t sp{};
            uint64_t cpsr{};
            uint64_t fpcr{};
            uint64_t fpsr{};
            uint64_t cpacr_el1{};
            uint64_t tpidr_el0{};
            uint64_t tpidrro_el0{};
            std::array<hv_simd_fp_uchar16_t, 32> v{};

            // The ten PAC key halves occupy a contiguous 0xc108..0xc119 sysreg range. Changing a key
            // changes every signature, so they are per-instance state: a snapshot that omitted them
            // would fail every autia the guest performed before it was taken.
            std::array<uint64_t, 10> pac_keys{};
            uint64_t sctlr_el1{};
            uint64_t tcr_el1{};
            uint64_t ttbr0_el1{};
            uint64_t mair_el1{};
            uint64_t vbar_el1{};
            uint64_t sp_el1{};
            uint64_t esr_el1{};
            uint64_t far_el1{};
            uint64_t elr_el1{};
            uint64_t spsr_el1{};

            void load(const hv_vcpu_t vcpu)
            {
                for (size_t i = 0; i < this->x.size(); ++i)
                {
                    hv_call(hv_vcpu_get_reg(vcpu, static_cast<hv_reg_t>(HV_REG_X0 + i), &this->x[i]), "hv_vcpu_get_reg");
                }

                hv_call(hv_vcpu_get_reg(vcpu, HV_REG_PC, &this->pc), "hv_vcpu_get_reg");
                hv_call(hv_vcpu_get_reg(vcpu, HV_REG_CPSR, &this->cpsr), "hv_vcpu_get_reg");
                hv_call(hv_vcpu_get_reg(vcpu, HV_REG_FPCR, &this->fpcr), "hv_vcpu_get_reg");
                hv_call(hv_vcpu_get_reg(vcpu, HV_REG_FPSR, &this->fpsr), "hv_vcpu_get_reg");

                hv_call(hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_SP_EL0, &this->sp), "hv_vcpu_get_sys_reg");
                hv_call(hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_CPACR_EL1, &this->cpacr_el1), "hv_vcpu_get_sys_reg");
                hv_call(hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_TPIDR_EL0, &this->tpidr_el0), "hv_vcpu_get_sys_reg");
                hv_call(hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_TPIDRRO_EL0, &this->tpidrro_el0), "hv_vcpu_get_sys_reg");

                for (size_t i = 0; i < this->v.size(); ++i)
                {
                    hv_call(hv_vcpu_get_simd_fp_reg(vcpu, static_cast<hv_simd_fp_reg_t>(HV_SIMD_FP_REG_Q0 + i), &this->v[i]),
                            "hv_vcpu_get_simd_fp_reg");
                }

                for (size_t i = 0; i < this->pac_keys.size(); ++i)
                {
                    hv_call(hv_vcpu_get_sys_reg(vcpu, pac_key_registers[i], &this->pac_keys[i]), "hv_vcpu_get_sys_reg");
                }

                hv_call(hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_SCTLR_EL1, &this->sctlr_el1), "hv_vcpu_get_sys_reg");
                hv_call(hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_TCR_EL1, &this->tcr_el1), "hv_vcpu_get_sys_reg");
                hv_call(hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_TTBR0_EL1, &this->ttbr0_el1), "hv_vcpu_get_sys_reg");
                hv_call(hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_MAIR_EL1, &this->mair_el1), "hv_vcpu_get_sys_reg");
                hv_call(hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_VBAR_EL1, &this->vbar_el1), "hv_vcpu_get_sys_reg");
                hv_call(hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_SP_EL1, &this->sp_el1), "hv_vcpu_get_sys_reg");
                hv_call(hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_ESR_EL1, &this->esr_el1), "hv_vcpu_get_sys_reg");
                hv_call(hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_FAR_EL1, &this->far_el1), "hv_vcpu_get_sys_reg");
                hv_call(hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_ELR_EL1, &this->elr_el1), "hv_vcpu_get_sys_reg");
                hv_call(hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_SPSR_EL1, &this->spsr_el1), "hv_vcpu_get_sys_reg");
            }

            void store(const hv_vcpu_t vcpu) const
            {
                for (size_t i = 0; i < this->x.size(); ++i)
                {
                    hv_call(hv_vcpu_set_reg(vcpu, static_cast<hv_reg_t>(HV_REG_X0 + i), this->x[i]), "hv_vcpu_set_reg");
                }

                hv_call(hv_vcpu_set_reg(vcpu, HV_REG_PC, this->pc), "hv_vcpu_set_reg");
                hv_call(hv_vcpu_set_reg(vcpu, HV_REG_CPSR, this->cpsr), "hv_vcpu_set_reg");
                hv_call(hv_vcpu_set_reg(vcpu, HV_REG_FPCR, this->fpcr), "hv_vcpu_set_reg");
                hv_call(hv_vcpu_set_reg(vcpu, HV_REG_FPSR, this->fpsr), "hv_vcpu_set_reg");

                hv_call(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_SP_EL0, this->sp), "hv_vcpu_set_sys_reg");
                hv_call(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_CPACR_EL1, this->cpacr_el1), "hv_vcpu_set_sys_reg");
                hv_call(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_TPIDR_EL0, this->tpidr_el0), "hv_vcpu_set_sys_reg");
                hv_call(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_TPIDRRO_EL0, this->tpidrro_el0), "hv_vcpu_set_sys_reg");

                for (size_t i = 0; i < this->v.size(); ++i)
                {
                    hv_call(hv_vcpu_set_simd_fp_reg(vcpu, static_cast<hv_simd_fp_reg_t>(HV_SIMD_FP_REG_Q0 + i), this->v[i]),
                            "hv_vcpu_set_simd_fp_reg");
                }

                for (size_t i = 0; i < this->pac_keys.size(); ++i)
                {
                    hv_call(hv_vcpu_set_sys_reg(vcpu, pac_key_registers[i], this->pac_keys[i]), "hv_vcpu_set_sys_reg");
                }

                hv_call(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_SCTLR_EL1, this->sctlr_el1), "hv_vcpu_set_sys_reg");
                hv_call(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_TCR_EL1, this->tcr_el1), "hv_vcpu_set_sys_reg");
                hv_call(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_TTBR0_EL1, this->ttbr0_el1), "hv_vcpu_set_sys_reg");
                hv_call(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_MAIR_EL1, this->mair_el1), "hv_vcpu_set_sys_reg");
                hv_call(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_VBAR_EL1, this->vbar_el1), "hv_vcpu_set_sys_reg");
                hv_call(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_SP_EL1, this->sp_el1), "hv_vcpu_set_sys_reg");
            }
        };

        struct register_access
        {
            void* storage;
            size_t width;
        };

        // x29 and x30 are listed separately because arm64_register mirrors uc_arm64_reg, where they
        // appear *before* x0 rather than after x28. Collapsing the ranges would silently alias them.
        std::optional<register_access> classify_register(register_file& file, const arm64_register reg)
        {
            const auto id = static_cast<int>(reg);

            if (id >= static_cast<int>(arm64_register::x0) && id <= static_cast<int>(arm64_register::x28))
            {
                return register_access{.storage = &file.x[static_cast<size_t>(id - static_cast<int>(arm64_register::x0))],
                                       .width = sizeof(uint64_t)};
            }

            if (id >= static_cast<int>(arm64_register::w0) && id <= static_cast<int>(arm64_register::w30))
            {
                return register_access{.storage = &file.x[static_cast<size_t>(id - static_cast<int>(arm64_register::w0))],
                                       .width = sizeof(uint32_t)};
            }

            if (id >= static_cast<int>(arm64_register::q0) && id <= static_cast<int>(arm64_register::q31))
            {
                return register_access{.storage = &file.v[static_cast<size_t>(id - static_cast<int>(arm64_register::q0))],
                                       .width = sizeof(hv_simd_fp_uchar16_t)};
            }

            if (id >= static_cast<int>(arm64_register::d0) && id <= static_cast<int>(arm64_register::d31))
            {
                return register_access{.storage = &file.v[static_cast<size_t>(id - static_cast<int>(arm64_register::d0))],
                                       .width = sizeof(uint64_t)};
            }

            if (id >= static_cast<int>(arm64_register::s0) && id <= static_cast<int>(arm64_register::s31))
            {
                return register_access{.storage = &file.v[static_cast<size_t>(id - static_cast<int>(arm64_register::s0))],
                                       .width = sizeof(uint32_t)};
            }

            switch (reg)
            {
            case arm64_register::x29:
                return register_access{.storage = &file.x[29], .width = sizeof(uint64_t)};
            case arm64_register::x30:
                return register_access{.storage = &file.x[30], .width = sizeof(uint64_t)};
            case arm64_register::pc:
                return register_access{.storage = &file.pc, .width = sizeof(uint64_t)};
            case arm64_register::sp:
                return register_access{.storage = &file.sp, .width = sizeof(uint64_t)};
            case arm64_register::cpacr_el1:
                return register_access{.storage = &file.cpacr_el1, .width = sizeof(uint64_t)};
            case arm64_register::fpcr:
                return register_access{.storage = &file.fpcr, .width = sizeof(uint64_t)};
            case arm64_register::fpsr:
                return register_access{.storage = &file.fpsr, .width = sizeof(uint64_t)};
            case arm64_register::tpidr_el0:
                return register_access{.storage = &file.tpidr_el0, .width = sizeof(uint64_t)};
            case arm64_register::tpidrro_el0:
                return register_access{.storage = &file.tpidrro_el0, .width = sizeof(uint64_t)};
            default:
                return std::nullopt;
            }
        }

        class hvf_arm64_emulator : public arm64_mappable_emulator
        {
          public:
            hvf_arm64_emulator()
            {
                // Primed from the vCPU rather than assumed. A freshly created vCPU reports CPSR as 0 on
                // this OS -- measured, not assumed -- but the cache is the only copy the guest is
                // entered with, so it has to start from whatever the vCPU actually holds.
                this->vcpu_.execute([this](const hv_vcpu_t vcpu, hv_vcpu_exit_t&) { this->registers_.load(vcpu); });

                this->tables_size_ = 0x200000;
                this->tables_host_ = ::mmap(nullptr, this->tables_size_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
                if (this->tables_host_ == MAP_FAILED)
                {
                    throw std::runtime_error("hvf: could not reserve the guest page table arena");
                }

                this->tables_ipa_ = this->allocate_ipa(this->tables_size_);
                hv_call(hv_vm_map(this->tables_host_, this->tables_ipa_, this->tables_size_, HV_MEMORY_READ | HV_MEMORY_WRITE),
                        "hv_vm_map");

                this->ttbr0_ = this->allocate_table();

                // Above Darwin's 47-bit user VA, so it can never collide with a guest mapping.
                this->el1_va_ = 0xFFFF00000000ULL;
                this->el1_ipa_ = this->allocate_ipa(host_frame_size);
                this->el1_host_ = ::mmap(nullptr, host_frame_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
                if (this->el1_host_ == MAP_FAILED)
                {
                    throw std::runtime_error("hvf: could not reserve the EL1 stub page");
                }

                hv_call(hv_vm_map(this->el1_host_, this->el1_ipa_, host_frame_size, HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC),
                        "hv_vm_map");

                this->build_el1_page();

                // EL1-only, executable at EL1: AP[2:1] = 00 gives EL1 read/write and no EL0 access, and
                // PXN stays clear so the stub can run. UXN keeps EL0 out of it entirely.
                constexpr uint64_t el1_attributes = pte_valid | pte_page | pte_attr_normal | pte_shareable | pte_access_flag | pte_uxn;

                for (uint64_t offset = 0; offset < host_frame_size; offset += guest_page_size)
                {
                    *this->leaf_entry(this->el1_va_ + offset) = (this->el1_ipa_ + offset) | el1_attributes;
                }

                this->scratch_va_ = this->el1_va_ + host_frame_size;
                this->scratch_ipa_ = this->allocate_ipa(host_frame_size);
                this->scratch_host_ = ::mmap(nullptr, host_frame_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
                if (this->scratch_host_ == MAP_FAILED)
                {
                    throw std::runtime_error("hvf: could not reserve the scratch page");
                }

                hv_call(
                    hv_vm_map(this->scratch_host_, this->scratch_ipa_, host_frame_size, HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC),
                    "hv_vm_map");

                this->regions_[this->scratch_va_] = mapped_region{.host = this->scratch_host_,
                                                                  .host_size = host_frame_size,
                                                                  .ipa = this->scratch_ipa_,
                                                                  .size = host_frame_size,
                                                                  .owns_host = false};

                for (uint64_t offset = 0; offset < host_frame_size; offset += guest_page_size)
                {
                    *this->leaf_entry(this->scratch_va_ + offset) = (this->scratch_ipa_ + offset) | leaf_attributes(memory_permission::all);
                }

                this->configure_translation();
            }

            // Stage-1 translation has to be switched on before any of the tables above mean anything.
            void configure_translation()
            {
                this->vcpu_.execute([this](const hv_vcpu_t vcpu, hv_vcpu_exit_t&) {
                    constexpr uint64_t tcr_t0sz_48bit = 16;
                    constexpr uint64_t tcr_irgn0_wb = 1ULL << 8;
                    constexpr uint64_t tcr_orgn0_wb = 1ULL << 10;
                    constexpr uint64_t tcr_sh0_inner = 3ULL << 12;
                    constexpr uint64_t tcr_tg0_4k = 0ULL << 14;
                    constexpr uint64_t tcr_ips_40bit = 2ULL << 32;
                    constexpr uint64_t tcr_epd1 = 1ULL << 23;

                    constexpr uint64_t sctlr_m = 1ULL << 0;
                    constexpr uint64_t sctlr_c = 1ULL << 2;
                    constexpr uint64_t sctlr_i = 1ULL << 12;
                    constexpr uint64_t sctlr_reserved = 0x30D00800ULL;

                    constexpr uint64_t mair_normal_writeback = 0xFFULL;
                    constexpr uint64_t cpacr_fpen = 3ULL << 20;

                    hv_call(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_TTBR0_EL1, this->ttbr0_), "hv_vcpu_set_sys_reg");
                    hv_call(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_TCR_EL1,
                                                tcr_t0sz_48bit | tcr_irgn0_wb | tcr_orgn0_wb | tcr_sh0_inner | tcr_tg0_4k | tcr_ips_40bit |
                                                    tcr_epd1),
                            "hv_vcpu_set_sys_reg");
                    hv_call(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_MAIR_EL1, mair_normal_writeback), "hv_vcpu_set_sys_reg");
                    hv_call(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_VBAR_EL1, this->el1_va_), "hv_vcpu_set_sys_reg");
                    // The En{IA,IB,DA,DB} bits are what make the PAC instructions do anything at EL0;
                    // without them pacia is a no-op and every signature reads back as the input.
                    constexpr uint64_t sctlr_en_ia = 1ULL << 31;
                    constexpr uint64_t sctlr_en_ib = 1ULL << 30;
                    constexpr uint64_t sctlr_en_da = 1ULL << 27;
                    constexpr uint64_t sctlr_en_db = 1ULL << 13;

                    hv_call(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_SCTLR_EL1,
                                                sctlr_reserved | sctlr_m | sctlr_c | sctlr_i | sctlr_en_ia | sctlr_en_ib | sctlr_en_da |
                                                    sctlr_en_db),
                            "hv_vcpu_set_sys_reg");
                    hv_call(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_CPACR_EL1, cpacr_fpen), "hv_vcpu_set_sys_reg");

                    this->registers_.load(vcpu);
                });
            }

            ~hvf_arm64_emulator() override
            {
                for (auto& [base, region] : this->regions_)
                {
                    hv_vm_unmap(region.ipa, region.host_size);
                    if (region.owns_host)
                    {
                        ::munmap(region.host, region.host_size);
                    }
                }

                if (this->scratch_host_ != nullptr && this->scratch_host_ != MAP_FAILED)
                {
                    hv_vm_unmap(this->scratch_ipa_, host_frame_size);
                    ::munmap(this->scratch_host_, host_frame_size);
                }

                if (this->el1_host_ != nullptr && this->el1_host_ != MAP_FAILED)
                {
                    hv_vm_unmap(this->el1_ipa_, host_frame_size);
                    ::munmap(this->el1_host_, host_frame_size);
                }

                if (this->tables_host_ != nullptr && this->tables_host_ != MAP_FAILED)
                {
                    hv_vm_unmap(this->tables_ipa_, this->tables_size_);
                    ::munmap(this->tables_host_, this->tables_size_);
                }

                release_arena(this->arena_base_);
            }

            std::string get_name() const override
            {
                return "Hypervisor.framework";
            }

            size_t index() const override
            {
                return 0;
            }

            bool supports_multiple_vcpus() const override
            {
                return false;
            }

            // Guest code runs natively on silicon, so there is no point at which every executed
            // instruction could be observed. Address-specific hooks are implementable by patching a brk,
            // which is exactly the int3 mode this reports as its own.
            bool supports_global_memory_execution_hooks() const override
            {
                return false;
            }

            void set_memory_execution_hook_mode(memory_execution_hook_mode) override
            {
            }

            // Hypervisor.framework exposes no instruction retire count for a guest, so a caller that
            // needs exact counts has to use a different backend rather than trust an approximation.
            bool supports_instruction_counting() const override
            {
                return false;
            }

            // hv_vcpus_exit() may be called from any thread, which is what makes an asynchronous stop
            // possible at all.
            bool is_stop_thread_safe() const override
            {
                return true;
            }

            bool read_descriptor_table(int, descriptor_table_register&) override
            {
                return false;
            }

            bool has_violation() const override
            {
                return this->violation_ip_.has_value();
            }

            void start(const size_t count) override
            {
                this->stop_requested_ = false;
                this->violation_ip_.reset();

                auto remaining = count;

                while (!this->stop_requested_)
                {
                    this->enter_guest(count != 0);

                    const auto action = this->dispatch_exit();
                    if (action == exit_action::stop)
                    {
                        return;
                    }

                    if (count != 0 && action == exit_action::stepped && --remaining == 0)
                    {
                        return;
                    }
                }
            }

            // hv_vcpus_exit is the one call that is legal from a thread other than the vCPU's owner,
            // which is what makes an asynchronous stop possible and is why is_stop_thread_safe is true.
            void stop() override
            {
                this->stop_requested_ = true;

                auto vcpu = this->vcpu_.id();
                hv_vcpus_exit(&vcpu, 1);
            }

            // While the guest is stopped -- which is whenever anything outside start() runs -- these
            // touch no HVF API at all, so they are safe from any thread. Deliberately no memset of the
            // destination: there is no in-out register encoding here, and zero-filling a 16-byte q read
            // for an 8-byte request would corrupt the caller's buffer.
            size_t read_raw_register(const int reg, void* value, const size_t size) override
            {
                const auto id = static_cast<arm64_register>(reg);

                if (id == arm64_register::nzcv)
                {
                    const auto flags = static_cast<uint32_t>(this->registers_.cpsr & NZCV_MASK);
                    std::memcpy(value, &flags, std::min(size, sizeof(flags)));
                    return sizeof(flags);
                }

                const auto access = classify_register(this->registers_, id);
                if (!access)
                {
                    throw std::runtime_error("hvf: unsupported register " + std::to_string(reg));
                }

                std::memcpy(value, access->storage, std::min(size, access->width));
                return access->width;
            }

            size_t write_raw_register(const int reg, const void* value, const size_t size) override
            {
                const auto id = static_cast<arm64_register>(reg);

                if (id == arm64_register::nzcv)
                {
                    uint32_t flags = 0;
                    std::memcpy(&flags, value, std::min(size, sizeof(flags)));
                    this->registers_.cpsr = (this->registers_.cpsr & ~NZCV_MASK) | (flags & NZCV_MASK);
                    return sizeof(flags);
                }

                const auto access = classify_register(this->registers_, id);
                if (!access)
                {
                    throw std::runtime_error("hvf: unsupported register " + std::to_string(reg));
                }

                std::memcpy(access->storage, value, std::min(size, access->width));
                return access->width;
            }

            std::vector<std::byte> save_registers() const override
            {
                std::vector<std::byte> data(sizeof(register_file));
                std::memcpy(data.data(), &this->registers_, sizeof(register_file));
                return data;
            }

            void restore_registers(const std::vector<std::byte>& register_data) override
            {
                if (register_data.size() != sizeof(register_file))
                {
                    throw std::runtime_error("hvf: register snapshot is the wrong size");
                }

                std::memcpy(&this->registers_, register_data.data(), sizeof(register_file));
            }

            void set_thread_pointer(const pointer_type value) override
            {
                this->registers_.tpidr_el0 = value;
            }

            pointer_type get_thread_pointer() override
            {
                return this->registers_.tpidr_el0;
            }

            // HVF's sysreg ids are the architectural encoding packed the obvious way: SCTLR_EL1
            // (3,0,1,0,0) is 0xc080, which is (3 << 14) | (1 << 7).
            uint64_t read_system_register(const uint32_t op0, const uint32_t op1, const uint32_t crn, const uint32_t crm,
                                          const uint32_t op2) override
            {
                const auto id = static_cast<hv_sys_reg_t>((op0 << 14) | (op1 << 11) | (crn << 7) | (crm << 3) | op2);

                uint64_t value = 0;
                this->vcpu_.execute(
                    [&](const hv_vcpu_t vcpu, hv_vcpu_exit_t&) { hv_call(hv_vcpu_get_sys_reg(vcpu, id, &value), "hv_vcpu_get_sys_reg"); });

                return value;
            }

            void set_pointer_authentication(const bool enabled) override
            {
                this->vcpu_.execute([enabled](const hv_vcpu_t vcpu, hv_vcpu_exit_t&) {
                    constexpr uint64_t keys = (1ULL << 31) | (1ULL << 30) | (1ULL << 27) | (1ULL << 13);

                    uint64_t sctlr = 0;
                    hv_call(hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_SCTLR_EL1, &sctlr), "hv_vcpu_get_sys_reg");
                    sctlr = enabled ? (sctlr | keys) : (sctlr & ~keys);
                    hv_call(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_SCTLR_EL1, sctlr), "hv_vcpu_set_sys_reg");
                });
            }

            // There is no host-side signing primitive: the keys live in the vCPU and only the guest can
            // apply them, so the instruction is executed in the guest on a page reserved for exactly
            // this, with the register file saved and restored around it.
            bool sign_pointer(pointer_type& pointer, const arm64_pauth_key key, const uint64_t discriminator) override
            {
                const auto instruction = pac_instruction_for(key);
                if (instruction == 0)
                {
                    return false;
                }

                const auto saved = this->registers_;

                this->write_memory(this->scratch_va_, &instruction, sizeof(instruction));

                this->registers_.x[0] = pointer;
                this->registers_.x[1] = discriminator;
                this->registers_.pc = this->scratch_va_;

                this->enter_guest(true);
                const auto action = this->dispatch_exit();
                const auto signed_pointer = this->registers_.x[0];

                this->registers_ = saved;

                if (action != exit_action::stepped)
                {
                    return false;
                }

                pointer = signed_pointer;
                return true;
            }

            emulator_hook* hook_memory_execution(memory_execution_hook_callback) override
            {
                return this->register_inert_hook();
            }

            emulator_hook* hook_memory_execution(const uint64_t address, memory_execution_hook_callback callback) override
            {
                auto entry = std::make_unique<execution_hook_entry>();
                entry->address = address;
                entry->callback = std::move(callback);

                if (!this->try_read_memory(address, &entry->original, sizeof(entry->original)))
                {
                    throw std::runtime_error("hvf: cannot hook execution at unmapped address " + format_hex(address));
                }

                this->write_memory(address, &insn_brk_0, sizeof(insn_brk_0));

                auto* handle = reinterpret_cast<emulator_hook*>(entry.get());
                this->execution_hooks_.push_back(std::move(entry));
                return handle;
            }

            emulator_hook* hook_memory_range_execution(uint64_t, uint64_t, memory_execution_hook_callback) override
            {
                return this->register_inert_hook();
            }

            emulator_hook* hook_memory_read(const uint64_t address, const uint64_t size, memory_access_hook_callback callback) override
            {
                return this->add_watchpoint(address, size, false, std::move(callback));
            }

            emulator_hook* hook_memory_write(const uint64_t address, const uint64_t size, memory_access_hook_callback callback) override
            {
                return this->add_watchpoint(address, size, true, std::move(callback));
            }

            emulator_hook* hook_instruction(const int instruction_type, instruction_hook_callback callback) override
            {
                auto entry = std::make_unique<instruction_hook_entry>();
                entry->type = static_cast<arm64_hookable_instructions>(instruction_type);
                entry->callback = std::move(callback);

                auto* handle = reinterpret_cast<emulator_hook*>(entry.get());
                this->instruction_hooks_.push_back(std::move(entry));
                return handle;
            }

            emulator_hook* hook_interrupt(interrupt_hook_callback callback) override
            {
                auto entry = std::make_unique<interrupt_hook_entry>();
                entry->callback = std::move(callback);

                auto* handle = reinterpret_cast<emulator_hook*>(entry.get());
                this->interrupt_hooks_.push_back(std::move(entry));
                return handle;
            }

            emulator_hook* hook_memory_violation(memory_violation_hook_callback callback) override
            {
                auto entry = std::make_unique<memory_violation_hook_entry>();
                entry->callback = std::move(callback);

                auto* handle = reinterpret_cast<emulator_hook*>(entry.get());
                this->violation_hooks_.push_back(std::move(entry));
                return handle;
            }

            emulator_hook* hook_basic_block(basic_block_hook_callback) override
            {
                return this->register_inert_hook();
            }

            emulator_hook* register_inert_hook()
            {
                auto entry = std::make_unique<inert_hook_entry>();
                auto* handle = reinterpret_cast<emulator_hook*>(entry.get());
                this->inert_hooks_.push_back(std::move(entry));
                return handle;
            }

            // ID_AA64DFR0_EL1 reports four watchpoints on this silicon, and one covers a naturally
            // aligned power-of-two range of at most eight bytes. The unbounded API cannot be honoured, so
            // the honest contract is to refuse what the hardware cannot hold rather than accept a hook
            // that would never fire.
            emulator_hook* add_watchpoint(const uint64_t address, const uint64_t size, const bool is_write,
                                          memory_access_hook_callback callback)
            {
                if (size == 0 || size > 8 || (size & (size - 1)) != 0 || (address % size) != 0)
                {
                    throw std::runtime_error("hvf: a watchpoint covers a naturally aligned power-of-two range of at most "
                                             "8 bytes; asked for " +
                                             std::to_string(size) + " bytes at " + format_hex(address));
                }

                if (this->access_hooks_.size() >= watchpoint_count)
                {
                    throw std::runtime_error("hvf: all " + std::to_string(watchpoint_count) + " hardware watchpoints are already in use");
                }

                auto entry = std::make_unique<memory_access_hook_entry>();
                entry->address = address;
                entry->size = static_cast<size_t>(size);
                entry->is_write = is_write;
                entry->watchpoint = this->access_hooks_.size();
                entry->callback = std::move(callback);

                auto* handle = reinterpret_cast<emulator_hook*>(entry.get());
                this->access_hooks_.push_back(std::move(entry));
                this->program_watchpoints();
                return handle;
            }

            void program_watchpoints()
            {
                this->vcpu_.execute([this](const hv_vcpu_t vcpu, hv_vcpu_exit_t&) {
                    for (const auto& entry : this->access_hooks_)
                    {
                        constexpr uint64_t control_enable = 1ULL << 0;
                        constexpr uint64_t control_el0 = 1ULL << 1;
                        const auto load_store = entry->is_write ? (2ULL << 3) : (1ULL << 3);
                        const auto byte_select = ((1ULL << entry->size) - 1) << 5;

                        hv_call(hv_vcpu_set_sys_reg(vcpu, watchpoint_value_registers[entry->watchpoint], entry->address),
                                "hv_vcpu_set_sys_reg");
                        hv_call(hv_vcpu_set_sys_reg(vcpu, watchpoint_control_registers[entry->watchpoint],
                                                    control_enable | control_el0 | load_store | byte_select),
                                "hv_vcpu_set_sys_reg");
                    }
                });
            }

            void delete_hook(emulator_hook* hook) override
            {
                const auto erase_from = [hook](auto& container) {
                    std::erase_if(container, [hook](const auto& entry) { return reinterpret_cast<emulator_hook*>(entry.get()) == hook; });
                };

                for (const auto& entry : this->execution_hooks_)
                {
                    if (reinterpret_cast<emulator_hook*>(entry.get()) == hook)
                    {
                        this->write_memory(entry->address, &entry->original, sizeof(entry->original));
                    }
                }

                erase_from(this->instruction_hooks_);
                erase_from(this->interrupt_hooks_);
                erase_from(this->violation_hooks_);
                erase_from(this->execution_hooks_);
                erase_from(this->access_hooks_);
                erase_from(this->inert_hooks_);
            }

            // These resolve through the region map rather than the guest MMU: the memory manager calls
            // them while the guest is stopped and before any page table exists.
            void read_memory(const uint64_t address, void* data, const size_t size) const override
            {
                if (!this->try_read_memory(address, data, size))
                {
                    throw std::runtime_error("hvf: unreadable guest memory");
                }
            }

            bool try_read_memory(const uint64_t address, void* data, const size_t size) const override
            {
                return this->access(address, size, [&](void* host, const size_t offset, const size_t length) {
                    std::memcpy(static_cast<uint8_t*>(data) + offset, host, length);
                });
            }

            void write_memory(const uint64_t address, const void* data, const size_t size) override
            {
                if (!this->try_write_memory(address, data, size))
                {
                    throw std::runtime_error("hvf: unwritable guest memory");
                }
            }

            bool try_write_memory(const uint64_t address, const void* data, const size_t size) override
            {
                return this->access(address, size, [&](void* host, const size_t offset, const size_t length) {
                    std::memcpy(host, static_cast<const uint8_t*>(data) + offset, length);
                });
            }

            bool host_memory_aliasing_is_coherent() const override
            {
                return true;
            }

            // Backed read-only at stage 2 and re-seeded from the read callback before every entry, so
            // reads execute natively on silicon and only writes trap. That is what keeps the decoder
            // down to ISV=1 writes rather than needing a full AArch64 load/store decoder.
            void map_mmio(const uint64_t address, const size_t size, mmio_read_callback read_cb, mmio_write_callback write_cb) override
            {
                const auto rounded = round_up(size, host_frame_size);

                auto* host = ::mmap(nullptr, rounded, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
                if (host == MAP_FAILED)
                {
                    throw std::runtime_error("hvf: could not reserve host memory for the mmio mapping");
                }

                const auto ipa = this->allocate_ipa(rounded);
                hv_call(hv_vm_map(host, ipa, rounded, HV_MEMORY_READ | HV_MEMORY_EXEC), "hv_vm_map");

                this->regions_[address] = mapped_region{.host = host, .host_size = rounded, .ipa = ipa, .size = rounded, .owns_host = true};
                this->mmio_.push_back(mmio_region{.address = address,
                                                  .size = size,
                                                  .ipa = ipa,
                                                  .host = host,
                                                  .read_cb = std::move(read_cb),
                                                  .write_cb = std::move(write_cb)});

                // Ordinary EL0 read/write leaves: the stage-2 mapping is what enforces the trap, so
                // stage 1 must not reject the access first and hide it.
                for (uint64_t offset = 0; offset < rounded; offset += guest_page_size)
                {
                    *this->leaf_entry(address + offset) = (ipa + offset) | leaf_attributes(memory_permission::read_write);
                }

                this->tables_dirty_ = true;
            }

            void map_memory(const uint64_t address, const size_t size, const memory_permission permissions) override
            {
                const auto rounded = round_up(size, host_frame_size);

                // mmap returns 16 KiB-aligned memory because the host page size *is* 16 KiB, which is
                // exactly what hv_vm_map demands of the host VA, the IPA and the size alike.
                auto* host = ::mmap(nullptr, rounded, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
                if (host == MAP_FAILED)
                {
                    throw std::runtime_error("hvf: could not reserve host memory for the guest mapping");
                }

                this->install(address, rounded, host, permissions, true);
            }

            void map_host_memory(const uint64_t address, const size_t size, void* host_pointer,
                                 const memory_permission permissions) override
            {
                // The one place HVF's granularity is visible to a caller. Naming both operands beats a
                // bare HV_BAD_ARGUMENT from inside hv_vm_map.
                if ((reinterpret_cast<uintptr_t>(host_pointer) % host_frame_size) != 0 || (size % host_frame_size) != 0)
                {
                    throw std::runtime_error("hvf: host aliasing needs a 16 KiB-aligned pointer and size");
                }

                this->install(address, size, host_pointer, permissions, false);
            }

            void unmap_memory(const uint64_t address, const size_t size) override
            {
                for (uint64_t offset = 0; offset < size; offset += guest_page_size)
                {
                    if (auto* leaf = this->find_leaf(address + offset); leaf != nullptr)
                    {
                        *leaf = 0;
                        this->tables_dirty_ = true;
                    }
                }

                this->release_covered_regions(address, size);
            }

            // Rewrites stage-1 leaves only. hv_vm_protect is never called: the HVF mapping stays
            // permissive RWX backing and the real permissions live in the guest tables, which is what
            // makes 4 KiB granularity possible under a 16 KiB host page.
            void apply_memory_protection(const uint64_t address, const size_t size, const memory_permission permissions) override
            {
                for (uint64_t offset = 0; offset < size; offset += guest_page_size)
                {
                    const auto page = address + offset;
                    const auto resolved = this->resolve(page);
                    if (!resolved)
                    {
                        continue;
                    }

                    *this->leaf_entry(page) = resolved->ipa | leaf_attributes(permissions);
                    this->tables_dirty_ = true;
                }
            }

            // Guest memory is not part of this, matching every other backend. The version tag makes a
            // stale snapshot fail loudly rather than being reinterpreted as a newer layout.
            void serialize_state(utils::buffer_serializer& buffer, bool) const override
            {
                buffer.write(hvf_state_version);
                buffer.write(this->registers_);
            }

            void deserialize_state(utils::buffer_deserializer& buffer, bool) override
            {
                const auto version = buffer.read<uint32_t>();
                if (version != hvf_state_version)
                {
                    throw std::runtime_error("hvf: unsupported vcpu state version " + std::to_string(version));
                }

                buffer.read(this->registers_);
            }

          private:
            enum class exit_action : uint8_t
            {
                resume,
                stepped,
                stop,
            };

            // Entry to EL0 always goes through this trampoline, never by writing PC and CPSR directly.
            // That makes "enter" and "resume" one mechanism, and makes every stage-1 edit visible
            // without a separate flush path -- only the guest can execute tlbi.
            void build_el1_page()
            {
                std::array<uint32_t, 0x400> page{};

                // ic iallu as well as tlbi: an execution hook patches a brk over guest code from the
                // host side, and with SCTLR_EL1.I set the instruction cache can still hold the
                // displaced word. Only the guest can run cache maintenance, so it belongs here.
                size_t offset = 0;
                page[offset++] = insn_ic_iallu;
                page[offset++] = insn_tlbi_vmalle1;
                page[offset++] = insn_dsb_nsh;
                page[offset++] = insn_isb;
                page[offset++] = insn_eret;

                // A brk in every vector: an unexpected exception class then exits with a known EC at a
                // known PC instead of silently spinning.
                for (size_t vector = 0; vector < 16; ++vector)
                {
                    page[(0x400 + vector * 0x80) / sizeof(uint32_t)] = insn_brk_0;
                }

                const auto sync_vector = 0x400 / sizeof(uint32_t);
                page[sync_vector + 0] = insn_hvc_0;
                page[sync_vector + 1] = insn_ic_iallu;
                page[sync_vector + 2] = insn_tlbi_vmalle1;
                page[sync_vector + 3] = insn_dsb_nsh;
                page[sync_vector + 4] = insn_isb;
                page[sync_vector + 5] = insn_eret;

                std::memcpy(this->el1_host_, page.data(), page.size() * sizeof(uint32_t));
            }

            void enter_guest(const bool single_step)
            {
                this->refresh_mmio_pages();

                this->vcpu_.execute([this, single_step](const hv_vcpu_t vcpu, hv_vcpu_exit_t& exit) {
                    this->registers_.store(vcpu);
                    hv_call(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_ELR_EL1, this->registers_.pc), "hv_vcpu_set_sys_reg");

                    auto spsr = spsr_el0t;
                    if (single_step)
                    {
                        // The hardware clears PSTATE.SS on taking the exception, so it has to be
                        // re-armed before every step rather than once.
                        //
                        // Debug exceptions deliberately stay routed to EL1 rather than EL2. Routing them
                        // to EL2 makes MDSCR_EL1.SS step the stub's own maintenance instructions, and
                        // the step fires before the instruction retires, so resuming at that pc cannot
                        // make progress. Left at EL1, the step from EL0 lands in the stub's sync vector,
                        // whose hvc reports it with ESR_EL1.EC = 0x32, and the stub itself is never
                        // stepped because PSTATE.SS is clear at EL1.
                        spsr |= spsr_single_step;
                        hv_call(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_MDSCR_EL1, 1), "hv_vcpu_set_sys_reg");
                        hv_call(hv_vcpu_set_trap_debug_exceptions(vcpu, false), "hv_vcpu_set_trap_debug_exceptions");
                    }
                    else
                    {
                        hv_call(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_MDSCR_EL1, 0), "hv_vcpu_set_sys_reg");
                        hv_call(hv_vcpu_set_trap_debug_exceptions(vcpu, false), "hv_vcpu_set_trap_debug_exceptions");
                    }

                    hv_call(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_SPSR_EL1, spsr), "hv_vcpu_set_sys_reg");
                    hv_call(hv_vcpu_set_reg(vcpu, HV_REG_PC, this->el1_va_), "hv_vcpu_set_reg");
                    hv_call(hv_vcpu_set_reg(vcpu, HV_REG_CPSR, cpsr_el1h), "hv_vcpu_set_reg");

                    hv_call(hv_vcpu_run(vcpu), "hv_vcpu_run");

                    this->exit_reason_ = exit.reason;
                    this->esr_el2_ = exit.exception.syndrome;
                    this->far_el2_ = exit.exception.virtual_address;
                    this->exit_physical_address_ = exit.exception.physical_address;

                    this->registers_.load(vcpu);

                    uint64_t elr = 0;
                    hv_call(hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_ELR_EL1, &elr), "hv_vcpu_get_sys_reg");
                    hv_call(hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_ESR_EL1, &this->esr_el1_), "hv_vcpu_get_sys_reg");
                    hv_call(hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_FAR_EL1, &this->far_el1_), "hv_vcpu_get_sys_reg");
                    hv_call(hv_vcpu_get_reg(vcpu, HV_REG_PC, &this->exit_pc_), "hv_vcpu_get_reg");

                    // Where the guest's pc lives depends on how the exception was taken. Via the stub it
                    // is ELR_EL1, because the stub's hvc is what exited and HVF has already advanced the
                    // real PC past it. Taken straight to EL2 -- a cancel -- the real PC is the guest's
                    // own.
                    //
                    // The syndrome is only populated for an exception exit. A cancel leaves whatever was
                    // there, so reading it unconditionally makes a cancel look like a stub exit and
                    // reports the entry address instead of where the guest actually stopped.
                    const auto through_stub =
                        this->exit_reason_ == HV_EXIT_REASON_EXCEPTION && ((this->esr_el2_ >> 26) & 0x3F) == exception_class_hvc;
                    this->registers_.pc = through_stub ? elr : this->exit_pc_;
                });
            }

            struct mmio_region
            {
                uint64_t address{};
                size_t size{};
                uint64_t ipa{};
                void* host{};
                mmio_read_callback read_cb{};
                mmio_write_callback write_cb{};
            };

            void refresh_mmio_pages()
            {
                for (auto& region : this->mmio_)
                {
                    region.read_cb(region.address, region.host, region.size);
                }
            }

            const mmio_region* find_mmio_region(const uint64_t physical_address) const
            {
                for (const auto& region : this->mmio_)
                {
                    if (physical_address >= region.ipa && physical_address < region.ipa + region.size)
                    {
                        return &region;
                    }
                }

                return nullptr;
            }

            exit_action handle_stage2_abort()
            {
                const auto* region = this->find_mmio_region(this->exit_physical_address_);
                if (region == nullptr)
                {
                    this->violation_ip_ = this->registers_.pc;
                    return exit_action::stop;
                }

                const auto syndrome = this->esr_el2_ & 0x1FFFFFFULL;
                if (((syndrome >> 24) & 1) == 0)
                {
                    // ldp/stp, SIMD and writeback addressing all report ISV=0 and would need a real
                    // load/store decoder. Which forms actually reach a given device is not knowable in
                    // advance, so this names precisely what to add when a real case appears.
                    uint32_t instruction = 0;
                    (void)this->try_read_memory(this->registers_.pc, &instruction, sizeof(instruction));

                    throw std::runtime_error("hvf: ISV=0 mmio access at IPA " + format_hex(this->exit_physical_address_) +
                                             "; the faulting instruction is " + format_hex(instruction) +
                                             " and needs an AArch64 load/store decoder");
                }

                const auto is_write = ((syndrome >> 6) & 1) != 0;
                const auto access_size = size_t{1} << ((syndrome >> 22) & 3);
                const auto transfer_register = static_cast<size_t>((syndrome >> 16) & 0x1F);
                const auto offset = this->exit_physical_address_ - region->ipa;

                // SRT == 31 is the zero register rather than a thirty-second general register: a load
                // discards the value and a store writes zero.
                constexpr size_t zero_register = 31;

                if (is_write)
                {
                    uint64_t value = 0;
                    if (transfer_register != zero_register)
                    {
                        value = this->registers_.x[transfer_register];
                    }

                    region->write_cb(region->address + offset, &value, access_size);
                }
                else
                {
                    uint64_t value = 0;
                    region->read_cb(region->address + offset, &value, access_size);

                    if (transfer_register != zero_register)
                    {
                        this->registers_.x[transfer_register] = value;
                    }
                }

                // Unlike a permission fault, this access is complete and must not be retried.
                this->registers_.pc += trapping_instruction_size;
                return exit_action::stepped;
            }

            exit_action dispatch_exit()
            {
                if (this->exit_reason_ == HV_EXIT_REASON_CANCELED)
                {
                    return exit_action::stop;
                }

                if (this->exit_reason_ == HV_EXIT_REASON_VTIMER_ACTIVATED)
                {
                    this->vcpu_.execute([](const hv_vcpu_t vcpu, hv_vcpu_exit_t&) {
                        hv_call(hv_vcpu_set_vtimer_mask(vcpu, true), "hv_vcpu_set_vtimer_mask");
                    });
                    return exit_action::resume;
                }

                const auto el2_class = static_cast<uint32_t>(this->esr_el2_ >> 26) & 0x3F;
                switch (el2_class)
                {
                case exception_class_hvc:
                    return this->dispatch_el1_exception();
                case exception_class_data_abort_lower:
                    return this->handle_stage2_abort();
                case exception_class_brk:
                    return exit_action::stop;
                case exception_class_software_step:
                    return exit_action::stepped;
                case exception_class_watchpoint:
                    return exit_action::stop;
                default:
                    throw std::runtime_error("hvf: unhandled exception class " + std::to_string(el2_class));
                }
            }

            exit_action dispatch_el1_exception()
            {
                const auto el1_class = static_cast<uint32_t>(this->esr_el1_ >> 26) & 0x3F;

                // Every decoded EL1 exception is offered to the interrupt hooks, so a catch-all handler
                // can see classes the backend does not otherwise route.
                for (const auto& hook : this->interrupt_hooks_)
                {
                    hook->callback(*this, static_cast<int>(el1_class));
                }

                switch (el1_class)
                {
                case exception_class_software_step:
                    return exit_action::stepped;
                case exception_class_brk:
                    if (auto action = this->dispatch_patched_execution(); action.has_value())
                    {
                        return *action;
                    }

                    return this->dispatch_trapped_instruction(arm64_hookable_instructions::brk, 0, this->registers_.pc);
                case exception_class_svc:
                    return this->dispatch_trapped_instruction(arm64_hookable_instructions::svc, this->esr_el1_ & 0xFFFF,
                                                              this->registers_.pc - trapping_instruction_size);
                case exception_class_instruction_abort_lower:
                case exception_class_data_abort_lower:
                    return this->dispatch_abort(el1_class == exception_class_instruction_abort_lower);
                default:
                    throw std::runtime_error("hvf: unhandled EL1 exception class " + std::to_string(el1_class) + " (ESR_EL2 class " +
                                             std::to_string(static_cast<uint32_t>(this->esr_el2_ >> 26) & 0x3F) + ")");
                }
            }

            // A brk the backend planted for an execution hook, rather than one the guest wrote. The
            // displaced instruction still has to run: restore it, single-step exactly one instruction,
            // then put the brk back. Reporting the hit and resuming would silently skip it.
            std::optional<exit_action> dispatch_patched_execution()
            {
                const auto faulting = this->registers_.pc;

                std::vector<execution_hook_entry*> matches{};
                for (const auto& entry : this->execution_hooks_)
                {
                    if (entry->address == faulting)
                    {
                        matches.push_back(entry.get());
                    }
                }

                if (matches.empty())
                {
                    return std::nullopt;
                }

                for (auto* entry : matches)
                {
                    entry->callback(*this, faulting);
                }

                for (const auto* entry : matches)
                {
                    this->write_memory(entry->address, &entry->original, sizeof(entry->original));
                }

                this->enter_guest(true);
                const auto action = this->dispatch_exit();

                for (const auto* entry : matches)
                {
                    this->write_memory(entry->address, &insn_brk_0, sizeof(insn_brk_0));
                }

                return action == exit_action::stepped ? exit_action::resume : action;
            }

            // svc is a trap and brk is fault-like, so ELR_EL1 means different things for the two: after
            // the svc, but *at* the brk. The caller works out which, because getting it wrong shows up
            // only as an off-by-one pc that still looks plausible.
            //
            // The faulting address is set before the callback runs, which is what lets a syscall handler
            // read the address of the svc it is servicing.
            exit_action dispatch_trapped_instruction(const arm64_hookable_instructions type, const uint64_t data, const uint64_t faulting)
            {
                const auto after = faulting + trapping_instruction_size;
                this->registers_.pc = faulting;

                auto continuation = instruction_hook_continuation::run_instruction;
                auto handled = false;

                for (const auto& hook : this->instruction_hooks_)
                {
                    if (hook->type == type)
                    {
                        handled = true;
                        continuation = hook->callback(*this, data);
                    }
                }

                // run_instruction and skip_instruction are the same action here: the instruction has
                // already trapped, so there is nothing left to run or to skip. Only
                // finalized_instruction_pointer differs, leaving whatever pc the callback wrote.
                if (continuation != instruction_hook_continuation::finalized_instruction_pointer)
                {
                    this->registers_.pc = after;
                }

                return handled ? exit_action::stepped : exit_action::stop;
            }

            exit_action dispatch_abort(const bool instruction_fetch)
            {
                const auto status = static_cast<uint32_t>(this->esr_el1_ & 0x3F);
                const auto written = (this->esr_el1_ & (1ULL << 6)) != 0;

                // DFSC 0b0001xx is a translation fault. Anything else is reported as a protection
                // failure rather than being silently reclassified as unmapped.
                const auto type = ((status & 0x3C) == 0x04) ? memory_violation_type::unmapped : memory_violation_type::protection;

                const auto data_operation = written ? memory_operation::write : memory_operation::read;
                const auto operation = instruction_fetch ? memory_operation::exec : data_operation;

                auto continuation = memory_violation_continuation::stop;
                for (const auto& hook : this->violation_hooks_)
                {
                    continuation = hook->callback(*this, this->far_el1_, 1, operation, type);
                }

                if (continuation == memory_violation_continuation::stop)
                {
                    this->violation_ip_ = this->registers_.pc;
                    return exit_action::stop;
                }

                // An abort is a fault rather than a trap: ELR_EL1 already addresses the faulting
                // instruction, so resuming re-executes it and restart needs no pc arithmetic.
                return exit_action::resume;
            }

            struct mapped_region
            {
                void* host{};
                size_t host_size{};
                uint64_t ipa{};
                size_t size{};
                bool owns_host{};
            };

            struct resolved_page
            {
                void* host{};
                uint64_t ipa{};
            };

            static size_t round_up(const size_t value, const size_t granularity)
            {
                return ((value + granularity - 1) / granularity) * granularity;
            }

            // The VM is process-wide, so every instance carves its own slice of the 64 GiB IPA space:
            // 4 GiB each, which is 15 slices before 0x1000000000 -- the first IPA that fails. The slice
            // is returned on destruction rather than bumped forever: 15 is a limit on how many
            // instances are alive at once, not on how many a process may ever create, and a test suite
            // creates far more than 15 over its lifetime.
            static std::mutex& arena_mutex()
            {
                static std::mutex instance{};
                return instance;
            }

            static std::set<uint64_t>& arena_in_use()
            {
                static std::set<uint64_t> instance{};
                return instance;
            }

            static uint64_t acquire_arena()
            {
                const std::lock_guard lock{arena_mutex()};

                for (uint64_t base = 0x100000000ULL; base < ipa_space_limit; base += 0x100000000ULL)
                {
                    if (arena_in_use().insert(base).second)
                    {
                        return base;
                    }
                }

                throw std::runtime_error("hvf: the process ran out of intermediate physical address space");
            }

            static void release_arena(const uint64_t base)
            {
                const std::lock_guard lock{arena_mutex()};
                arena_in_use().erase(base);
            }

            uint64_t allocate_ipa(const size_t size)
            {
                const auto ipa = this->arena_next_;
                this->arena_next_ += round_up(size, host_frame_size);
                return ipa;
            }

            void install(const uint64_t address, const size_t size, void* host, const memory_permission permissions, const bool owns_host)
            {
                const auto ipa = this->allocate_ipa(size);
                hv_call(hv_vm_map(host, ipa, size, HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC), "hv_vm_map");

                this->regions_[address] = mapped_region{.host = host, .host_size = size, .ipa = ipa, .size = size, .owns_host = owns_host};

                for (uint64_t offset = 0; offset < size; offset += guest_page_size)
                {
                    *this->leaf_entry(address + offset) = (ipa + offset) | leaf_attributes(permissions);
                }

                this->tables_dirty_ = true;
            }

            std::optional<resolved_page> resolve(const uint64_t address) const
            {
                auto entry = this->regions_.upper_bound(address);
                if (entry == this->regions_.begin())
                {
                    return std::nullopt;
                }

                --entry;

                const auto offset = address - entry->first;
                if (offset >= entry->second.size)
                {
                    return std::nullopt;
                }

                return resolved_page{.host = static_cast<uint8_t*>(entry->second.host) + offset, .ipa = entry->second.ipa + offset};
            }

            template <typename Callback>
            bool access(const uint64_t address, const size_t size, const Callback& callback) const
            {
                size_t done = 0;

                while (done < size)
                {
                    const auto page = address + done;
                    const auto resolved = this->resolve(page);
                    if (!resolved)
                    {
                        return false;
                    }

                    const auto page_end = (page / guest_page_size + 1) * guest_page_size;
                    const auto length = std::min<size_t>(size - done, page_end - page);

                    if (this->leaf_is_present(page))
                    {
                        callback(resolved->host, done, length);
                    }
                    else
                    {
                        return false;
                    }

                    done += length;
                }

                return true;
            }

            void release_covered_regions(const uint64_t address, const size_t size)
            {
                for (auto entry = this->regions_.begin(); entry != this->regions_.end();)
                {
                    const auto base = entry->first;
                    auto& region = entry->second;

                    if (base < address || base + region.size > address + size)
                    {
                        ++entry;
                        continue;
                    }

                    hv_vm_unmap(region.ipa, region.host_size);
                    if (region.owns_host)
                    {
                        ::munmap(region.host, region.host_size);
                    }

                    entry = this->regions_.erase(entry);
                }
            }

            uint64_t* table_view(const uint64_t ipa)
            {
                return reinterpret_cast<uint64_t*>(static_cast<uint8_t*>(this->tables_host_) + (ipa - this->tables_ipa_));
            }

            const uint64_t* table_view(const uint64_t ipa) const
            {
                return reinterpret_cast<const uint64_t*>(static_cast<const uint8_t*>(this->tables_host_) + (ipa - this->tables_ipa_));
            }

            uint64_t allocate_table()
            {
                if (this->tables_used_ + guest_page_size > this->tables_size_)
                {
                    throw std::runtime_error("hvf: the guest page table arena is exhausted");
                }

                const auto ipa = this->tables_ipa_ + this->tables_used_;
                this->tables_used_ += guest_page_size;
                return ipa;
            }

            // 48-bit VA, 4 KiB granule, four levels. pte_page is the architectural table-descriptor bit
            // at levels 0-2 and the page bit at level 3 -- the same bit position, hence one constant.
            uint64_t* leaf_entry(const uint64_t address)
            {
                auto table = this->ttbr0_;

                for (int level = 0; level < 3; ++level)
                {
                    const auto shift = 39 - 9 * level;
                    const auto index = static_cast<size_t>((address >> shift) & 0x1FF);
                    auto& entry = this->table_view(table)[index];

                    if ((entry & pte_valid) == 0)
                    {
                        const auto child = this->allocate_table();
                        entry = child | pte_valid | pte_page;
                        table = child;
                    }
                    else
                    {
                        table = entry & pte_address_mask;
                    }
                }

                return this->table_view(table) + ((address >> 12) & 0x1FF);
            }

            const uint64_t* find_leaf(const uint64_t address) const
            {
                auto table = this->ttbr0_;

                for (int level = 0; level < 3; ++level)
                {
                    const auto shift = 39 - 9 * level;
                    const auto index = static_cast<size_t>((address >> shift) & 0x1FF);
                    const auto entry = this->table_view(table)[index];

                    if ((entry & pte_valid) == 0)
                    {
                        return nullptr;
                    }

                    table = entry & pte_address_mask;
                }

                return this->table_view(table) + ((address >> 12) & 0x1FF);
            }

            uint64_t* find_leaf(const uint64_t address)
            {
                return const_cast<uint64_t*>(std::as_const(*this).find_leaf(address));
            }

            bool leaf_is_present(const uint64_t address) const
            {
                const auto* leaf = this->find_leaf(address);
                return leaf != nullptr && (*leaf & pte_valid) != 0;
            }

            vm_reference vm_{};
            vcpu_thread vcpu_{};
            register_file registers_{};

            uint64_t arena_base_{acquire_arena()};
            uint64_t arena_next_{arena_base_};

            void* tables_host_{};
            uint64_t tables_ipa_{};
            size_t tables_size_{};
            size_t tables_used_{};
            uint64_t ttbr0_{};
            bool tables_dirty_{false};

            std::map<uint64_t, mapped_region> regions_{};
            std::vector<mmio_region> mmio_{};

            void* el1_host_{};
            uint64_t el1_ipa_{};
            uint64_t el1_va_{};

            void* scratch_host_{};
            uint64_t scratch_ipa_{};
            uint64_t scratch_va_{};

            std::atomic<bool> stop_requested_{false};
            std::optional<uint64_t> violation_ip_{};

            uint32_t exit_reason_{};
            uint64_t esr_el2_{};
            uint64_t far_el2_{};
            uint64_t esr_el1_{};
            uint64_t far_el1_{};
            uint64_t exit_pc_{};
            uint64_t exit_physical_address_{};

            std::vector<std::unique_ptr<instruction_hook_entry>> instruction_hooks_{};
            std::vector<std::unique_ptr<interrupt_hook_entry>> interrupt_hooks_{};
            std::vector<std::unique_ptr<memory_violation_hook_entry>> violation_hooks_{};
            std::vector<std::unique_ptr<execution_hook_entry>> execution_hooks_{};
            std::vector<std::unique_ptr<memory_access_hook_entry>> access_hooks_{};
            std::vector<std::unique_ptr<inert_hook_entry>> inert_hooks_{};
        };
    }

    std::unique_ptr<arm64_mappable_emulator> create_arm64_emulator()
    {
        return std::make_unique<hvf_arm64_emulator>();
    }
}
