#define WHP_EMULATOR_IMPL
#include "whp_x86_64_emulator.hpp"

#include <WinHvPlatform.h>
#include <windows.h>

#include <array>
#include <cstring>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include <utils/object.hpp>

namespace whp
{
    namespace
    {
        constexpr UINT32 vp_index = 0;
        constexpr uint64_t page_size = 0x1000;
        constexpr uint64_t page_table_entry_present = 1ull << 0;
        constexpr uint64_t page_table_entry_writable = 1ull << 1;
        constexpr uint64_t page_table_entry_user = 1ull << 2;
        constexpr uint64_t page_table_entry_address_mask = 0x000FFFFFFFFFF000ull;
        constexpr uint64_t internal_page_table_base = 0x0000007000000000ull;

        uint64_t align_down_to_page(const uint64_t value)
        {
            return value & ~(page_size - 1);
        }

        bool is_page_aligned(const uint64_t value)
        {
            return (value % page_size) == 0;
        }

        [[noreturn]] void throw_hr(const HRESULT hr, const char* action)
        {
            std::ostringstream stream;
            stream << action << " failed with HRESULT 0x" << std::hex << std::setw(8) << std::setfill('0') << static_cast<uint32_t>(hr);
            throw std::runtime_error(stream.str());
        }

#define WHP_CHECK_HR(expr)          \
    do                              \
    {                               \
        const HRESULT hr_ = (expr); \
        if (FAILED(hr_))            \
        {                           \
            throw_hr(hr_, #expr);   \
        }                           \
    } while (false)

        class partition_handle
        {
          public:
            partition_handle()
            {
                WHP_CHECK_HR(WHvCreatePartition(&this->handle_));
            }

            partition_handle(const partition_handle&) = delete;
            partition_handle& operator=(const partition_handle&) = delete;

            ~partition_handle()
            {
                if (this->handle_ != nullptr)
                {
                    (void)WHvDeletePartition(this->handle_);
                }
            }

            operator WHV_PARTITION_HANDLE() const
            {
                return this->handle_;
            }

          private:
            WHV_PARTITION_HANDLE handle_ = nullptr;
        };

        class virtual_processor_handle
        {
          public:
            explicit virtual_processor_handle(const WHV_PARTITION_HANDLE partition)
                : partition_(partition)
            {
                WHP_CHECK_HR(WHvCreateVirtualProcessor(this->partition_, vp_index, 0));
            }

            virtual_processor_handle(const virtual_processor_handle&) = delete;
            virtual_processor_handle& operator=(const virtual_processor_handle&) = delete;

            ~virtual_processor_handle()
            {
                if (this->partition_ != nullptr)
                {
                    (void)WHvDeleteVirtualProcessor(this->partition_, vp_index);
                }
            }

          private:
            WHV_PARTITION_HANDLE partition_ = nullptr;
        };

        struct mapped_page
        {
            struct virtual_free_deleter
            {
                void operator()(uint8_t* page) const
                {
                    if (page != nullptr)
                    {
                        ::VirtualFree(page, 0, MEM_RELEASE);
                    }
                }
            };

            void* host_page = nullptr;
            uint32_t map_flags = 0;
            memory_permission permissions = memory_permission::none;
            std::shared_ptr<uint8_t> owned_page{};
        };

        std::shared_ptr<uint8_t> allocate_backing_memory(const size_t size)
        {
            auto* raw_memory = static_cast<uint8_t*>(::VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
            if (raw_memory == nullptr)
            {
                throw std::runtime_error("VirtualAlloc failed while backing guest memory");
            }

            std::memset(raw_memory, 0, size);
            return std::shared_ptr<uint8_t>(raw_memory, mapped_page::virtual_free_deleter{});
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

        struct register_mapping
        {
            WHV_REGISTER_NAME name{};
            register_kind kind = register_kind::reg64;
            size_t logical_size = sizeof(uint64_t);
        };

        struct gp_register_access
        {
            size_t offset = 0;
            size_t width = sizeof(uint64_t);
            bool zero_extend_32 = false;
        };

        std::optional<gp_register_access> classify_gp_register_access(const x86_register reg)
        {
            switch (reg)
            {
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
                return gp_register_access{.offset = 0, .width = sizeof(uint8_t)};
            case x86_register::ah:
            case x86_register::bh:
            case x86_register::ch:
            case x86_register::dh:
                return gp_register_access{.offset = 1, .width = sizeof(uint8_t)};
            case x86_register::ax:
            case x86_register::bx:
            case x86_register::cx:
            case x86_register::dx:
            case x86_register::si:
            case x86_register::di:
            case x86_register::bp:
            case x86_register::sp:
            case x86_register::r8w:
            case x86_register::r9w:
            case x86_register::r10w:
            case x86_register::r11w:
            case x86_register::r12w:
            case x86_register::r13w:
            case x86_register::r14w:
            case x86_register::r15w:
            case x86_register::ip:
            case x86_register::flags:
                return gp_register_access{.offset = 0, .width = sizeof(uint16_t)};
            case x86_register::eax:
            case x86_register::ebx:
            case x86_register::ecx:
            case x86_register::edx:
            case x86_register::esi:
            case x86_register::edi:
            case x86_register::ebp:
            case x86_register::esp:
            case x86_register::r8d:
            case x86_register::r9d:
            case x86_register::r10d:
            case x86_register::r11d:
            case x86_register::r12d:
            case x86_register::r13d:
            case x86_register::r14d:
            case x86_register::r15d:
            case x86_register::eip:
            case x86_register::eflags:
                return gp_register_access{.offset = 0, .width = sizeof(uint32_t), .zero_extend_32 = true};
            case x86_register::rax:
            case x86_register::rbx:
            case x86_register::rcx:
            case x86_register::rdx:
            case x86_register::rsi:
            case x86_register::rdi:
            case x86_register::rbp:
            case x86_register::rsp:
            case x86_register::r8:
            case x86_register::r9:
            case x86_register::r10:
            case x86_register::r11:
            case x86_register::r12:
            case x86_register::r13:
            case x86_register::r14:
            case x86_register::r15:
            case x86_register::rip:
            case x86_register::rflags:
                return gp_register_access{.offset = 0, .width = sizeof(uint64_t)};
            default:
                return std::nullopt;
            }
        }

        uint32_t to_whp_map_flags(const memory_permission permissions)
        {
            uint32_t flags = 0;

            if ((permissions & memory_permission::read) != memory_permission::none)
            {
                flags |= WHvMapGpaRangeFlagRead;
            }

            if ((permissions & memory_permission::write) != memory_permission::none)
            {
                flags |= WHvMapGpaRangeFlagWrite;
            }

            if ((permissions & memory_permission::exec) != memory_permission::none)
            {
                flags |= WHvMapGpaRangeFlagExecute;
            }

            return flags;
        }

        WHV_X64_SEGMENT_REGISTER make_segment(const uint16_t selector, const bool is_code)
        {
            WHV_X64_SEGMENT_REGISTER segment{};
            segment.Base = 0;
            segment.Limit = 0xFFFFF;
            segment.Selector = selector;
            segment.SegmentType = is_code ? 0xB : 0x3;
            segment.NonSystemSegment = 1;
            segment.DescriptorPrivilegeLevel = selector == 0x33 || selector == 0x2B || selector == 0x53 ? 3 : 0;
            segment.Present = 1;
            segment.Long = is_code ? 1 : 0;
            segment.Default = is_code ? 0 : 1;
            segment.Granularity = 1;
            return segment;
        }

        memory_operation map_memory_operation(const WHV_MEMORY_ACCESS_TYPE type)
        {
            switch (type)
            {
            case WHvMemoryAccessRead:
                return memory_operation::read;
            case WHvMemoryAccessWrite:
                return memory_operation::write;
            case WHvMemoryAccessExecute:
                return memory_operation::exec;
            default:
                return memory_operation::none;
            }
        }

        const char* exit_reason_name(const WHV_RUN_VP_EXIT_REASON reason)
        {
            switch (reason)
            {
            case WHvRunVpExitReasonNone:
                return "None";
            case WHvRunVpExitReasonMemoryAccess:
                return "MemoryAccess";
            case WHvRunVpExitReasonX64IoPortAccess:
                return "X64IoPortAccess";
            case WHvRunVpExitReasonUnrecoverableException:
                return "UnrecoverableException";
            case WHvRunVpExitReasonInvalidVpRegisterValue:
                return "InvalidVpRegisterValue";
            case WHvRunVpExitReasonUnsupportedFeature:
                return "UnsupportedFeature";
            case WHvRunVpExitReasonX64InterruptWindow:
                return "X64InterruptWindow";
            case WHvRunVpExitReasonX64Halt:
                return "X64Halt";
            case WHvRunVpExitReasonX64ApicEoi:
                return "X64ApicEoi";
            case WHvRunVpExitReasonSynicSintDeliverable:
                return "SynicSintDeliverable";
            case WHvRunVpExitReasonX64MsrAccess:
                return "X64MsrAccess";
            case WHvRunVpExitReasonX64Cpuid:
                return "X64Cpuid";
            case WHvRunVpExitReasonException:
                return "Exception";
            case WHvRunVpExitReasonX64Rdtsc:
                return "X64Rdtsc";
            case WHvRunVpExitReasonHypercall:
                return "Hypercall";
            case WHvRunVpExitReasonX64ApicInitSipiTrap:
                return "X64ApicInitSipiTrap";
            case WHvRunVpExitReasonX64ApicWriteTrap:
                return "X64ApicWriteTrap";
            case WHvRunVpExitReasonCanceled:
                return "Canceled";
            default:
                return "Unknown";
            }
        }

        [[noreturn]] void throw_unhandled_exit(const WHV_RUN_VP_EXIT_CONTEXT& exit_context)
        {
            std::ostringstream stream;
            stream << "Unhandled WHP virtual processor exit: reason=" << exit_reason_name(exit_context.ExitReason) << " (0x" << std::hex
                   << static_cast<uint32_t>(exit_context.ExitReason) << ")"
                   << " rip=0x" << exit_context.VpContext.Rip;

            switch (exit_context.ExitReason)
            {
            case WHvRunVpExitReasonX64IoPortAccess:
                stream << " port=0x" << exit_context.IoPortAccess.PortNumber;
                break;
            case WHvRunVpExitReasonX64MsrAccess:
                stream << " msr=0x" << exit_context.MsrAccess.MsrNumber
                       << " write=" << static_cast<uint32_t>(exit_context.MsrAccess.AccessInfo.IsWrite);
                break;
            case WHvRunVpExitReasonInvalidVpRegisterValue:
                stream << " register=0x" << exit_context.VpContext.ExecutionState.AsUINT16;
                break;
            case WHvRunVpExitReasonUnsupportedFeature:
                stream << " feature_code=0x" << static_cast<uint32_t>(exit_context.UnsupportedFeature.FeatureCode) << " feature_param=0x"
                       << exit_context.UnsupportedFeature.FeatureParameter;
                break;
            case WHvRunVpExitReasonException:
                stream << " exception_type=0x" << static_cast<uint32_t>(exit_context.VpException.ExceptionType);
                break;
            default:
                break;
            }

            throw std::runtime_error(stream.str());
        }

        constexpr std::array<WHV_REGISTER_NAME, 64> snapshot_register_names()
        {
            return std::array<WHV_REGISTER_NAME, 64>{{
                WHvX64RegisterRax,
                WHvX64RegisterRbx,
                WHvX64RegisterRcx,
                WHvX64RegisterRdx,
                WHvX64RegisterRsi,
                WHvX64RegisterRdi,
                WHvX64RegisterRbp,
                WHvX64RegisterRsp,
                WHvX64RegisterR8,
                WHvX64RegisterR9,
                WHvX64RegisterR10,
                WHvX64RegisterR11,
                WHvX64RegisterR12,
                WHvX64RegisterR13,
                WHvX64RegisterR14,
                WHvX64RegisterR15,
                WHvX64RegisterRip,
                WHvX64RegisterRflags,
                WHvX64RegisterCs,
                WHvX64RegisterSs,
                WHvX64RegisterDs,
                WHvX64RegisterEs,
                WHvX64RegisterFs,
                WHvX64RegisterGs,
                WHvX64RegisterCr0,
                WHvX64RegisterCr2,
                WHvX64RegisterCr3,
                WHvX64RegisterCr4,
                WHvX64RegisterDr0,
                WHvX64RegisterDr1,
                WHvX64RegisterDr2,
                WHvX64RegisterDr3,
                WHvX64RegisterDr6,
                WHvX64RegisterDr7,
                WHvX64RegisterGdtr,
                WHvX64RegisterIdtr,
                WHvX64RegisterEfer,
                WHvX64RegisterXCr0,
                WHvX64RegisterFpControlStatus,
                WHvX64RegisterXmmControlStatus,
                WHvX64RegisterFpMmx0,
                WHvX64RegisterFpMmx1,
                WHvX64RegisterFpMmx2,
                WHvX64RegisterFpMmx3,
                WHvX64RegisterFpMmx4,
                WHvX64RegisterFpMmx5,
                WHvX64RegisterFpMmx6,
                WHvX64RegisterFpMmx7,
                WHvX64RegisterXmm0,
                WHvX64RegisterXmm1,
                WHvX64RegisterXmm2,
                WHvX64RegisterXmm3,
                WHvX64RegisterXmm4,
                WHvX64RegisterXmm5,
                WHvX64RegisterXmm6,
                WHvX64RegisterXmm7,
                WHvX64RegisterXmm8,
                WHvX64RegisterXmm9,
                WHvX64RegisterXmm10,
                WHvX64RegisterXmm11,
                WHvX64RegisterXmm12,
                WHvX64RegisterXmm13,
                WHvX64RegisterXmm14,
                WHvX64RegisterXmm15,
            }};
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
                return {WHvX64RegisterRax, register_kind::reg64, sizeof(uint64_t)};
            case x86_register::bl:
            case x86_register::bh:
            case x86_register::bx:
            case x86_register::ebx:
            case x86_register::rbx:
                return {WHvX64RegisterRbx, register_kind::reg64, sizeof(uint64_t)};
            case x86_register::cl:
            case x86_register::ch:
            case x86_register::cx:
            case x86_register::ecx:
            case x86_register::rcx:
                return {WHvX64RegisterRcx, register_kind::reg64, sizeof(uint64_t)};
            case x86_register::dl:
            case x86_register::dh:
            case x86_register::dx:
            case x86_register::edx:
            case x86_register::rdx:
                return {WHvX64RegisterRdx, register_kind::reg64, sizeof(uint64_t)};
            case x86_register::si:
            case x86_register::sil:
            case x86_register::esi:
            case x86_register::rsi:
                return {WHvX64RegisterRsi, register_kind::reg64, sizeof(uint64_t)};
            case x86_register::di:
            case x86_register::dil:
            case x86_register::edi:
            case x86_register::rdi:
                return {WHvX64RegisterRdi, register_kind::reg64, sizeof(uint64_t)};
            case x86_register::bp:
            case x86_register::bpl:
            case x86_register::ebp:
            case x86_register::rbp:
                return {WHvX64RegisterRbp, register_kind::reg64, sizeof(uint64_t)};
            case x86_register::sp:
            case x86_register::spl:
            case x86_register::esp:
            case x86_register::rsp:
                return {WHvX64RegisterRsp, register_kind::reg64, sizeof(uint64_t)};
            case x86_register::ip:
            case x86_register::eip:
            case x86_register::rip:
                return {WHvX64RegisterRip, register_kind::reg64, sizeof(uint64_t)};
            case x86_register::r8:
            case x86_register::r8d:
            case x86_register::r8w:
            case x86_register::r8b:
                return {WHvX64RegisterR8, register_kind::reg64, sizeof(uint64_t)};
            case x86_register::r9:
            case x86_register::r9d:
            case x86_register::r9w:
            case x86_register::r9b:
                return {WHvX64RegisterR9, register_kind::reg64, sizeof(uint64_t)};
            case x86_register::r10:
            case x86_register::r10d:
            case x86_register::r10w:
            case x86_register::r10b:
                return {WHvX64RegisterR10, register_kind::reg64, sizeof(uint64_t)};
            case x86_register::r11:
            case x86_register::r11d:
            case x86_register::r11w:
            case x86_register::r11b:
                return {WHvX64RegisterR11, register_kind::reg64, sizeof(uint64_t)};
            case x86_register::r12:
            case x86_register::r12d:
            case x86_register::r12w:
            case x86_register::r12b:
                return {WHvX64RegisterR12, register_kind::reg64, sizeof(uint64_t)};
            case x86_register::r13:
            case x86_register::r13d:
            case x86_register::r13w:
            case x86_register::r13b:
                return {WHvX64RegisterR13, register_kind::reg64, sizeof(uint64_t)};
            case x86_register::r14:
            case x86_register::r14d:
            case x86_register::r14w:
            case x86_register::r14b:
                return {WHvX64RegisterR14, register_kind::reg64, sizeof(uint64_t)};
            case x86_register::r15:
            case x86_register::r15d:
            case x86_register::r15w:
            case x86_register::r15b:
                return {WHvX64RegisterR15, register_kind::reg64, sizeof(uint64_t)};
            case x86_register::flags:
            case x86_register::eflags:
            case x86_register::rflags:
                return {WHvX64RegisterRflags, register_kind::reg64, sizeof(uint64_t)};
            case x86_register::cs:
                return {WHvX64RegisterCs, register_kind::segment, sizeof(uint16_t)};
            case x86_register::ss:
                return {WHvX64RegisterSs, register_kind::segment, sizeof(uint16_t)};
            case x86_register::ds:
                return {WHvX64RegisterDs, register_kind::segment, sizeof(uint16_t)};
            case x86_register::es:
                return {WHvX64RegisterEs, register_kind::segment, sizeof(uint16_t)};
            case x86_register::fs:
            case x86_register::fs_base:
                return {WHvX64RegisterFs, register_kind::segment, sizeof(uint16_t)};
            case x86_register::gs:
            case x86_register::gs_base:
                return {WHvX64RegisterGs, register_kind::segment, sizeof(uint16_t)};
            case x86_register::gdtr:
                return {WHvX64RegisterGdtr, register_kind::table, sizeof(WHV_X64_TABLE_REGISTER)};
            case x86_register::idtr:
                return {WHvX64RegisterIdtr, register_kind::table, sizeof(WHV_X64_TABLE_REGISTER)};
            case x86_register::cr0:
                return {WHvX64RegisterCr0, register_kind::reg64, sizeof(uint64_t)};
            case x86_register::cr2:
                return {WHvX64RegisterCr2, register_kind::reg64, sizeof(uint64_t)};
            case x86_register::cr3:
                return {WHvX64RegisterCr3, register_kind::reg64, sizeof(uint64_t)};
            case x86_register::cr4:
                return {WHvX64RegisterCr4, register_kind::reg64, sizeof(uint64_t)};
            case x86_register::dr0:
                return {WHvX64RegisterDr0, register_kind::reg64, sizeof(uint64_t)};
            case x86_register::dr1:
                return {WHvX64RegisterDr1, register_kind::reg64, sizeof(uint64_t)};
            case x86_register::dr2:
                return {WHvX64RegisterDr2, register_kind::reg64, sizeof(uint64_t)};
            case x86_register::dr3:
                return {WHvX64RegisterDr3, register_kind::reg64, sizeof(uint64_t)};
            case x86_register::dr6:
                return {WHvX64RegisterDr6, register_kind::reg64, sizeof(uint64_t)};
            case x86_register::dr7:
                return {WHvX64RegisterDr7, register_kind::reg64, sizeof(uint64_t)};
            case x86_register::fpcw:
            case x86_register::fpsw:
            case x86_register::fptag:
                return {WHvX64RegisterFpControlStatus, register_kind::fp_control, sizeof(uint16_t)};
            case x86_register::mxcsr:
                return {WHvX64RegisterXmmControlStatus, register_kind::xmm_control, sizeof(uint32_t)};
            case x86_register::msr:
                return {WHvX64RegisterEfer, register_kind::reg64, sizeof(uint64_t)};
            default:
                break;
            }

            if (reg >= x86_register::xmm0 && reg <= x86_register::xmm15)
            {
                const auto index = static_cast<int>(reg) - static_cast<int>(x86_register::xmm0);
                return {static_cast<WHV_REGISTER_NAME>(WHvX64RegisterXmm0 + index), register_kind::reg128, sizeof(WHV_UINT128)};
            }

            if (reg >= x86_register::st0 && reg <= x86_register::st7)
            {
                const auto index = static_cast<int>(reg) - static_cast<int>(x86_register::st0);
                return {static_cast<WHV_REGISTER_NAME>(WHvX64RegisterFpMmx0 + index), register_kind::fp, sizeof(WHV_X64_FP_REGISTER)};
            }

            throw std::runtime_error("Unsupported WHP register");
        }

        class whp_x86_64_emulator : public x86_64_emulator
        {
          public:
            whp_x86_64_emulator()
            {
                this->ensure_platform_support();
                this->configure_partition();
                this->vp_ = std::make_unique<virtual_processor_handle>(this->partition_);
                this->initialize_long_mode_page_tables();
                this->initialize_virtual_processor_state();
                this->initialize_syscall_intercept_page();
            }

            ~whp_x86_64_emulator() override
            {
                utils::reset_object_with_delayed_destruction(this->memory_write_hooks_);
                utils::reset_object_with_delayed_destruction(this->memory_read_hooks_);
                utils::reset_object_with_delayed_destruction(this->memory_execution_hooks_);
                utils::reset_object_with_delayed_destruction(this->memory_violation_hooks_);
                utils::reset_object_with_delayed_destruction(this->interrupt_hooks_);
                utils::reset_object_with_delayed_destruction(this->basic_block_hooks_);
                utils::reset_object_with_delayed_destruction(this->instruction_hooks_);
            }

            void start(const size_t count) override
            {
                if (count != 0)
                {
                    throw std::runtime_error("WHP backend does not support exact instruction counts yet");
                }

                this->stop_requested_ = false;

                while (!this->stop_requested_)
                {
                    WHV_RUN_VP_EXIT_CONTEXT exit_context{};
                    const auto start_rip = this->read_instruction_pointer();
                    this->run_active_ = true;
                    const auto run_hr = WHvRunVirtualProcessor(this->partition_, vp_index, &exit_context, sizeof(exit_context));
                    this->run_active_ = false;
                    if (FAILED(run_hr))
                    {
                        throw_hr(run_hr, "WHvRunVirtualProcessor");
                    }
                    switch (exit_context.ExitReason)
                    {
                    case WHvRunVpExitReasonCanceled: {
                        // If rip has not changed, we _probably_ executed nothing.
                        // Could be that the execution was cancelled while it was not running.
                        // Just restart it.
                        if (start_rip == exit_context.VpContext.Rip)
                        {
                            this->stop_requested_ = false;
                            continue;
                        }

                        return;
                    }
                    case WHvRunVpExitReasonX64Halt:
                        if (this->syscall_hook_ && exit_context.VpContext.Rip == (this->syscall_hook_page_ + 1))
                        {
                            if (this->handle_syscall_halt())
                            {
                                continue;
                            }
                        }
                        else
                        {
                            return;
                        }

                        if (this->stop_requested_)
                        {
                            return;
                        }

                        if (this->syscall_hook_)
                        {
                            continue;
                        }
                        return;
                    case WHvRunVpExitReasonMemoryAccess:
                        if (this->handle_memory_access(exit_context.MemoryAccess))
                        {
                            continue;
                        }
                        return;
                    case WHvRunVpExitReasonException:
                        if (this->handle_exception(exit_context))
                        {
                            this->clear_pending_exception_state();
                            continue;
                        }
                        return;
                    case WHvRunVpExitReasonX64Cpuid:
                        if (this->handle_instruction_exit(x86_hookable_instructions::cpuid, exit_context, 2))
                        {
                            continue;
                        }
                        throw std::runtime_error("Unhandled CPUID exit");
                    case WHvRunVpExitReasonX64Rdtsc:
                        if (this->handle_instruction_exit(exit_context.ReadTsc.RdtscInfo.IsRdtscp ? x86_hookable_instructions::rdtscp
                                                                                                  : x86_hookable_instructions::rdtsc,
                                                          exit_context, 2))
                        {
                            continue;
                        }
                        throw std::runtime_error("Unhandled RDTSC/RDTSCP exit");
                    case WHvRunVpExitReasonUnrecoverableException:
                        if (this->handle_unrecoverable_exception())
                        {
                            this->clear_pending_exception_state();
                            continue;
                        }
                        throw_unhandled_exit(exit_context);
                    case WHvRunVpExitReasonUnsupportedFeature:
                        throw std::runtime_error("Encountered unsupported processor feature");
                    default:
                        throw_unhandled_exit(exit_context);
                    }
                }
            }

            void stop() override
            {
                this->stop_requested_ = true;
                if (this->run_active_)
                {
                    WHP_CHECK_HR(WHvCancelRunVirtualProcessor(this->partition_, vp_index, 0));
                }
            }

            void load_gdt(const pointer_type address, const uint32_t limit) override
            {
                WHV_REGISTER_VALUE value{};
                value.Table.Base = address;
                value.Table.Limit = static_cast<UINT16>(limit);
                this->set_register(WHvX64RegisterGdtr, value);
            }

            void set_segment_base(const x86_register base, const pointer_type value) override
            {
                const auto mapping = map_register(base);
                auto segment = this->get_register(mapping.name).Segment;
                segment.Base = value;
                this->set_register(mapping.name, WHV_REGISTER_VALUE{.Segment = segment});
            }

            pointer_type get_segment_base(const x86_register base) override
            {
                const auto mapping = map_register(base);
                return this->get_register(mapping.name).Segment.Base;
            }

            void map_mmio(const uint64_t address, const size_t size, mmio_read_callback read_cb, mmio_write_callback write_cb) override
            {
                if (!is_page_aligned(address) || !is_page_aligned(size))
                {
                    throw std::runtime_error("WHP MMIO mappings must be page aligned");
                }

                mmio_region region{
                    .address = address,
                    .size = size,
                    .read_cb = std::move(read_cb),
                    .write_cb = std::move(write_cb),
                };

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
                        page->permissions = memory_permission::none;
                        this->ensure_virtual_mapping(guest_address);
                    }

                    this->remap_pages(address, size);
                    this->mmio_regions_[address] = std::move(region);
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

                    page->permissions = memory_permission::none;
                    this->remap_page(guest_address, *page);
                    this->ensure_virtual_mapping(guest_address);
                }

                this->mmio_regions_[address] = std::move(region);
            }

            void map_memory(const uint64_t address, const size_t size, const memory_permission permissions) override
            {
                if (!is_page_aligned(address) || !is_page_aligned(size))
                {
                    throw std::runtime_error("WHP memory mappings must be page aligned");
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
                        this->ensure_virtual_mapping(guest_address);
                    }

                    this->remap_pages(address, size);
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
                    this->remap_page(guest_address, *page);
                    this->ensure_virtual_mapping(guest_address);
                }
            }

            void unmap_memory(const uint64_t address, const size_t size) override
            {
                if (!is_page_aligned(address) || !is_page_aligned(size))
                {
                    throw std::runtime_error("WHP memory unmappings must be page aligned");
                }

                for (size_t offset = 0; offset < size; offset += page_size)
                {
                    const auto guest_address = address + offset;
                    const auto entry = this->mapped_pages_.find(guest_address);
                    if (entry == this->mapped_pages_.end())
                    {
                        continue;
                    }

                    if (entry->second->map_flags != 0)
                    {
                        WHP_CHECK_HR(WHvUnmapGpaRange(this->partition_, guest_address, page_size));
                    }

                    this->mapped_pages_.erase(entry);
                }

                const auto mmio = this->mmio_regions_.find(address);
                if (mmio != this->mmio_regions_.end() && mmio->second.size == size)
                {
                    this->mmio_regions_.erase(mmio);
                }
            }

            bool try_read_memory(const uint64_t address, void* data, const size_t size) const override
            {
                return this->access_memory(address, data, size, false);
            }

            void read_memory(const uint64_t address, void* data, const size_t size) const override
            {
                if (!this->try_read_memory(address, data, size))
                {
                    throw std::runtime_error("Failed to read WHP guest memory");
                }
            }

            bool try_write_memory(const uint64_t address, const void* data, const size_t size) override
            {
                return this->access_memory(address, const_cast<void*>(data), size, true);
            }

            void write_memory(const uint64_t address, const void* data, const size_t size) override
            {
                if (!this->try_write_memory(address, data, size))
                {
                    throw std::runtime_error("Failed to write WHP guest memory");
                }
            }

            void apply_memory_protection(const uint64_t address, const size_t size, const memory_permission permissions) override
            {
                if (!is_page_aligned(address) || !is_page_aligned(size))
                {
                    throw std::runtime_error("WHP protection changes must be page aligned");
                }

                for (size_t offset = 0; offset < size; offset += page_size)
                {
                    const auto guest_address = address + offset;
                    const auto entry = this->mapped_pages_.find(guest_address);
                    if (entry == this->mapped_pages_.end())
                    {
                        continue;
                    }

                    entry->second->permissions = permissions;
                }

                this->remap_pages(address, size);
            }

            size_t write_raw_register(const int reg, const void* value, const size_t size) override
            {
                const auto mapping = map_register(static_cast<x86_register>(reg));
                auto current = this->get_register(mapping.name);

                switch (mapping.kind)
                {
                case register_kind::reg64:
                    if (const auto access = classify_gp_register_access(static_cast<x86_register>(reg)))
                    {
                        uint64_t incoming = 0;
                        const auto copy_size = (std::min)(size, access->width);
                        std::memcpy(&incoming, value, copy_size);

                        if (access->zero_extend_32)
                        {
                            current.Reg64 = static_cast<uint32_t>(incoming);
                        }
                        else if (access->width >= sizeof(current.Reg64))
                        {
                            current.Reg64 = incoming;
                        }
                        else
                        {
                            const auto mask = ((uint64_t{1} << (access->width * 8)) - 1) << (access->offset * 8);
                            current.Reg64 = (current.Reg64 & ~mask) | ((incoming << (access->offset * 8)) & mask);
                        }
                    }
                    else
                    {
                        std::memcpy(&current.Reg64, value, (std::min)(size, sizeof(current.Reg64)));
                    }
                    break;
                case register_kind::segment:
                    if (static_cast<x86_register>(reg) == x86_register::fs_base || static_cast<x86_register>(reg) == x86_register::gs_base)
                    {
                        std::memcpy(&current.Segment.Base, value, (std::min)(size, sizeof(current.Segment.Base)));
                    }
                    else
                    {
                        std::memcpy(&current.Segment.Selector, value, (std::min)(size, sizeof(current.Segment.Selector)));
                    }
                    break;
                case register_kind::table:
                    std::memcpy(&current.Table, value, (std::min)(size, sizeof(current.Table)));
                    break;
                case register_kind::fp:
                    std::memcpy(&current.Fp.AsUINT128, value, (std::min)(size, sizeof(current.Fp.AsUINT128)));
                    break;
                case register_kind::fp_control:
                    if (static_cast<x86_register>(reg) == x86_register::fpcw)
                    {
                        std::memcpy(&current.FpControlStatus.FpControl, value, (std::min)(size, sizeof(current.FpControlStatus.FpControl)));
                    }
                    else if (static_cast<x86_register>(reg) == x86_register::fpsw)
                    {
                        std::memcpy(&current.FpControlStatus.FpStatus, value, (std::min)(size, sizeof(current.FpControlStatus.FpStatus)));
                    }
                    else
                    {
                        uint16_t fp_tag{};
                        std::memcpy(&fp_tag, value, (std::min)(size, sizeof(fp_tag)));
                        current.FpControlStatus.FpTag = static_cast<UINT8>(fp_tag);
                    }
                    break;
                case register_kind::xmm_control:
                    std::memcpy(&current.XmmControlStatus.XmmStatusControl, value,
                                (std::min)(size, sizeof(current.XmmControlStatus.XmmStatusControl)));
                    current.XmmControlStatus.XmmStatusControlMask = 0xFFFFFFFF;
                    break;
                case register_kind::reg128:
                    std::memcpy(&current.Reg128, value, (std::min)(size, sizeof(current.Reg128)));
                    break;
                }

                this->set_register(mapping.name, current);
                return size;
            }

            size_t read_raw_register(const int reg, void* value, const size_t size) override
            {
                const auto mapping = map_register(static_cast<x86_register>(reg));
                const auto current = this->get_register(mapping.name);

                switch (mapping.kind)
                {
                case register_kind::reg64:
                    if (const auto access = classify_gp_register_access(static_cast<x86_register>(reg)))
                    {
                        const auto narrowed = current.Reg64 >> (access->offset * 8);
                        std::memcpy(value, &narrowed, (std::min)(size, access->width));
                    }
                    else
                    {
                        std::memcpy(value, &current.Reg64, (std::min)(size, sizeof(current.Reg64)));
                    }
                    break;
                case register_kind::segment:
                    if (static_cast<x86_register>(reg) == x86_register::fs_base || static_cast<x86_register>(reg) == x86_register::gs_base)
                    {
                        std::memcpy(value, &current.Segment.Base, (std::min)(size, sizeof(current.Segment.Base)));
                    }
                    else
                    {
                        std::memcpy(value, &current.Segment.Selector, (std::min)(size, sizeof(current.Segment.Selector)));
                    }
                    break;
                case register_kind::table:
                    std::memcpy(value, &current.Table, (std::min)(size, sizeof(current.Table)));
                    break;
                case register_kind::fp:
                    std::memcpy(value, &current.Fp.AsUINT128, (std::min)(size, sizeof(current.Fp.AsUINT128)));
                    break;
                case register_kind::fp_control:
                    if (static_cast<x86_register>(reg) == x86_register::fpcw)
                    {
                        std::memcpy(value, &current.FpControlStatus.FpControl, (std::min)(size, sizeof(current.FpControlStatus.FpControl)));
                    }
                    else if (static_cast<x86_register>(reg) == x86_register::fpsw)
                    {
                        std::memcpy(value, &current.FpControlStatus.FpStatus, (std::min)(size, sizeof(current.FpControlStatus.FpStatus)));
                    }
                    else
                    {
                        uint16_t fp_tag = current.FpControlStatus.FpTag;
                        std::memcpy(value, &fp_tag, (std::min)(size, sizeof(fp_tag)));
                    }
                    break;
                case register_kind::xmm_control:
                    std::memcpy(value, &current.XmmControlStatus.XmmStatusControl,
                                (std::min)(size, sizeof(current.XmmControlStatus.XmmStatusControl)));
                    break;
                case register_kind::reg128:
                    std::memcpy(value, &current.Reg128, (std::min)(size, sizeof(current.Reg128)));
                    break;
                }

                return size;
            }

            bool read_descriptor_table(const int reg, descriptor_table_register& table) override
            {
                if (reg != static_cast<int>(x86_register::gdtr) && reg != static_cast<int>(x86_register::idtr))
                {
                    return false;
                }

                const auto mapping = map_register(static_cast<x86_register>(reg));
                const auto value = this->get_register(mapping.name).Table;
                table.base = value.Base;
                table.limit = value.Limit;
                return true;
            }

            emulator_hook* hook_instruction(const int instruction_type, instruction_hook_callback callback) override
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

            emulator_hook* hook_basic_block(basic_block_hook_callback callback) override
            {
                auto* hook = this->make_hook();
                this->basic_block_hooks_[hook] = std::move(callback);
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

            emulator_hook* hook_memory_execution(memory_execution_hook_callback callback) override
            {
                auto* hook = this->make_hook();
                this->memory_execution_hooks_[hook] = execution_hook_entry{.address = std::nullopt, .callback = std::move(callback)};
                return hook;
            }

            emulator_hook* hook_memory_execution(const uint64_t address, memory_execution_hook_callback callback) override
            {
                auto* hook = this->make_hook();
                this->memory_execution_hooks_[hook] = execution_hook_entry{.address = address, .callback = std::move(callback)};
                return hook;
            }

            emulator_hook* hook_memory_read(const uint64_t address, const uint64_t size, memory_access_hook_callback callback) override
            {
                auto* hook = this->make_hook();
                this->memory_read_hooks_[hook] =
                    memory_access_hook_entry{.address = address, .size = size, .callback = std::move(callback)};
                return hook;
            }

            emulator_hook* hook_memory_write(const uint64_t address, const uint64_t size, memory_access_hook_callback callback) override
            {
                auto* hook = this->make_hook();
                this->memory_write_hooks_[hook] =
                    memory_access_hook_entry{.address = address, .size = size, .callback = std::move(callback)};
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

            void serialize_state(utils::buffer_serializer& buffer, const bool) const override
            {
                buffer.write_vector(this->save_registers());
            }

            void deserialize_state(utils::buffer_deserializer& buffer, const bool) override
            {
                this->restore_registers(buffer.read_vector<std::byte>());
            }

            std::vector<std::byte> save_registers() const override
            {
                auto names = snapshot_register_names();
                std::vector<WHV_REGISTER_VALUE> values(names.size());
                WHP_CHECK_HR(WHvGetVirtualProcessorRegisters(this->partition_, vp_index, names.data(), static_cast<UINT32>(names.size()),
                                                             values.data()));

                std::vector<std::byte> bytes(sizeof(WHV_REGISTER_VALUE) * values.size());
                std::memcpy(bytes.data(), values.data(), bytes.size());
                return bytes;
            }

            void restore_registers(const std::vector<std::byte>& register_data) override
            {
                auto names = snapshot_register_names();
                const auto expected_size = sizeof(WHV_REGISTER_VALUE) * names.size();
                if (register_data.size() != expected_size)
                {
                    throw std::runtime_error("Unexpected WHP register snapshot size");
                }

                std::vector<WHV_REGISTER_VALUE> values(names.size());
                std::memcpy(values.data(), register_data.data(), register_data.size());

                WHP_CHECK_HR(WHvSetVirtualProcessorRegisters(this->partition_, vp_index, names.data(), static_cast<UINT32>(names.size()),
                                                             values.data()));
            }

            bool has_violation() const override
            {
                return false;
            }

            std::string get_name() const override
            {
                return "Windows Hypervisor Platform";
            }

            bool supports_instruction_counting() const override
            {
                return false;
            }

          private:
            struct instruction_hook_entry
            {
                x86_hookable_instructions type = x86_hookable_instructions::invalid;
                instruction_hook_callback callback{};
            };

            struct execution_hook_entry
            {
                std::optional<uint64_t> address{};
                memory_execution_hook_callback callback{};
            };

            struct memory_access_hook_entry
            {
                uint64_t address{};
                uint64_t size{};
                memory_access_hook_callback callback{};
            };

            struct mmio_region
            {
                uint64_t address{};
                size_t size{};
                mmio_read_callback read_cb{};
                mmio_write_callback write_cb{};
            };

            struct pending_mmio_step
            {
                uint64_t page_base{};
                uint64_t original_rflags{};
                bool had_trap_flag{};
                bool allow_debug_delivery{};
                bool is_write{};
                std::vector<std::byte> old_page{};
            };

            partition_handle partition_{};
            std::unique_ptr<virtual_processor_handle> vp_{};
            WHV_EXTENDED_VM_EXITS supported_exits_{};
            WHV_PROCESSOR_XSAVE_FEATURES supported_xsave_features_{};
            bool has_supported_xsave_features_ = false;
            std::unordered_map<uint64_t, std::unique_ptr<mapped_page>> mapped_pages_{};
            std::unordered_map<uint64_t, uint64_t*> page_table_views_{};
            uint64_t pml4_gpa_ = 0;
            uint64_t next_internal_gpa_ = internal_page_table_base;
            std::atomic_bool stop_requested_ = false;
            std::atomic_bool run_active_ = false;
            uint64_t syscall_hook_page_ = 0;
            size_t next_hook_id_ = 1;

            std::unordered_map<emulator_hook*, instruction_hook_entry> instruction_hooks_{};
            std::unordered_map<emulator_hook*, basic_block_hook_callback> basic_block_hooks_{};
            std::unordered_map<emulator_hook*, interrupt_hook_callback> interrupt_hooks_{};
            std::unordered_map<emulator_hook*, memory_violation_hook_callback> memory_violation_hooks_{};
            std::unordered_map<emulator_hook*, execution_hook_entry> memory_execution_hooks_{};
            std::unordered_map<emulator_hook*, memory_access_hook_entry> memory_read_hooks_{};
            std::unordered_map<emulator_hook*, memory_access_hook_entry> memory_write_hooks_{};
            std::unordered_map<uint64_t, mmio_region> mmio_regions_{};
            std::optional<pending_mmio_step> pending_mmio_step_{};
            instruction_hook_entry* syscall_hook_ = nullptr;
            void ensure_platform_support()
            {
                BOOL hypervisor_present = FALSE;
                UINT32 bytes_written = 0;

                WHP_CHECK_HR(
                    WHvGetCapability(WHvCapabilityCodeHypervisorPresent, &hypervisor_present, sizeof(hypervisor_present), &bytes_written));

                if (!hypervisor_present)
                {
                    throw std::runtime_error("Hypervisor is not present. Enable Hyper-V and Windows Hypervisor Platform.");
                }

                WHP_CHECK_HR(WHvGetCapability(WHvCapabilityCodeExtendedVmExits, &this->supported_exits_, sizeof(this->supported_exits_),
                                              &bytes_written));

                bytes_written = 0;
                const auto xsave_hr = WHvGetCapability(WHvCapabilityCodeProcessorXsaveFeatures, &this->supported_xsave_features_,
                                                       sizeof(this->supported_xsave_features_), &bytes_written);
                this->has_supported_xsave_features_ = SUCCEEDED(xsave_hr) && bytes_written == sizeof(this->supported_xsave_features_) &&
                                                      this->supported_xsave_features_.XsaveSupport;
            }

            void configure_partition()
            {
                UINT32 processor_count = 1;
                WHP_CHECK_HR(WHvSetPartitionProperty(this->partition_, WHvPartitionPropertyCodeProcessorCount, &processor_count,
                                                     sizeof(processor_count)));

                WHV_EXTENDED_VM_EXITS enabled_exits{};
                enabled_exits.ExceptionExit = this->supported_exits_.ExceptionExit ? 1 : 0;
                enabled_exits.X64CpuidExit = this->supported_exits_.X64CpuidExit ? 1 : 0;
                enabled_exits.X64RdtscExit = this->supported_exits_.X64RdtscExit ? 1 : 0;

                WHP_CHECK_HR(WHvSetPartitionProperty(this->partition_, WHvPartitionPropertyCodeExtendedVmExits, &enabled_exits,
                                                     sizeof(enabled_exits)));

                WHP_CHECK_HR(WHvSetupPartition(this->partition_));
            }

            void initialize_virtual_processor_state()
            {
                std::vector<WHV_REGISTER_NAME> names = {
                    WHvX64RegisterCs,
                    WHvX64RegisterSs,
                    WHvX64RegisterDs,
                    WHvX64RegisterEs,
                    WHvX64RegisterFs,
                    WHvX64RegisterGs,
                    WHvX64RegisterCr0,
                    WHvX64RegisterCr4,
                    WHvX64RegisterCr3,
                    WHvX64RegisterEfer,
                    WHvX64RegisterRflags,
                    WHvX64RegisterStar,
                    WHvX64RegisterSfmask,
                    WHvX64RegisterFpControlStatus,
                    WHvX64RegisterXmmControlStatus,
                };
                std::vector<WHV_REGISTER_VALUE> values(names.size());
                values[0].Segment = make_segment(0x33, true);
                values[1].Segment = make_segment(0x2B, false);
                values[2].Segment = make_segment(0x2B, false);
                values[3].Segment = make_segment(0x2B, false);
                values[4].Segment = make_segment(0x53, false);
                values[5].Segment = make_segment(0x2B, false);
                values[6].Reg64 = 0x80000033ull;
                values[7].Reg64 = this->has_supported_xsave_features_ ? 0x40620ull : 0x620ull;
                values[8].Reg64 = this->pml4_gpa_;
                values[9].Reg64 = (1ull << 0) | (1ull << 8) | (1ull << 10);
                values[10].Reg64 = 0x2ull;
                values[11].Reg64 = (0x23ull << 48) | (0x08ull << 32);
                values[12].Reg64 = 0;
                values[13].FpControlStatus.FpControl = 0x037Full;
                values[13].FpControlStatus.FpStatus = 0;
                values[13].FpControlStatus.FpTag = 0xFF;
                values[14].XmmControlStatus.XmmStatusControl = 0x1F80u;
                values[14].XmmControlStatus.XmmStatusControlMask = 0xFFFFFFFFu;

                if (this->has_supported_xsave_features_)
                {
                    names.push_back(WHvX64RegisterXCr0);
                    WHV_REGISTER_VALUE xcr0{};
                    xcr0.Reg64 = 0x3ull;
                    values.push_back(xcr0);
                }

                WHP_CHECK_HR(WHvSetVirtualProcessorRegisters(this->partition_, vp_index, names.data(), static_cast<UINT32>(names.size()),
                                                             values.data()));
            }

            void initialize_syscall_intercept_page()
            {
                this->syscall_hook_page_ = this->allocate_internal_page(true);
                auto* code = static_cast<uint8_t*>(this->mapped_pages_.at(this->syscall_hook_page_)->host_page);
                code[0] = 0xF4;

                WHV_REGISTER_VALUE lstar{};
                lstar.Reg64 = this->syscall_hook_page_;
                this->set_register(WHvX64RegisterLstar, lstar);
            }

            bool handle_syscall_halt()
            {
                if (!this->syscall_hook_)
                {
                    return false;
                }

                constexpr std::array<WHV_REGISTER_NAME, 7> entry_names = {
                    WHvX64RegisterRip,    WHvX64RegisterRcx, WHvX64RegisterR10, WHvX64RegisterR11,
                    WHvX64RegisterRflags, WHvX64RegisterCs,  WHvX64RegisterSs,
                };
                auto entry_values = this->get_registers(entry_names);

                const auto post_syscall_rcx = entry_values[1].Reg64;
                const auto post_syscall_r10 = entry_values[2].Reg64;
                const auto saved_rflags = entry_values[3].Reg64;

                const auto pre_syscall_rip = post_syscall_rcx - 2;

                entry_values[0].Reg64 = pre_syscall_rip;
                entry_values[1].Reg64 = post_syscall_r10;
                entry_values[4].Reg64 = saved_rflags;
                entry_values[5].Segment = make_segment(0x33, true);
                entry_values[6].Segment = make_segment(0x2B, false);

                constexpr std::array<WHV_REGISTER_NAME, 5> pre_hook_names = {
                    WHvX64RegisterRip, WHvX64RegisterRcx, WHvX64RegisterRflags, WHvX64RegisterCs, WHvX64RegisterSs,
                };
                const std::array<WHV_REGISTER_VALUE, 5> pre_hook_values = {
                    entry_values[0], entry_values[1], entry_values[4], entry_values[5], entry_values[6],
                };
                this->set_registers(pre_hook_names, pre_hook_values);

                const auto continuation = this->syscall_hook_->callback(0);

                constexpr std::array<WHV_REGISTER_NAME, 1> post_hook_rip_name = {WHvX64RegisterRip};
                auto post_hook_rip_value = this->get_registers(post_hook_rip_name);
                if (continuation == instruction_hook_continuation::skip_instruction && post_hook_rip_value[0].Reg64 == pre_syscall_rip)
                {
                    post_hook_rip_value[0].Reg64 = post_syscall_rcx;
                }
                else
                {
                    post_hook_rip_value[0].Reg64 += 2;
                }

                constexpr std::array<WHV_REGISTER_NAME, 3> post_hook_names = {
                    WHvX64RegisterRip,
                    WHvX64RegisterCs,
                    WHvX64RegisterSs,
                };
                std::array<WHV_REGISTER_VALUE, 3> post_hook_values = {
                    post_hook_rip_value[0],
                    {},
                    {},
                };
                post_hook_values[1].Segment = make_segment(0x33, true);
                post_hook_values[2].Segment = make_segment(0x2B, false);
                this->set_registers(post_hook_names, post_hook_values);

                return true;
            }

            void initialize_long_mode_page_tables()
            {
                this->pml4_gpa_ = this->allocate_internal_page(false, false);
            }

            uint64_t allocate_internal_page(const bool executable = false, const bool map_into_guest = true)
            {
                auto backing = allocate_backing_memory(page_size);
                auto* raw_page = backing.get();

                const auto page_gpa = this->next_internal_gpa_;
                this->next_internal_gpa_ += page_size;

                auto page = std::make_unique<mapped_page>();
                page->owned_page = std::move(backing);
                page->host_page = raw_page;
                page->permissions = executable ? memory_permission::all : memory_permission::read_write;

                this->mapped_pages_[page_gpa] = std::move(page);
                this->remap_page(page_gpa, *this->mapped_pages_[page_gpa]);
                this->page_table_views_[page_gpa] = reinterpret_cast<uint64_t*>(raw_page);

                if (map_into_guest)
                {
                    this->ensure_virtual_mapping(page_gpa);
                }

                return page_gpa;
            }

            void remap_page(const uint64_t guest_address, mapped_page& page)
            {
                if (page.map_flags != 0)
                {
                    WHP_CHECK_HR(WHvUnmapGpaRange(this->partition_, guest_address, page_size));
                }

                page.map_flags = to_whp_map_flags(page.permissions);
                if (page.map_flags == 0)
                {
                    return;
                }

                WHP_CHECK_HR(WHvMapGpaRange(this->partition_, page.host_page, guest_address, page_size,
                                            static_cast<WHV_MAP_GPA_RANGE_FLAGS>(page.map_flags)));
            }

            void remap_pages(const uint64_t address, const size_t size)
            {
                if (size == 0)
                {
                    return;
                }

                size_t offset = 0;
                while (offset < size)
                {
                    const auto run_address = address + offset;
                    auto entry = this->mapped_pages_.find(run_address);
                    if (entry == this->mapped_pages_.end() || !entry->second || entry->second->host_page == nullptr)
                    {
                        offset += page_size;
                        continue;
                    }

                    auto* const run_host_base = static_cast<uint8_t*>(entry->second->host_page);
                    const auto run_flags = to_whp_map_flags(entry->second->permissions);
                    const auto run_had_mapping = entry->second->map_flags != 0;

                    size_t run_size = page_size;
                    while (offset + run_size < size)
                    {
                        const auto next_address = address + offset + run_size;
                        const auto next_entry = this->mapped_pages_.find(next_address);
                        if (next_entry == this->mapped_pages_.end() || !next_entry->second || next_entry->second->host_page == nullptr)
                        {
                            break;
                        }

                        const auto next_flags = to_whp_map_flags(next_entry->second->permissions);
                        const auto next_had_mapping = next_entry->second->map_flags != 0;
                        auto* const expected_host = run_host_base + run_size;
                        if (next_entry->second->host_page != expected_host || next_flags != run_flags ||
                            next_had_mapping != run_had_mapping)
                        {
                            break;
                        }

                        run_size += page_size;
                    }

                    this->remap_page_range(run_address, run_size, run_host_base, run_flags, run_had_mapping);
                    for (size_t run_offset = 0; run_offset < run_size; run_offset += page_size)
                    {
                        this->mapped_pages_.at(run_address + run_offset)->map_flags = run_flags;
                    }

                    offset += run_size;
                }
            }

            void remap_page_range(const uint64_t guest_address, const size_t size, void* host_base, const uint32_t map_flags,
                                  const bool had_mapping)
            {
                if (had_mapping)
                {
                    WHP_CHECK_HR(WHvUnmapGpaRange(this->partition_, guest_address, size));
                }

                if (map_flags != 0)
                {
                    WHP_CHECK_HR(
                        WHvMapGpaRange(this->partition_, host_base, guest_address, size, static_cast<WHV_MAP_GPA_RANGE_FLAGS>(map_flags)));
                }
            }

            mmio_region* find_mmio_region(const uint64_t address)
            {
                for (auto& [base, region] : this->mmio_regions_)
                {
                    if (address >= base && address < base + region.size)
                    {
                        return &region;
                    }
                }

                return nullptr;
            }

            const mmio_region* find_mmio_region(const uint64_t address) const
            {
                for (const auto& [base, region] : this->mmio_regions_)
                {
                    if (address >= base && address < base + region.size)
                    {
                        return &region;
                    }
                }

                return nullptr;
            }

            void refresh_mmio_page(const mmio_region& region, const uint64_t page_base)
            {
                auto entry = this->mapped_pages_.find(page_base);
                if (entry == this->mapped_pages_.end() || !entry->second || entry->second->host_page == nullptr)
                {
                    throw std::runtime_error("MMIO page backing is missing");
                }

                auto* page = static_cast<std::byte*>(entry->second->host_page);
                std::memset(page, 0, page_size);

                const auto region_offset = static_cast<size_t>(page_base - region.address);
                const auto bytes_to_read = (std::min)(static_cast<size_t>(page_size), region.size - region_offset);
                region.read_cb(region_offset, page, bytes_to_read);
            }

            void flush_mmio_write(const mmio_region& region, const uint64_t page_base, const std::vector<std::byte>& old_page)
            {
                auto entry = this->mapped_pages_.find(page_base);
                if (entry == this->mapped_pages_.end() || !entry->second || entry->second->host_page == nullptr)
                {
                    throw std::runtime_error("MMIO page backing is missing");
                }

                const auto* current_page = static_cast<const std::byte*>(entry->second->host_page);
                const auto region_offset = static_cast<size_t>(page_base - region.address);
                const auto bytes_in_region = (std::min)(static_cast<size_t>(page_size), region.size - region_offset);

                size_t index = 0;
                while (index < bytes_in_region)
                {
                    if (old_page[index] == current_page[index])
                    {
                        ++index;
                        continue;
                    }

                    const auto start = index;
                    while (index < bytes_in_region && old_page[index] != current_page[index])
                    {
                        ++index;
                    }

                    region.write_cb(region_offset + start, current_page + start, index - start);
                }
            }

            bool arm_mmio_single_step(const uint64_t page_base, const bool is_write)
            {
                if (this->pending_mmio_step_.has_value())
                {
                    return false;
                }

                auto rflags = this->get_register(WHvX64RegisterRflags);
                const auto original_rflags = rflags.Reg64;

                pending_mmio_step state{};
                state.page_base = page_base;
                state.original_rflags = original_rflags;
                state.had_trap_flag = (original_rflags & 0x100) != 0;
                state.allow_debug_delivery = state.had_trap_flag;
                state.is_write = is_write;

                if (is_write)
                {
                    auto entry = this->mapped_pages_.find(page_base);
                    if (entry == this->mapped_pages_.end() || !entry->second || entry->second->host_page == nullptr)
                    {
                        throw std::runtime_error("MMIO page backing is missing");
                    }

                    state.old_page.resize(page_size);
                    std::memcpy(state.old_page.data(), entry->second->host_page, page_size);
                }

                rflags.Reg64 = original_rflags | 0x100;
                this->set_register(WHvX64RegisterRflags, rflags);
                this->pending_mmio_step_ = std::move(state);
                return true;
            }

            bool complete_pending_mmio_step()
            {
                if (!this->pending_mmio_step_.has_value())
                {
                    return false;
                }

                const auto state = std::move(*this->pending_mmio_step_);
                this->pending_mmio_step_.reset();

                const auto entry = this->mapped_pages_.find(state.page_base);
                if (entry == this->mapped_pages_.end() || !entry->second)
                {
                    throw std::runtime_error("MMIO page backing is missing");
                }

                if (state.is_write)
                {
                    if (const auto* region = this->find_mmio_region(state.page_base))
                    {
                        this->flush_mmio_write(*region, state.page_base, state.old_page);
                    }
                }

                entry->second->permissions = memory_permission::none;
                this->remap_page(state.page_base, *entry->second);

                WHV_REGISTER_VALUE rflags{};
                rflags.Reg64 = state.original_rflags;
                this->set_register(WHvX64RegisterRflags, rflags);

                return !state.allow_debug_delivery;
            }

            uint64_t* get_page_table_entries(const uint64_t page_gpa)
            {
                return this->page_table_views_.at(page_gpa);
            }

            uint64_t ensure_child_table(const uint64_t table_gpa, const size_t index)
            {
                auto* const table_entries = this->get_page_table_entries(table_gpa);
                auto& entry = table_entries[index];

                if ((entry & page_table_entry_present) == 0)
                {
                    const auto child_gpa = this->allocate_internal_page(false, false);
                    entry = child_gpa | page_table_entry_present | page_table_entry_writable | page_table_entry_user;
                    return child_gpa;
                }

                return entry & page_table_entry_address_mask;
            }

            void ensure_virtual_mapping(const uint64_t guest_address)
            {
                const auto page_base = align_down_to_page(guest_address);
                const auto pml4_index = static_cast<size_t>((page_base >> 39) & 0x1FF);
                const auto pdpt_index = static_cast<size_t>((page_base >> 30) & 0x1FF);
                const auto pd_index = static_cast<size_t>((page_base >> 21) & 0x1FF);
                const auto pt_index = static_cast<size_t>((page_base >> 12) & 0x1FF);

                const auto pdpt_gpa = this->ensure_child_table(this->pml4_gpa_, pml4_index);
                const auto pd_gpa = this->ensure_child_table(pdpt_gpa, pdpt_index);
                const auto pt_gpa = this->ensure_child_table(pd_gpa, pd_index);

                auto* const pt_entries = this->get_page_table_entries(pt_gpa);
                pt_entries[pt_index] = page_base | page_table_entry_present | page_table_entry_writable | page_table_entry_user;
            }

            bool access_memory(const uint64_t address, void* data, size_t size, const bool is_write) const
            {
                auto current_address = address;
                auto* cursor = static_cast<std::byte*>(data);

                while (size > 0)
                {
                    const auto page_base = align_down_to_page(current_address);
                    const auto entry = this->mapped_pages_.find(page_base);
                    if (entry == this->mapped_pages_.end() || !entry->second || entry->second->host_page == nullptr)
                    {
                        return false;
                    }

                    const auto offset = static_cast<size_t>(current_address - page_base);
                    const auto chunk = (std::min)(size, static_cast<size_t>(page_size - offset));
                    auto* page_ptr = static_cast<std::byte*>(entry->second->host_page) + offset;

                    if (is_write)
                    {
                        std::memcpy(page_ptr, cursor, chunk);
                    }
                    else
                    {
                        std::memcpy(cursor, page_ptr, chunk);
                    }

                    current_address += chunk;
                    cursor += chunk;
                    size -= chunk;
                }

                return true;
            }

            WHV_REGISTER_VALUE get_register(const WHV_REGISTER_NAME name) const
            {
                WHV_REGISTER_VALUE value{};
                WHP_CHECK_HR(WHvGetVirtualProcessorRegisters(this->partition_, vp_index, &name, 1, &value));
                return value;
            }

            template <size_t N>
            std::array<WHV_REGISTER_VALUE, N> get_registers(const std::array<WHV_REGISTER_NAME, N>& names) const
            {
                std::array<WHV_REGISTER_VALUE, N> values{};
                WHP_CHECK_HR(WHvGetVirtualProcessorRegisters(this->partition_, vp_index, names.data(), static_cast<UINT32>(names.size()),
                                                             values.data()));
                return values;
            }

            void set_register(const WHV_REGISTER_NAME name, const WHV_REGISTER_VALUE& value)
            {
                WHP_CHECK_HR(WHvSetVirtualProcessorRegisters(this->partition_, vp_index, &name, 1, &value));
            }

            template <size_t N>
            void set_registers(const std::array<WHV_REGISTER_NAME, N>& names, const std::array<WHV_REGISTER_VALUE, N>& values)
            {
                WHP_CHECK_HR(WHvSetVirtualProcessorRegisters(this->partition_, vp_index, names.data(), static_cast<UINT32>(names.size()),
                                                             values.data()));
            }

            emulator_hook* make_hook()
            {
                return reinterpret_cast<emulator_hook*>(this->next_hook_id_++);
            }

            void apply_default_instruction_exit(const x86_hookable_instructions type, const WHV_RUN_VP_EXIT_CONTEXT& exit_context)
            {
                switch (type)
                {
                case x86_hookable_instructions::cpuid: {
                    const auto& cpuid = exit_context.CpuidAccess;
                    this->reg(x86_register::rax, static_cast<uint64_t>(cpuid.DefaultResultRax));
                    this->reg(x86_register::rbx, static_cast<uint64_t>(cpuid.DefaultResultRbx));
                    this->reg(x86_register::rcx, static_cast<uint64_t>(cpuid.DefaultResultRcx));
                    this->reg(x86_register::rdx, static_cast<uint64_t>(cpuid.DefaultResultRdx));
                    break;
                }
                case x86_hookable_instructions::rdtsc:
                case x86_hookable_instructions::rdtscp: {
                    const auto tsc = exit_context.ReadTsc.Tsc;
                    this->reg(x86_register::rax, static_cast<uint32_t>(tsc & 0xFFFFFFFFull));
                    this->reg(x86_register::rdx, static_cast<uint32_t>(tsc >> 32));

                    if (type == x86_hookable_instructions::rdtscp)
                    {
                        this->reg(x86_register::rcx, static_cast<uint32_t>(exit_context.ReadTsc.TscAux & 0xFFFFFFFFull));
                    }

                    break;
                }
                default:
                    break;
                }
            }

            bool handle_instruction_exit(const x86_hookable_instructions type, const WHV_RUN_VP_EXIT_CONTEXT& exit_context,
                                         const uint64_t instruction_size)
            {
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

                if (!handled || !skip)
                {
                    this->apply_default_instruction_exit(type, exit_context);
                }

                this->advance_rip(instruction_size);

                return handled || type == x86_hookable_instructions::cpuid || type == x86_hookable_instructions::rdtsc ||
                       type == x86_hookable_instructions::rdtscp;
            }

            bool handle_unrecoverable_exception()
            {
                const auto rip = this->read_instruction_pointer();
                std::array<std::byte, 2> opcode{};
                if (this->access_memory(rip, opcode.data(), opcode.size(), false) && opcode[0] == std::byte{0x0F} &&
                    opcode[1] == std::byte{0x0B})
                {
                    bool skip = false;
                    bool consumed = false;

                    for (auto& [_, hook] : this->instruction_hooks_)
                    {
                        if (hook.type != x86_hookable_instructions::invalid)
                        {
                            continue;
                        }

                        if (hook.callback(0) == instruction_hook_continuation::skip_instruction)
                        {
                            skip = true;
                            consumed = true;
                        }
                    }

                    if (skip && this->read_instruction_pointer() == rip)
                    {
                        this->advance_rip(2);
                    }

                    if (consumed)
                    {
                        return true;
                    }

                    for (auto& [_, hook] : this->interrupt_hooks_)
                    {
                        hook(6);
                        return true;
                    }

                    return false;
                }

                if (this->pending_mmio_step_.has_value())
                {
                    const auto swallow = this->complete_pending_mmio_step();
                    if (swallow)
                    {
                        return true;
                    }
                }

                const auto rflags = this->reg<uint64_t>(x86_register::rflags);
                if ((rflags & 0x100ull) != 0)
                {
                    for (auto& [_, hook] : this->interrupt_hooks_)
                    {
                        hook(1);
                        return true;
                    }
                }

                const auto fault_address = this->reg<uint64_t>(x86_register::cr2);

                if (fault_address != 0 && !this->memory_violation_hooks_.empty())
                {
                    for (auto& [_, hook] : this->memory_violation_hooks_)
                    {
                        const auto result = hook(fault_address, 1, memory_operation::read, memory_violation_type::unmapped);
                        if (result == memory_violation_continuation::resume || result == memory_violation_continuation::restart)
                        {
                            return true;
                        }
                    }
                }

                for (auto& [_, hook] : this->interrupt_hooks_)
                {
                    hook(14);
                    return true;
                }

                return false;
            }

            bool handle_memory_access(const WHV_MEMORY_ACCESS_CONTEXT& memory_access)
            {
                const auto mmio_address = memory_access.AccessInfo.GvaValid ? memory_access.Gva : memory_access.Gpa;
                if (auto* region = this->find_mmio_region(mmio_address))
                {
                    const auto page_base = align_down_to_page(mmio_address);
                    const auto operation = map_memory_operation(static_cast<WHV_MEMORY_ACCESS_TYPE>(memory_access.AccessInfo.AccessType));
                    const auto is_write = operation == memory_operation::write;

                    this->refresh_mmio_page(*region, page_base);

                    auto page_it = this->mapped_pages_.find(page_base);
                    if (page_it == this->mapped_pages_.end())
                    {
                        throw std::runtime_error("MMIO page backing is missing");
                    }

                    page_it->second->permissions = is_write ? memory_permission::read_write : memory_permission::read;
                    this->remap_page(page_base, *page_it->second);

                    if (is_write && !this->arm_mmio_single_step(page_base, true))
                    {
                        throw std::runtime_error("Nested WHP MMIO single-step state is not supported");
                    }

                    return true;
                }

                if (this->memory_violation_hooks_.empty())
                {
                    throw std::runtime_error("Unhandled WHP memory access violation");
                }

                const auto operation = map_memory_operation(static_cast<WHV_MEMORY_ACCESS_TYPE>(memory_access.AccessInfo.AccessType));
                const auto type =
                    memory_access.AccessInfo.GpaUnmapped ? memory_violation_type::unmapped : memory_violation_type::protection;

                for (auto& [_, hook] : this->memory_violation_hooks_)
                {
                    const auto result = hook(mmio_address, 1, operation, type);
                    if (result == memory_violation_continuation::resume || result == memory_violation_continuation::restart)
                    {
                        return true;
                    }
                }

                return false;
            }

            bool handle_exception(const WHV_RUN_VP_EXIT_CONTEXT& exit_context)
            {
                const auto& exception = exit_context.VpException;

                if (exception.ExceptionType == WHvX64ExceptionTypeDebugTrapOrFault && this->pending_mmio_step_.has_value())
                {
                    const auto swallow = this->complete_pending_mmio_step();
                    if (swallow)
                    {
                        return true;
                    }
                }

                if (exception.ExceptionType == WHvX64ExceptionTypeBreakpointTrap && this->syscall_hook_ != nullptr)
                {
                    const auto rip = exit_context.VpContext.Rip;
                    if (rip == this->syscall_hook_page_ || rip == (this->syscall_hook_page_ + 1))
                    {
                        return this->handle_syscall_halt();
                    }
                }

                if (exception.ExceptionType == WHvX64ExceptionTypeInvalidOpcodeFault)
                {
                    bool consumed = false;

                    for (auto& [_, hook] : this->instruction_hooks_)
                    {
                        if (hook.type != x86_hookable_instructions::invalid)
                        {
                            continue;
                        }

                        if (hook.callback(0) == instruction_hook_continuation::skip_instruction)
                        {
                            this->advance_rip(exception.InstructionByteCount);
                            consumed = true;
                        }
                    }

                    if (consumed)
                    {
                        return true;
                    }
                }

                for (auto& [_, hook] : this->interrupt_hooks_)
                {
                    hook(exception.ExceptionType);
                    return true;
                }

                return false;
            }

            void advance_rip(const uint64_t amount)
            {
                auto rip = this->get_register(WHvX64RegisterRip);
                rip.Reg64 += amount;
                this->set_register(WHvX64RegisterRip, rip);
            }

            void clear_pending_exception_state()
            {
                WHV_REGISTER_VALUE pending_interruption{};
                pending_interruption.PendingInterruption.AsUINT64 = 0;
                this->set_register(WHvRegisterPendingInterruption, pending_interruption);

                WHV_REGISTER_VALUE pending_event{};
                pending_event.ExceptionEvent.AsUINT128 = {};
                this->set_register(WHvRegisterPendingEvent, pending_event);

                WHV_REGISTER_VALUE pending_debug{};
                pending_debug.PendingDebugException.AsUINT64 = 0;
                this->set_register(WHvX64RegisterPendingDebugException, pending_debug);
            }
        };
    }

    std::unique_ptr<x86_64_emulator> create_x86_64_emulator()
    {
        return std::make_unique<whp_x86_64_emulator>();
    }
}
