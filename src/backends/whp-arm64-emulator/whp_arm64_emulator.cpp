#define WHP_ARM64_EMULATOR_IMPL
#include "whp_arm64_emulator.hpp"

#include <windows.h>
#include <WinHvPlatform.h>

#include <utils/finally.hpp>
#include <utils/object.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <map>
#include <mutex>
#include <optional>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace sogen::whp
{
    namespace
    {
        constexpr uint64_t page_size = 0x1000;
        constexpr uint64_t vector_gpa = 0xFFFF0000;
        constexpr uint64_t exit_gpa = vector_gpa + page_size;
        constexpr uint64_t gic_gpa = 0x08000000;
        constexpr uint64_t gic_size = 0x10000;
        constexpr uint64_t gicr_gpa = 0x08020000;
        constexpr uint64_t gicr_size = 0x20000;
        constexpr uint64_t user_address_limit = 1ull << 48;
        // Guest pages get physical addresses from this point up to the partition's physical address width,
        // above the private GIC, translation table and exception-vector ranges.
        constexpr uint64_t gpa_pool_base = 1ull << 32;
        constexpr uint64_t page_table_base = 0xFE000000;
        constexpr uint64_t page_table_limit = page_table_base + 0x01000000;
        constexpr uint64_t resume_gpa = vector_gpa + 0x800;
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
        constexpr uint32_t mrs_cycle_counter = 0xD53B9D00;

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

        [[noreturn]] void throw_hr(const HRESULT hr, const char* action)
        {
            std::ostringstream message{};
            message << action << " failed with HRESULT 0x" << std::hex << static_cast<uint32_t>(hr);
            throw std::runtime_error(message.str());
        }

        void check_hr(const HRESULT hr, const char* action)
        {
            if (FAILED(hr))
            {
                throw_hr(hr, action);
            }
        }

        void log_cleanup_failure(const HRESULT hr, const char* action) noexcept
        {
            if (FAILED(hr))
            {
                std::fprintf(stderr, "%s failed during cleanup: HRESULT 0x%08X\n", action, static_cast<uint32_t>(hr));
            }
        }

        void flush_instruction_cache(const void* host, const size_t size)
        {
            if (!FlushInstructionCache(GetCurrentProcess(), host, size))
            {
                throw std::runtime_error("FlushInstructionCache failed for WHP ARM64 backing: " + std::to_string(GetLastError()));
            }
        }

        struct partition_handle
        {
            WHV_PARTITION_HANDLE value{};

            partition_handle()
            {
                check_hr(WHvCreatePartition(&value), "WHvCreatePartition");
            }

            ~partition_handle()
            {
                log_cleanup_failure(WHvDeletePartition(value), "WHvDeletePartition");
            }

            partition_handle(const partition_handle&) = delete;
            partition_handle& operator=(const partition_handle&) = delete;
        };

        struct processor_handle
        {
            WHV_PARTITION_HANDLE partition{};

            explicit processor_handle(const WHV_PARTITION_HANDLE handle)
                : partition(handle)
            {
                check_hr(WHvCreateVirtualProcessor(partition, 0, 0), "WHvCreateVirtualProcessor");
            }

            ~processor_handle()
            {
                log_cleanup_failure(WHvDeleteVirtualProcessor(partition, 0), "WHvDeleteVirtualProcessor");
            }

            processor_handle(const processor_handle&) = delete;
            processor_handle& operator=(const processor_handle&) = delete;
        };

        std::shared_ptr<std::byte> allocate_page()
        {
            auto* page = static_cast<std::byte*>(VirtualAlloc(nullptr, page_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
            if (!page)
            {
                throw std::runtime_error("VirtualAlloc failed while backing WHP ARM64 memory: " + std::to_string(GetLastError()));
            }

            return {page, [](std::byte* allocation) {
                        if (!VirtualFree(allocation, 0, MEM_RELEASE))
                        {
                            std::fprintf(stderr, "VirtualFree failed during WHP ARM64 cleanup: %lu\n", GetLastError());
                        }
                    }};
        }

        struct mmio_region
        {
            uint64_t address{};
            size_t size{};
            mmio_read_callback read{};
            mmio_write_callback write{};
        };

        struct mapped_page
        {
            std::shared_ptr<std::byte> owned{};
            std::byte* host{};
            uint64_t gpa{};
            UINT32 flags{};
            memory_permission permissions{};
            std::shared_ptr<mmio_region> mmio{};
        };

        struct translation_table
        {
            std::shared_ptr<std::byte> backing;
            uint64_t gpa{};
            uint32_t used{};
        };

        struct register_mapping
        {
            WHV_REGISTER_NAME name{WHvArm64RegisterX0};
            size_t size{sizeof(uint64_t)};
            bool zero{};
            bool nzcv{};
        };

        register_mapping map_register(const arm64_register reg)
        {
            const auto id = static_cast<int>(reg);
            if (reg >= arm64_register::bcr0 && reg <= arm64_register::bcr7)
            {
                return {.name = static_cast<WHV_REGISTER_NAME>(WHvArm64RegisterDbgbcr0El1 + id - static_cast<int>(arm64_register::bcr0)),
                        .size = sizeof(uint32_t)};
            }
            if (reg >= arm64_register::bvr0 && reg <= arm64_register::bvr7)
            {
                return {.name = static_cast<WHV_REGISTER_NAME>(WHvArm64RegisterDbgbvr0El1 + id - static_cast<int>(arm64_register::bvr0))};
            }
            if (reg >= arm64_register::wcr0 && reg <= arm64_register::wcr1)
            {
                return {.name = static_cast<WHV_REGISTER_NAME>(WHvArm64RegisterDbgwcr0El1 + id - static_cast<int>(arm64_register::wcr0)),
                        .size = sizeof(uint32_t)};
            }
            if (reg >= arm64_register::wvr0 && reg <= arm64_register::wvr1)
            {
                return {.name = static_cast<WHV_REGISTER_NAME>(WHvArm64RegisterDbgwvr0El1 + id - static_cast<int>(arm64_register::wvr0))};
            }
            if (reg >= arm64_register::x0 && reg <= arm64_register::x28)
            {
                return {.name = static_cast<WHV_REGISTER_NAME>(WHvArm64RegisterX0 + id - static_cast<int>(arm64_register::x0))};
            }
            if (reg >= arm64_register::w0 && reg <= arm64_register::w30)
            {
                return {.name = static_cast<WHV_REGISTER_NAME>(WHvArm64RegisterX0 + id - static_cast<int>(arm64_register::w0)),
                        .size = sizeof(uint32_t)};
            }
            if (reg >= arm64_register::q0 && reg <= arm64_register::q31)
            {
                return {.name = static_cast<WHV_REGISTER_NAME>(WHvArm64RegisterQ0 + id - static_cast<int>(arm64_register::q0)), .size = 16};
            }
            if (reg >= arm64_register::v0 && reg <= arm64_register::v31)
            {
                return {.name = static_cast<WHV_REGISTER_NAME>(WHvArm64RegisterQ0 + id - static_cast<int>(arm64_register::v0)), .size = 16};
            }

            switch (reg)
            {
            case arm64_register::x29:
                return {.name = WHvArm64RegisterFp};
            case arm64_register::x30:
                return {.name = WHvArm64RegisterLr};
            case arm64_register::pc:
                return {.name = WHvArm64RegisterPc};
            case arm64_register::sp:
                return {.name = WHvArm64RegisterSpEl0};
            case arm64_register::wsp:
                return {.name = WHvArm64RegisterSpEl0, .size = sizeof(uint32_t)};
            case arm64_register::xzr:
                return {.size = sizeof(uint64_t), .zero = true};
            case arm64_register::wzr:
                return {.size = sizeof(uint32_t), .zero = true};
            case arm64_register::nzcv:
                return {.name = WHvArm64RegisterPstate, .size = sizeof(uint32_t), .nzcv = true};
            case arm64_register::pstate:
                return {.name = WHvArm64RegisterPstate, .size = sizeof(uint32_t)};
            case arm64_register::fpcr:
                return {.name = WHvArm64RegisterFpcr, .size = sizeof(uint32_t)};
            case arm64_register::fpsr:
                return {.name = WHvArm64RegisterFpsr, .size = sizeof(uint32_t)};
            case arm64_register::tpidr_el0:
                return {.name = WHvArm64RegisterTpidrEl0};
            case arm64_register::tpidrro_el0:
                return {.name = WHvArm64RegisterTpidrroEl0};
            case arm64_register::midr_el1:
                return {.name = WHvArm64RegisterMidrEl1};
            case arm64_register::id_aa64isar0_el1:
                return {.name = WHvArm64RegisterIdAa64Isar0El1};
            case arm64_register::id_aa64isar1_el1:
                return {.name = WHvArm64RegisterIdAa64Isar1El1};
            default:
                throw std::invalid_argument("Unsupported WHP ARM64 register: " + std::to_string(id));
            }
        }

        const auto saved_register_names = [] {
            std::array<WHV_REGISTER_NAME, 91> names;
            names.fill(WHvArm64RegisterX0);
            size_t index{};
            for (int reg = 0; reg < 31; ++reg)
            {
                names[index++] = static_cast<WHV_REGISTER_NAME>(WHvArm64RegisterX0 + reg);
            }
            for (int reg = 0; reg < 32; ++reg)
            {
                names[index++] = static_cast<WHV_REGISTER_NAME>(WHvArm64RegisterQ0 + reg);
            }
            for (const auto reg : {WHvArm64RegisterPc, WHvArm64RegisterPstate, WHvArm64RegisterSpEl0, WHvArm64RegisterTpidrEl0,
                                   WHvArm64RegisterTpidrroEl0, WHvArm64RegisterFpcr, WHvArm64RegisterFpsr, WHvArm64RegisterCpacrEl1})
            {
                names[index++] = reg;
            }
            for (const auto group : {WHvArm64RegisterDbgbcr0El1, WHvArm64RegisterDbgbvr0El1})
            {
                for (int reg = 0; reg < 8; ++reg)
                {
                    names[index++] = static_cast<WHV_REGISTER_NAME>(group + reg);
                }
            }
            for (const auto group : {WHvArm64RegisterDbgwcr0El1, WHvArm64RegisterDbgwvr0El1})
            {
                for (int reg = 0; reg < 2; ++reg)
                {
                    names[index++] = static_cast<WHV_REGISTER_NAME>(group + reg);
                }
            }
            return names;
        }();

        [[noreturn]] emulator_hook* unsupported(const char* operation)
        {
            throw std::runtime_error(std::string("WHP ARM64 does not support ") + operation);
        }

        class whp_arm64_emulator final : public arm64_emulator
        {
          public:
            whp_arm64_emulator()
            {
                WHV_CAPABILITY capability{};
                check_hr(WHvGetCapability(WHvCapabilityCodeHypervisorPresent, &capability, sizeof(capability), nullptr),
                         "WHvGetCapability(HypervisorPresent)");
                if (!capability.HypervisorPresent)
                {
                    throw std::runtime_error("WHP ARM64 requires an enabled Windows hypervisor");
                }

                WHV_PARTITION_PROPERTY property{};
                property.ProcessorCount = 1;
                check_hr(WHvSetPartitionProperty(partition_.value, WHvPartitionPropertyCodeProcessorCount, &property, sizeof(property)),
                         "WHvSetPartitionProperty(ProcessorCount)");
                property = {};
                auto& gic = property.Arm64IcParameters;
                gic.EmulationMode = WHvArm64IcEmulationModeGicV3;
                gic.GicV3Parameters.GicdBaseAddress = gic_gpa;
                gic.GicV3Parameters.GicLpiIntIdBits = 1;
                gic.GicV3Parameters.GicPpiOverflowInterruptFromCntv = 20;
                gic.GicV3Parameters.GicPpiPerformanceMonitorsInterrupt = 0x17;
                check_hr(WHvSetPartitionProperty(partition_.value, WHvPartitionPropertyCodeArm64IcParameters, &property, sizeof(property)),
                         "WHvSetPartitionProperty(Arm64IcParameters)");
                check_hr(WHvSetupPartition(partition_.value), "WHvSetupPartition");
                property = {};
                UINT32 written{};
                check_hr(WHvGetPartitionProperty(partition_.value, WHvPartitionPropertyCodePhysicalAddressWidth, &property,
                                                 sizeof(property), &written),
                         "WHvGetPartitionProperty(PhysicalAddressWidth)");
                gpa_limit_ = property.PhysicalAddressWidth >= 64 ? ~uint64_t{} : 1ull << property.PhysicalAddressWidth;

                vector_page_ = allocate_page();
                // Complete preceding guest stores before the host inspects memory. STR XZR,[SP]
                // then faults at SP_EL1 without clobbering a user register.
                constexpr std::array<uint32_t, 2> report_exception{0xD5033F9F, 0xF90003FF};
                for (size_t offset = 0; offset < 0x800; offset += 0x80)
                {
                    std::memcpy(vector_page_.get() + offset, report_exception.data(), sizeof(report_exception));
                }
                // Refresh stage-1 translations before every entry, then ERET to the user's state.
                constexpr std::array<uint32_t, 5> enter_user{0xD5033F9F, 0xD508831F, 0xD5033F9F, 0xD5033FDF, 0xD69F03E0};
                std::memcpy(vector_page_.get() + 0x800, enter_user.data(), sizeof(enter_user));
                flush_instruction_cache(vector_page_.get(), page_size);
                check_hr(WHvMapGpaRange(partition_.value, vector_page_.get(), vector_gpa, page_size,
                                        WHvMapGpaRangeFlagRead | WHvMapGpaRangeFlagExecute),
                         "WHvMapGpaRange(vector)");
                auto& root = translation_tables_
                                 .emplace(std::pair{0u, uint64_t{0}}, translation_table{.backing = allocate_page(), .gpa = next_table_gpa_})
                                 .first->second;
                check_hr(WHvMapGpaRange(partition_.value, root.backing.get(), root.gpa, page_size,
                                        WHvMapGpaRangeFlagRead | WHvMapGpaRangeFlagWrite),
                         "WHvMapGpaRange(root translation table)");
                next_table_gpa_ += page_size;
                map_translation(vector_gpa, false);
                map_translation(exit_gpa, false);
                processor_ = std::make_unique<processor_handle>(partition_.value);
                constexpr auto debug_feature_name = WHvArm64RegisterIdAa64Dfr0El1;
                WHV_REGISTER_VALUE debug_features{};
                check_hr(WHvGetVirtualProcessorRegisters(partition_.value, WHV_ANY_VP, &debug_feature_name, 1, &debug_features),
                         "WHvGetVirtualProcessorRegisters(ID_AA64DFR0_EL1)");
                breakpoint_count_ = static_cast<size_t>(((debug_features.Reg64 >> 12) & 0xF) + 1);
                watchpoint_count_ = static_cast<size_t>(((debug_features.Reg64 >> 20) & 0xF) + 1);

                set_register(WHvArm64RegisterGicrBaseGpa, gicr_gpa);
                set_register(WHvArm64RegisterPc, 0);
                set_register(WHvArm64RegisterPstate, 0x3C0);
                set_register(WHvArm64RegisterSpEl1, exit_gpa);
                set_register(WHvArm64RegisterVbarEl1, vector_gpa);
                set_register(WHvArm64RegisterMairEl1, 0xFF);
                set_register(WHvArm64RegisterTcrEl1, 16ull | (1ull << 8) | (1ull << 10) | (3ull << 12) | (1ull << 23) | (5ull << 32));
                set_register(WHvArm64RegisterTtbr0El1, translation_tables_.at({0, 0}).gpa);
                set_register(WHvArm64RegisterSctlrEl1, 0x30D01805);
                set_register(WHvArm64RegisterCpacrEl1, 3ull << 20);
                // EL0PCTEN keeps CNTFRQ_EL0 readable; CNTVCT_EL0 reads trap so the counter hook supplies them.
                set_register(WHvArm64RegisterCntkctlEl1, 1);
            }

            ~whp_arm64_emulator() override
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

            whp_arm64_emulator(const whp_arm64_emulator&) = delete;
            whp_arm64_emulator& operator=(const whp_arm64_emulator&) = delete;
            whp_arm64_emulator(whp_arm64_emulator&&) = delete;
            whp_arm64_emulator& operator=(whp_arm64_emulator&&) = delete;

            std::string get_name() const override
            {
                return "whp-arm64";
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

            void flush_host_memory_cache(const void* host, const size_t size) override
            {
                flush_instruction_cache(host, size);
                const auto begin = reinterpret_cast<uintptr_t>(host);
                for (auto& page : pages_ | std::views::values)
                {
                    const auto page_begin = reinterpret_cast<uintptr_t>(page.host);
                    if ((page.flags & WHvMapGpaRangeFlagExecute) &&
                        (page_begin <= begin ? begin - page_begin < page_size : page_begin - begin < size))
                    {
                        remap_executable_page(page);
                    }
                }
            }

            bool has_violation() const override
            {
                return violation_;
            }

            // The exception vectors share the user address space; the GIC and translation table ranges stay
            // reserved as well.
            std::vector<host_reserved_range> reserved_host_ranges() const override
            {
                return {
                    {.address = gic_gpa, .size = gic_size},
                    {.address = gicr_gpa, .size = gicr_size},
                    {.address = page_table_base, .size = page_table_limit - page_table_base},
                    {.address = vector_gpa, .size = 2 * page_size},
                };
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
                {
                    std::lock_guard lock(run_mutex_);
                    if (running_)
                    {
                        throw std::logic_error("WHP ARM64 CPU is already running");
                    }
                    running_ = true;
                    stop_requested_ = false;
                    violation_ = false;
                    stepping_ = count != 0;
                }
                auto finish = utils::finally([this] {
                    std::lock_guard lock(run_mutex_);
                    in_hypervisor_ = false;
                    running_ = false;
                });
                set_register(WHvArm64RegisterOslarEl1, 0);
                auto remaining = count;
                // A hooked instruction whose hooks already ran is stepped once with its page executable. If another
                // thread (TEB in X18) or PC resumes first, protect the page again so that context faults normally.
                if (stepped_page_ && (get_register(WHvArm64RegisterPc).Reg64 != stepped_context_.first ||
                                      get_register(WHvArm64RegisterX18).Reg64 != stepped_context_.second))
                {
                    const auto page = *stepped_page_;
                    stepped_page_.reset();
                    refresh_translation(page);
                }

                for (;;)
                {
                    refresh_mmio();
                    {
                        std::lock_guard lock(run_mutex_);
                        if (stop_requested_)
                        {
                            return;
                        }
                        prepare_user_entry();
                        in_hypervisor_ = true;
                    }
                    WHV_RUN_VP_EXIT_CONTEXT exit{};
                    const auto hr = WHvRunVirtualProcessor(partition_.value, 0, &exit, sizeof(exit));
                    {
                        std::lock_guard lock(run_mutex_);
                        in_hypervisor_ = false;
                    }
                    check_hr(hr, "WHvRunVirtualProcessor");
                    if (exit.ExitReason == WHvRunVpExitReasonCanceled)
                    {
                        std::lock_guard lock(run_mutex_);
                        restore_user_view();
                        if (stop_requested_)
                        {
                            return;
                        }
                        // A cancellation that raced with an ordinary exit stays pending and cancels the
                        // next entry instead; nothing asked this run to stop, so resume the guest.
                        continue;
                    }
                    hook_only_exit_ = false;
                    handle_exit(exit);
                    if (stepping_ && !hook_only_exit_ && --remaining == 0)
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
                    check_hr(WHvCancelRunVirtualProcessor(partition_.value, 0, 0), "WHvCancelRunVirtualProcessor");
                }
            }

            size_t read_raw_register(const int reg, void* value, const size_t size) override
            {
                const auto mapping = map_register(static_cast<arm64_register>(reg));
                validate_register_buffer(value, size, mapping.size);
                WHV_REGISTER_VALUE result{};
                if (!mapping.zero && !absent_debug_register(mapping.name))
                {
                    result = get_register(mapping.name);
                    if (mapping.nzcv)
                    {
                        result.Reg64 &= nzcv_mask;
                    }
                }
                std::memcpy(value, &result, mapping.size);
                return mapping.size;
            }

            size_t write_raw_register(const int reg, const void* value, const size_t size) override
            {
                const auto mapping = map_register(static_cast<arm64_register>(reg));
                validate_register_buffer(value, size, mapping.size);
                if (mapping.name == WHvArm64RegisterMidrEl1 || partition_id_register(mapping.name))
                {
                    throw std::invalid_argument("ARM64 identification registers are read-only");
                }
                if (absent_debug_register(mapping.name))
                {
                    const auto* bytes = static_cast<const std::byte*>(value);
                    if (std::any_of(bytes, bytes + mapping.size, [](const auto byte) { return byte != std::byte{}; }))
                    {
                        throw std::invalid_argument("ARM64 context uses a debug register not implemented by the virtual processor");
                    }
                    return mapping.size;
                }
                if (!mapping.zero)
                {
                    WHV_REGISTER_VALUE result{};
                    std::memcpy(&result, value, mapping.size);
                    if (mapping.nzcv)
                    {
                        result.Reg64 = (get_register(mapping.name).Reg64 & ~nzcv_mask) | (result.Reg64 & nzcv_mask);
                    }
                    else if (mapping.name == WHvArm64RegisterPstate && (result.Reg64 & 0x1F) != 0)
                    {
                        throw std::invalid_argument("WHP ARM64 only exposes an AArch64 EL0 CPU");
                    }
                    check_hr(WHvSetVirtualProcessorRegisters(partition_.value, 0, &mapping.name, 1, &result),
                             "WHvSetVirtualProcessorRegisters");
                }
                return mapping.size;
            }

            std::vector<std::byte> save_registers() const override
            {
                std::array<WHV_REGISTER_VALUE, saved_register_names.size()> values{};
                std::vector<WHV_REGISTER_NAME> names{};
                for (const auto name : saved_register_names)
                {
                    if (!absent_debug_register(name))
                    {
                        names.push_back(name);
                    }
                }
                std::vector<WHV_REGISTER_VALUE> readable(names.size());
                check_hr(
                    WHvGetVirtualProcessorRegisters(partition_.value, 0, names.data(), static_cast<UINT32>(names.size()), readable.data()),
                    "WHvGetVirtualProcessorRegisters(save)");
                size_t index{};
                for (size_t i = 0; i < values.size(); ++i)
                {
                    if (!absent_debug_register(saved_register_names[i]))
                    {
                        values[i] = readable[index++];
                    }
                }
                std::vector<std::byte> bytes(sizeof(values));
                std::memcpy(bytes.data(), values.data(), bytes.size());
                return bytes;
            }

            void restore_registers(const std::vector<std::byte>& bytes) override
            {
                std::array<WHV_REGISTER_VALUE, saved_register_names.size()> values{};
                if (bytes.size() != sizeof(values))
                {
                    throw std::invalid_argument("Invalid WHP ARM64 saved register size");
                }
                std::memcpy(values.data(), bytes.data(), bytes.size());
                if ((values[64].Reg64 & 0x1F) != 0)
                {
                    throw std::invalid_argument("Saved WHP ARM64 CPU is not AArch64 EL0");
                }
                std::vector<WHV_REGISTER_NAME> names{};
                std::vector<WHV_REGISTER_VALUE> writable{};
                for (size_t i = 0; i < values.size(); ++i)
                {
                    if (absent_debug_register(saved_register_names[i]))
                    {
                        const auto* first = reinterpret_cast<const std::byte*>(&values[i]);
                        if (std::any_of(first, first + sizeof(values[i]), [](const auto byte) { return byte != std::byte{}; }))
                        {
                            throw std::invalid_argument("Saved ARM64 registers exceed this virtual processor's debug capabilities");
                        }
                        continue;
                    }
                    names.push_back(saved_register_names[i]);
                    writable.push_back(values[i]);
                }
                check_hr(
                    WHvSetVirtualProcessorRegisters(partition_.value, 0, names.data(), static_cast<UINT32>(names.size()), writable.data()),
                    "WHvSetVirtualProcessorRegisters(restore)");
            }

            void serialize_state(utils::buffer_serializer& buffer, bool) const override
            {
                buffer.write_vector(save_registers());
            }

            void deserialize_state(utils::buffer_deserializer& buffer, bool) override
            {
                restore_registers(buffer.read_vector<std::byte>());
            }

            bool try_read_memory(const uint64_t address, void* data, const size_t size) const override
            {
                return access_memory(address, data, size, false);
            }

            void read_memory(const uint64_t address, void* data, const size_t size) const override
            {
                if (!try_read_memory(address, data, size))
                {
                    throw std::runtime_error("Reading unmapped WHP ARM64 guest memory at " + std::to_string(address));
                }
            }

            bool try_write_memory(const uint64_t address, const void* data, const size_t size) override
            {
                if (!access_memory(address, const_cast<void*>(data), size, true))
                {
                    return false;
                }
                if (size)
                {
                    for (auto current = address & ~(page_size - 1); current < address + size; current += page_size)
                    {
                        auto& page = pages_.at(current);
                        if (page.flags & WHvMapGpaRangeFlagExecute)
                        {
                            remap_executable_page(page);
                        }
                    }
                }
                return true;
            }

            void write_memory(const uint64_t address, const void* data, const size_t size) override
            {
                if (!try_write_memory(address, data, size))
                {
                    throw std::runtime_error("Writing unmapped WHP ARM64 guest memory at " + std::to_string(address));
                }
            }

            emulator_hook* hook_instruction(const int instruction, instruction_hook_callback callback) override
            {
                if (instruction == static_cast<int>(arm64_hookable_instructions::read_counter) && callback)
                {
                    if (counter_hook_)
                    {
                        throw std::logic_error("WHP ARM64 supports one counter instruction hook");
                    }
                    counter_hook_ = std::make_unique<syscall_hook>(syscall_hook{std::move(callback)});
                    return reinterpret_cast<emulator_hook*>(counter_hook_.get());
                }
                if (instruction != static_cast<int>(arm64_hookable_instructions::svc) || !callback)
                {
                    throw std::invalid_argument("WHP ARM64 requires a nonempty SVC instruction hook");
                }
                if (syscall_hook_)
                {
                    throw std::logic_error("WHP ARM64 supports one SVC instruction hook");
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
                if (syscall_hook_ && hook == reinterpret_cast<emulator_hook*>(syscall_hook_.get()))
                {
                    syscall_hook_.reset();
                    return;
                }
                if (counter_hook_ && hook == reinterpret_cast<emulator_hook*>(counter_hook_.get()))
                {
                    counter_hook_.reset();
                    return;
                }
                const auto access = std::ranges::find_if(access_hooks_, [&](const std::unique_ptr<memory_access_hook_callback>& entry) {
                    return reinterpret_cast<emulator_hook*>(entry.get()) == hook;
                });
                if (access != access_hooks_.end())
                {
                    access_hooks_.erase(access);
                    return;
                }
                const auto violation = std::ranges::find_if(violation_hooks_, [&](const std::unique_ptr<violation_hook>& entry) {
                    return reinterpret_cast<emulator_hook*>(entry.get()) == hook;
                });
                if (violation != violation_hooks_.end())
                {
                    violation_hooks_.erase(violation);
                    return;
                }
                const auto execution = std::ranges::find_if(execution_hooks_, [&](const std::unique_ptr<execution_hook>& entry) {
                    return reinterpret_cast<emulator_hook*>(entry.get()) == hook;
                });
                if (execution != execution_hooks_.end())
                {
                    const auto address = (*execution)->address;
                    const auto size = (*execution)->size;
                    execution_hooks_.erase(execution);
                    refresh_translations(address, size);
                    return;
                }
                const auto interrupt = std::ranges::find_if(interrupt_hooks_, [&](const std::unique_ptr<interrupt_hook>& entry) {
                    return reinterpret_cast<emulator_hook*>(entry.get()) == hook;
                });
                if (interrupt == interrupt_hooks_.end())
                {
                    throw std::invalid_argument("Unknown WHP ARM64 hook");
                }
                interrupt_hooks_.erase(interrupt);
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
                    throw std::invalid_argument("WHP ARM64 execution hooks need a nonempty range and callback");
                }
                auto& hook = execution_hooks_.emplace_back(
                    std::make_unique<execution_hook>(execution_hook{.address = address, .size = size, .callback = std::move(callback)}));
                refresh_translations(address, size);
                return reinterpret_cast<emulator_hook*>(hook.get());
            }

            // Like the x86-64 WHP backend, data access hooks are accepted but not reported: trapping every
            // access to a watched page (the TEB, for example) would fault on nearly every instruction.
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
                    throw std::invalid_argument("WHP ARM64 requires a nonempty interrupt hook");
                }
                auto& hook = interrupt_hooks_.emplace_back(std::make_unique<interrupt_hook>(interrupt_hook{std::move(callback)}));
                return reinterpret_cast<emulator_hook*>(hook.get());
            }

            emulator_hook* hook_memory_violation(memory_violation_hook_callback callback) override
            {
                if (!callback)
                {
                    throw std::invalid_argument("WHP ARM64 requires a nonempty memory violation hook");
                }
                auto& hook = violation_hooks_.emplace_back(std::make_unique<violation_hook>(violation_hook{std::move(callback)}));
                return reinterpret_cast<emulator_hook*>(hook.get());
            }

            emulator_hook* hook_basic_block(basic_block_hook_callback) override
            {
                return unsupported("basic block hooks");
            }

          private:
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

            // WHP must release the VP and partition before the backing allocations.
            std::map<uint64_t, mapped_page> pages_{};
            std::map<std::pair<unsigned, uint64_t>, translation_table> translation_tables_{};
            uint64_t next_table_gpa_{page_table_base};
            std::vector<uint64_t> free_table_gpas_{};
            uint64_t gpa_limit_{};
            uint64_t next_gpa_{gpa_pool_base};
            std::vector<uint64_t> free_gpas_{};
            std::shared_ptr<std::byte> vector_page_{};
            partition_handle partition_{};
            std::unique_ptr<processor_handle> processor_{};
            std::map<uint64_t, std::shared_ptr<mmio_region>> mmio_regions_{};
            std::unique_ptr<syscall_hook> syscall_hook_{};
            std::unique_ptr<syscall_hook> counter_hook_{};
            std::vector<std::unique_ptr<interrupt_hook>> interrupt_hooks_{};
            std::vector<std::unique_ptr<violation_hook>> violation_hooks_{};
            std::vector<std::unique_ptr<execution_hook>> execution_hooks_{};
            std::vector<std::unique_ptr<memory_access_hook_callback>> access_hooks_{};
            std::optional<uint64_t> stepped_page_{};
            std::pair<uint64_t, uint64_t> stepped_context_{};
            bool user_step_{};
            bool hook_only_exit_{};
            bool destroying_{};
            std::mutex run_mutex_{};
            bool running_{};
            bool in_hypervisor_{};
            bool stop_requested_{};
            bool violation_{};
            bool stepping_{};
            size_t breakpoint_count_{};
            size_t watchpoint_count_{};

            bool absent_debug_register(const WHV_REGISTER_NAME reg) const
            {
                // ARM64_NT_CONTEXT contains fixed maximum slots; absent hardware slots are inactive.
                for (const auto first : {WHvArm64RegisterDbgbcr0El1, WHvArm64RegisterDbgbvr0El1})
                {
                    if (reg >= first && reg < first + 16)
                    {
                        return static_cast<size_t>(reg - first) >= breakpoint_count_;
                    }
                }
                for (const auto first : {WHvArm64RegisterDbgwcr0El1, WHvArm64RegisterDbgwvr0El1})
                {
                    if (reg >= first && reg < first + 16)
                    {
                        return static_cast<size_t>(reg - first) >= watchpoint_count_;
                    }
                }
                return false;
            }

            static bool partition_id_register(const WHV_REGISTER_NAME reg)
            {
                return reg == WHvArm64RegisterIdAa64Isar0El1 || reg == WHvArm64RegisterIdAa64Isar1El1;
            }

            WHV_REGISTER_VALUE get_register(const WHV_REGISTER_NAME reg) const
            {
                WHV_REGISTER_VALUE result{};
                // Feature ID registers are partition-wide and only readable through WHV_ANY_VP; MIDR is per processor.
                const auto processor = partition_id_register(reg) ? WHV_ANY_VP : 0;
                check_hr(WHvGetVirtualProcessorRegisters(partition_.value, processor, &reg, 1, &result), "WHvGetVirtualProcessorRegisters");
                return result;
            }

            void set_register(const WHV_REGISTER_NAME reg, const uint64_t value) const
            {
                WHV_REGISTER_VALUE result{};
                result.Reg64 = value;
                check_hr(WHvSetVirtualProcessorRegisters(partition_.value, 0, &reg, 1, &result), "WHvSetVirtualProcessorRegisters");
            }

            static uint64_t* table_entries(const translation_table& table)
            {
                return reinterpret_cast<uint64_t*>(table.backing.get());
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

            translation_table& leaf_table(const uint64_t address)
            {
                auto* table = &translation_tables_.at(table_key(0, address));
                for (unsigned level = 1; level <= 3; ++level)
                {
                    auto found = translation_tables_.find(table_key(level, address));
                    if (found == translation_tables_.end())
                    {
                        if (free_table_gpas_.empty() && next_table_gpa_ >= page_table_limit)
                        {
                            throw std::runtime_error("WHP ARM64 translation table space exhausted");
                        }
                        const auto gpa = free_table_gpas_.empty() ? next_table_gpa_ : free_table_gpas_.back();
                        // Insert ownership before exposing the backing allocation to WHP.
                        found = translation_tables_
                                    .emplace(table_key(level, address), translation_table{.backing = allocate_page(), .gpa = gpa})
                                    .first;
                        const auto hr = WHvMapGpaRange(partition_.value, found->second.backing.get(), gpa, page_size,
                                                       WHvMapGpaRangeFlagRead | WHvMapGpaRangeFlagWrite);
                        if (FAILED(hr))
                        {
                            translation_tables_.erase(found);
                            throw_hr(hr, "WHvMapGpaRange(translation table)");
                        }
                        if (free_table_gpas_.empty())
                        {
                            next_table_gpa_ += page_size;
                        }
                        else
                        {
                            free_table_gpas_.pop_back();
                        }
                        table_entries(*table)[table_index(level - 1, address)] = gpa | 3;
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
                auto& slot = table_entries(leaf)[table_index(3, address)];
                if (!slot)
                {
                    ++leaf.used;
                }
                slot = entry;
            }

            // Tables left empty are unmapped and their GPAs reused, so long-running guests that touch many
            // distinct regions do not exhaust the private table pool.
            void clear_translation(const uint64_t address)
            {
                for (unsigned level = 3;; --level)
                {
                    const auto table = translation_tables_.find(table_key(level, address));
                    if (table == translation_tables_.end())
                    {
                        return;
                    }
                    auto& slot = table_entries(table->second)[table_index(level, address)];
                    if (!slot)
                    {
                        return;
                    }
                    slot = 0;
                    if (level == 0 || --table->second.used != 0)
                    {
                        return;
                    }
                    const auto hr = WHvUnmapGpaRange(partition_.value, table->second.gpa, page_size);
                    if (FAILED(hr))
                    {
                        log_cleanup_failure(hr, "WHvUnmapGpaRange(translation table)");
                        return;
                    }
                    free_table_gpas_.push_back(table->second.gpa);
                    translation_tables_.erase(table);
                }
            }

            void map_translation(const uint64_t address, const bool user)
            {
                // Normal WB memory with AF and inner-shareable attributes avoids the incompatible
                // Device-vs-host-WB aliases created by running a user guest with the MMU disabled.
                set_translation(address, address | 3 | (1ull << 10) | (3ull << 8) | (user ? (1ull << 6) : (1ull << 54)));
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

            // Stage 1 carries the EL0 permissions, so every user fault is a synchronous abort with ESR/FAR.
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
                if (!executable || (stepped_page_ != address && has_execution_hook(address)))
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

            void prepare_user_entry()
            {
                const auto pstate = get_register(WHvArm64RegisterPstate).Reg64;
                user_step_ = (pstate & software_step_bit) != 0;
                auto user_pstate = pstate & ~software_step_bit;
                const auto step = stepping_ || stepped_page_.has_value() || user_step_;
                set_register(WHvArm64RegisterMdscrEl1, step ? (1ull << 15) | 1 : 0);
                if (step)
                {
                    user_pstate = (user_pstate & ~(1ull << 9)) | software_step_bit;
                }
                set_register(WHvArm64RegisterElrEl1, get_register(WHvArm64RegisterPc).Reg64);
                set_register(WHvArm64RegisterSpsrEl1, user_pstate);
                set_register(WHvArm64RegisterPc, resume_gpa);
                set_register(WHvArm64RegisterPstate, 0x3C5);
            }

            void remap_executable_page(mapped_page& page) const
            {
                // WHP can retain an old instruction after host edits even with host/guest I-cache
                // flushes. Re-establishing the executable GPA mapping makes the new bytes visible.
                const auto flags = page.flags;
                check_hr(WHvUnmapGpaRange(partition_.value, page.gpa, page_size), "WHvUnmapGpaRange(code write)");
                page.flags = 0;
                check_hr(WHvMapGpaRange(partition_.value, page.host, page.gpa, page_size, static_cast<WHV_MAP_GPA_RANGE_FLAGS>(flags)),
                         "WHvMapGpaRange(code write)");
                page.flags = flags;
            }

            uint64_t allocate_gpa()
            {
                if (!free_gpas_.empty())
                {
                    const auto gpa = free_gpas_.back();
                    free_gpas_.pop_back();
                    return gpa;
                }
                if (next_gpa_ >= gpa_limit_)
                {
                    throw std::runtime_error("WHP ARM64 partition physical address space is exhausted");
                }
                const auto gpa = next_gpa_;
                next_gpa_ += page_size;
                return gpa;
            }

            void release_gpa(const uint64_t gpa)
            {
                if (gpa)
                {
                    free_gpas_.push_back(gpa);
                }
            }

            void restore_user_view()
            {
                const auto pc = get_register(WHvArm64RegisterPc).Reg64;
                if (pc >= vector_gpa && pc < vector_gpa + page_size && (get_register(WHvArm64RegisterPstate).Reg64 & 0x1F) == 5)
                {
                    auto return_pc = get_register(WHvArm64RegisterElrEl1).Reg64;
                    if (pc < resume_gpa && ((get_register(WHvArm64RegisterEsrEl1).Reg64 >> 26) & 0x3F) == 0x15 && return_pc >= 4)
                    {
                        return_pc -= 4;
                    }
                    set_register(WHvArm64RegisterPc, return_pc);
                    // Canceled before the ERET: a requested user step has not run yet.
                    const auto pending_step = pc >= resume_gpa && user_step_ ? software_step_bit : 0;
                    set_register(WHvArm64RegisterPstate, (get_register(WHvArm64RegisterSpsrEl1).Reg64 & ~software_step_bit) | pending_step);
                }
                else if (!user_step_)
                {
                    // Canceled at EL0 after the ERET: PSTATE.SS may still hold the backend's own step request.
                    const auto pstate = get_register(WHvArm64RegisterPstate).Reg64;
                    if (pstate & software_step_bit)
                    {
                        set_register(WHvArm64RegisterPstate, pstate & ~software_step_bit);
                    }
                }
            }

            static void validate_register_buffer(const void* buffer, const size_t size, const size_t required)
            {
                if (!buffer || size < required)
                {
                    throw std::invalid_argument("WHP ARM64 register buffer is too small or null");
                }
            }

            void handle_exit(const WHV_RUN_VP_EXIT_CONTEXT& exit)
            {
                const auto pc = get_register(WHvArm64RegisterPc).Reg64;
                const bool vector_exit = exit.ExitReason == WHvRunVpExitReasonUnmappedGpa && exit.MemoryAccess.Gpa == exit_gpa &&
                                         pc >= vector_gpa && pc < vector_gpa + 0x800 && (pc - vector_gpa) % 0x80 == 4 &&
                                         (get_register(WHvArm64RegisterPstate).Reg64 & 0x1F) == 5;
                const auto esr = get_register(WHvArm64RegisterEsrEl1).Reg64;
                if (!vector_exit)
                {
                    violation_ = true;
                    std::ostringstream error{};
                    error << "Unhandled WHP ARM64 exit 0x" << std::hex << static_cast<uint32_t>(exit.ExitReason) << " at PC 0x" << pc;
                    if (exit.ExitReason == WHvRunVpExitReasonUnmappedGpa || exit.ExitReason == WHvRunVpExitReasonGpaIntercept)
                    {
                        error << ", GPA 0x" << exit.MemoryAccess.Gpa;
                    }
                    throw std::runtime_error(error.str());
                }

                const auto return_pc = get_register(WHvArm64RegisterElrEl1).Reg64;
                const auto user_pstate = get_register(WHvArm64RegisterSpsrEl1).Reg64 & ~software_step_bit;
                const auto exception_class = static_cast<int>((esr >> 26) & 0x3F);
                const auto from_user = pc == vector_gpa + user_vector_offset && (user_pstate & 0x1F) == 0;
                // Any exit consumes a user step; only a completed step is reported.
                const auto user_step = std::exchange(user_step_, false);
                const auto hooked_step = stepped_page_.has_value();
                if (hooked_step)
                {
                    // The hooked instruction has run (or raised its own exception); protect its page again.
                    const auto page = *stepped_page_;
                    stepped_page_.reset();
                    refresh_translation(page);
                }
                if (from_user && exception_class == software_step_class && (stepping_ || hooked_step || user_step))
                {
                    set_register(WHvArm64RegisterPstate, user_pstate);
                    set_register(WHvArm64RegisterPc, return_pc);
                    if (user_step)
                    {
                        if (interrupt_hooks_.empty())
                        {
                            throw std::runtime_error("WHP ARM64 software step without an interrupt hook");
                        }
                        report_exception(static_cast<int>(arm64_exception_class::software_step));
                    }
                    return;
                }
                if (from_user && (exception_class == instruction_abort_class || exception_class == data_abort_class) &&
                    handle_abort(exception_class, esr, return_pc, user_pstate, user_step))
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
                if (from_user && !interrupt_hooks_.empty() &&
                    (exception_class == static_cast<int>(arm64_exception_class::undefined_instruction) ||
                     exception_class == static_cast<int>(arm64_exception_class::breakpoint_instruction)))
                {
                    // ELR_EL1 holds the faulting instruction for both classes.
                    set_register(WHvArm64RegisterPstate, user_pstate);
                    set_register(WHvArm64RegisterPc, return_pc);
                    report_exception(exception_class);
                    return;
                }
                if (!from_user || exception_class != svc_class || return_pc < 4)
                {
                    violation_ = true;
                    std::ostringstream error{};
                    error << "Unhandled WHP ARM64 guest exception: ESR_EL1=0x" << std::hex << esr << ", ELR_EL1=0x" << return_pc
                          << ", FAR_EL1=0x" << get_register(WHvArm64RegisterFarEl1).Reg64;
                    restore_user_view();
                    throw std::runtime_error(error.str());
                }
                uint32_t instruction{};
                read_memory(return_pc - 4, &instruction, sizeof(instruction));
                if ((instruction & 0xFFE0001F) != 0xD4000001 || ((instruction >> 5) & 0xFFFF) != (esr & 0xFFFF))
                {
                    violation_ = true;
                    restore_user_view();
                    std::ostringstream error{};
                    error << "WHP ARM64 SVC instruction does not match ESR_EL1: instruction=0x" << std::hex << instruction << ", ESR_EL1=0x"
                          << esr << ", ELR_EL1=0x" << return_pc;
                    throw std::runtime_error(error.str());
                }

                // Hooks see the SVC address in PC, as x86 hooks see SYSCALL in RIP. Restore EL0
                // before invoking the callback so stop/save/register access never exposes EL1.
                set_register(WHvArm64RegisterPstate, user_pstate);
                set_register(WHvArm64RegisterPc, return_pc - 4);
                if (!syscall_hook_)
                {
                    throw std::runtime_error("WHP ARM64 guest issued SVC without a syscall hook");
                }
                auto callback = syscall_hook_->callback;
                const auto continuation = callback(*this, (instruction >> 5) & 0xFFFF);
                switch (continuation)
                {
                case instruction_hook_continuation::skip_instruction:
                    set_register(WHvArm64RegisterPc, get_register(WHvArm64RegisterPc).Reg64 + 4);
                    return;
                case instruction_hook_continuation::finalized_instruction_pointer:
                    return;
                case instruction_hook_continuation::run_instruction:
                    throw std::runtime_error("WHP ARM64 SVC hooks must handle the syscall (skip or finalized PC)");
                default:
                    throw std::runtime_error("Invalid WHP ARM64 SVC hook continuation");
                }
            }

            void report_exception(const int exception_class)
            {
                std::vector<interrupt_hook_callback> callbacks{};
                callbacks.reserve(interrupt_hooks_.size());
                for (const auto& hook : interrupt_hooks_)
                {
                    callbacks.push_back(hook->callback);
                }
                for (const auto& callback : callbacks)
                {
                    callback(*this, exception_class);
                }
            }

            bool handle_abort(const int exception_class, const uint64_t esr, const uint64_t return_pc, const uint64_t user_pstate,
                              const bool user_step)
            {
                const auto status = esr & 0x3C;
                const auto translation_fault = status == 0x04;
                const auto permission_fault = status == 0x0C;
                if (!translation_fault && !permission_fault)
                {
                    return false;
                }

                const auto instruction_abort = exception_class == instruction_abort_class;
                const auto address = get_register(WHvArm64RegisterFarEl1).Reg64;
                const auto page_address = address & ~(page_size - 1);
                const auto page = pages_.find(page_address);
                const auto hooked_execution = instruction_abort && permission_fault && page != pages_.end() &&
                                              is_executable(page->second.permissions) && has_execution_hook(page_address);
                if (!hooked_execution && violation_hooks_.empty())
                {
                    return false;
                }

                set_register(WHvArm64RegisterPc, return_pc);

                if (hooked_execution)
                {
                    // The hooked instruction has not run yet, so a requested user step is still pending.
                    set_register(WHvArm64RegisterPstate, user_pstate | (user_step ? software_step_bit : 0));
                    std::vector<memory_execution_hook_callback> callbacks{};
                    for (const auto& hook : execution_hooks_)
                    {
                        if (return_pc >= hook->address && return_pc - hook->address < hook->size)
                        {
                            callbacks.push_back(hook->callback);
                        }
                    }
                    for (const auto& callback : callbacks)
                    {
                        callback(*this, return_pc);
                    }
                    if (get_register(WHvArm64RegisterPc).Reg64 == return_pc && has_execution_hook(page_address))
                    {
                        stepped_page_ = page_address;
                        stepped_context_ = {return_pc, get_register(WHvArm64RegisterX18).Reg64};
                    }
                    refresh_translation(page_address);
                    // No guest instruction completed, so the exit does not consume a step.
                    hook_only_exit_ = true;
                    return true;
                }

                set_register(WHvArm64RegisterPstate, user_pstate);

                auto operation = memory_operation::exec;
                size_t size = sizeof(uint32_t);
                if (!instruction_abort)
                {
                    operation = ((esr >> 6) & 1) != 0 ? memory_operation::write : memory_operation::read;
                    size = ((esr >> 24) & 1) != 0 ? size_t{1} << ((esr >> 22) & 3) : 0;
                }
                const auto type = translation_fault ? memory_violation_type::unmapped : memory_violation_type::protection;

                std::vector<memory_violation_hook_callback> callbacks{};
                callbacks.reserve(violation_hooks_.size());
                for (const auto& hook : violation_hooks_)
                {
                    callbacks.push_back(hook->callback);
                }
                for (const auto& callback : callbacks)
                {
                    if (callback(*this, address, size, operation, type) == memory_violation_continuation::stop)
                    {
                        violation_ = true;
                        std::ostringstream error{};
                        error << "WHP ARM64 memory violation at 0x" << std::hex << address << " was not handled (PC 0x" << return_pc << ")";
                        throw std::runtime_error(error.str());
                    }
                }
                return true;
            }

            bool handle_counter_read(const uint64_t return_pc, const uint64_t user_pstate, const bool user_step)
            {
                uint32_t instruction{};
                if (!counter_hook_ || !try_read_memory(return_pc, &instruction, sizeof(instruction)))
                {
                    return false;
                }
                const auto encoding = instruction & ~0x1Fu;
                if (encoding != mrs_virtual_counter && encoding != mrs_cycle_counter)
                {
                    return false;
                }

                set_register(WHvArm64RegisterPstate, user_pstate);
                set_register(WHvArm64RegisterPc, return_pc);
                auto callback = counter_hook_->callback;
                switch (callback(*this, static_cast<uint64_t>(general_register(instruction & 0x1F))))
                {
                case instruction_hook_continuation::skip_instruction:
                    set_register(WHvArm64RegisterPc, get_register(WHvArm64RegisterPc).Reg64 + 4);
                    break;
                case instruction_hook_continuation::finalized_instruction_pointer:
                    break;
                default:
                    throw std::runtime_error("WHP ARM64 counter hooks must supply the value (skip or finalized PC)");
                }

                // The counter read completed the stepped instruction.
                if (user_step && !interrupt_hooks_.empty())
                {
                    report_exception(static_cast<int>(arm64_exception_class::software_step));
                }
                return true;
            }

            static void validate_range(const uint64_t address, const size_t size)
            {
                if (!size || address % page_size != 0 || size % page_size != 0 || address >= user_address_limit ||
                    size > user_address_limit - address)
                {
                    throw std::invalid_argument("WHP ARM64 mappings must be nonempty, page-aligned, and within the 48-bit address space");
                }
                const auto end = address + size;
                if ((address < exit_gpa + page_size && end > vector_gpa) || (address < gic_gpa + gic_size && end > gic_gpa) ||
                    (address < gicr_gpa + gicr_size && end > gicr_gpa) || (address < page_table_limit && end > page_table_base))
                {
                    throw std::invalid_argument("WHP ARM64 mapping overlaps the private exception or GIC region");
                }
            }

            static UINT32 map_flags(const memory_permission permissions)
            {
                UINT32 flags{};
                if (is_readable(permissions))
                {
                    flags |= WHvMapGpaRangeFlagRead;
                }
                if (is_writable(permissions))
                {
                    flags |= WHvMapGpaRangeFlagWrite;
                }
                if (is_executable(permissions))
                {
                    flags |= WHvMapGpaRangeFlagExecute;
                }
                return flags;
            }

            void map_memory(const uint64_t address, const size_t size, const memory_permission permissions) override
            {
                map_pages(address, size, nullptr, permissions);
            }

            bool map_host_memory(const uint64_t address, const size_t size, void* host, const memory_permission permissions) override
            {
                if (!host || reinterpret_cast<uintptr_t>(host) % page_size != 0)
                {
                    throw std::invalid_argument("WHP ARM64 host backing must be nonnull and page-aligned");
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
                    throw std::invalid_argument("WHP ARM64 guest mapping overlaps an existing page");
                }
                std::map<uint64_t, mapped_page> pending{};
                auto rollback = utils::finally([&] {
                    for (auto it = pending.begin(); it != pending.end();)
                    {
                        if (it->second.flags)
                        {
                            const auto hr = WHvUnmapGpaRange(partition_.value, it->second.gpa, page_size);
                            if (FAILED(hr))
                            {
                                log_cleanup_failure(hr, "WHvUnmapGpaRange(rollback)");
                                // Retain backing until partition teardown if WHP cannot unmap it.
                                pages_.insert(pending.extract(it++));
                                continue;
                            }
                        }
                        clear_translation(it->first);
                        release_gpa(it->second.gpa);
                        it = pending.erase(it);
                    }
                });
                const auto flags = map_flags(permissions);
                for (size_t offset = 0; offset < size; offset += page_size)
                {
                    mapped_page page{};
                    if (host)
                    {
                        page.host = host + offset;
                    }
                    else
                    {
                        page.owned = allocate_page();
                        page.host = page.owned.get();
                    }
                    page.permissions = permissions;
                    auto& inserted = pending.emplace(address + offset, std::move(page)).first->second;
                    inserted.gpa = allocate_gpa();
                    if (flags)
                    {
                        if (flags & WHvMapGpaRangeFlagExecute)
                        {
                            flush_instruction_cache(inserted.host, page_size);
                        }
                        check_hr(WHvMapGpaRange(partition_.value, inserted.host, inserted.gpa, page_size,
                                                static_cast<WHV_MAP_GPA_RANGE_FLAGS>(flags)),
                                 "WHvMapGpaRange");
                        inserted.flags = flags;
                    }
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
                        throw std::invalid_argument("WHP ARM64 range contains an unmapped page");
                    }
                }
            }

            void unmap_memory(const uint64_t address, const size_t size) override
            {
                require_mapped_range(address, size);
                for (uint64_t current = address; current < address + size; current += page_size)
                {
                    const auto& page = pages_.at(current);
                    if (page.flags)
                    {
                        check_hr(WHvUnmapGpaRange(partition_.value, page.gpa, page_size), "WHvUnmapGpaRange");
                    }
                    clear_translation(current);
                    if (page.mmio)
                    {
                        mmio_regions_.erase(page.mmio->address);
                    }
                    release_gpa(page.gpa);
                    pages_.erase(current);
                }
            }

            void apply_memory_protection(const uint64_t address, const size_t size, const memory_permission permissions) override
            {
                require_mapped_range(address, size);
                const auto flags = map_flags(permissions);
                if (flags & WHvMapGpaRangeFlagExecute)
                {
                    for (uint64_t current = address; current < address + size; current += page_size)
                    {
                        flush_instruction_cache(pages_.at(current).host, page_size);
                    }
                }
                for (uint64_t current = address; current < address + size; current += page_size)
                {
                    auto& page = pages_.at(current);
                    if (page.flags == flags)
                    {
                        continue;
                    }
                    const auto old_flags = page.flags;
                    if (old_flags)
                    {
                        check_hr(WHvUnmapGpaRange(partition_.value, page.gpa, page_size), "WHvUnmapGpaRange(protect)");
                        page.flags = 0;
                    }
                    if (flags)
                    {
                        const auto hr =
                            WHvMapGpaRange(partition_.value, page.host, page.gpa, page_size, static_cast<WHV_MAP_GPA_RANGE_FLAGS>(flags));
                        if (FAILED(hr))
                        {
                            if (old_flags)
                            {
                                check_hr(WHvMapGpaRange(partition_.value, page.host, page.gpa, page_size,
                                                        static_cast<WHV_MAP_GPA_RANGE_FLAGS>(old_flags)),
                                         "WHvMapGpaRange(protect rollback)");
                                page.flags = old_flags;
                            }
                            throw_hr(hr, "WHvMapGpaRange(protect)");
                        }
                    }
                    page.flags = flags;
                }
                for (uint64_t current = address; current < address + size; current += page_size)
                {
                    auto& page = pages_.at(current);
                    page.permissions = permissions;
                    set_translation(current, user_translation(current, page));
                }
            }

            // WHP ARM64 has no MMIO exits for EL0 loads, so an MMIO range is a read-only page that is refreshed
            // from the read callback before every guest entry. Host accesses go to the callbacks directly.
            void map_mmio(const uint64_t address, const size_t size, mmio_read_callback read_cb, mmio_write_callback write_cb) override
            {
                if (!read_cb)
                {
                    throw std::invalid_argument("WHP ARM64 MMIO requires a read callback");
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
                    const auto count = static_cast<size_t>((std::min)(page_size - offset, end - current));
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
                        if (pages_.at(current - offset).flags & WHvMapGpaRangeFlagExecute)
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
    }

    std::unique_ptr<arm64_emulator> create_arm64_emulator(const size_t vcpu_count)
    {
        if (vcpu_count != 1)
        {
            throw std::invalid_argument("WHP ARM64 requires exactly one vCPU");
        }
        return std::make_unique<whp_arm64_emulator>();
    }
}
