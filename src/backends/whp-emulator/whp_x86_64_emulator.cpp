#define WHP_EMULATOR_IMPL
#include "whp_x86_64_emulator.hpp"

#include <WinHvPlatform.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <address_utils.hpp>
#include <utils/object.hpp>
#include <utils/cpu_features.hpp>

namespace sogen::whp
{
    namespace
    {
        constexpr size_t maximum_vcpu_count = 64;
        constexpr uint64_t page_size = 0x1000;
        constexpr uint64_t trap_flag_bit = 0x100ull;
        constexpr uint64_t syscall_instruction_size = 2;
        constexpr uint64_t page_table_entry_present = 1ull << 0;
        constexpr uint64_t page_table_entry_writable = 1ull << 1;
        constexpr uint64_t page_table_entry_user = 1ull << 2;
        constexpr uint64_t page_table_entry_address_mask = 0x000FFFFFFFFFF000ull;
        constexpr uint64_t guest_physical_memory_base = 0x0000000100000000ull;
        constexpr uint64_t internal_page_table_base = 0x0000007000000000ull;
        constexpr uint64_t unmapped_guest_page = (std::numeric_limits<uint64_t>::max)();

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
            virtual_processor_handle(const WHV_PARTITION_HANDLE partition, const UINT32 vp_index)
                : partition_(partition),
                  vp_index_(vp_index)
            {
                WHP_CHECK_HR(WHvCreateVirtualProcessor(this->partition_, this->vp_index_, 0));
            }

            virtual_processor_handle(const virtual_processor_handle&) = delete;
            virtual_processor_handle& operator=(const virtual_processor_handle&) = delete;

            ~virtual_processor_handle()
            {
                if (this->partition_ != nullptr)
                {
                    (void)WHvDeleteVirtualProcessor(this->partition_, this->vp_index_);
                }
            }

          private:
            WHV_PARTITION_HANDLE partition_ = nullptr;
            UINT32 vp_index_ = 0;
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
            uint64_t guest_physical_address = unmapped_guest_page;
            uint64_t guest_page_base = unmapped_guest_page;
            uint32_t map_flags = 0;
            size_t page_execution_hook_count = 0;
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
            fp_last_instruction,
            fp_last_data,
            xmm_control,
            zero,
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
                return {
                    .name = WHvX64RegisterRax,
                    .kind = register_kind::reg64,
                    .logical_size = sizeof(uint64_t),
                };
            case x86_register::bl:
            case x86_register::bh:
            case x86_register::bx:
            case x86_register::ebx:
            case x86_register::rbx:
                return {
                    .name = WHvX64RegisterRbx,
                    .kind = register_kind::reg64,
                    .logical_size = sizeof(uint64_t),
                };
            case x86_register::cl:
            case x86_register::ch:
            case x86_register::cx:
            case x86_register::ecx:
            case x86_register::rcx:
                return {
                    .name = WHvX64RegisterRcx,
                    .kind = register_kind::reg64,
                    .logical_size = sizeof(uint64_t),
                };
            case x86_register::dl:
            case x86_register::dh:
            case x86_register::dx:
            case x86_register::edx:
            case x86_register::rdx:
                return {
                    .name = WHvX64RegisterRdx,
                    .kind = register_kind::reg64,
                    .logical_size = sizeof(uint64_t),
                };
            case x86_register::si:
            case x86_register::sil:
            case x86_register::esi:
            case x86_register::rsi:
                return {
                    .name = WHvX64RegisterRsi,
                    .kind = register_kind::reg64,
                    .logical_size = sizeof(uint64_t),
                };
            case x86_register::di:
            case x86_register::dil:
            case x86_register::edi:
            case x86_register::rdi:
                return {
                    .name = WHvX64RegisterRdi,
                    .kind = register_kind::reg64,
                    .logical_size = sizeof(uint64_t),
                };
            case x86_register::bp:
            case x86_register::bpl:
            case x86_register::ebp:
            case x86_register::rbp:
                return {
                    .name = WHvX64RegisterRbp,
                    .kind = register_kind::reg64,
                    .logical_size = sizeof(uint64_t),
                };
            case x86_register::sp:
            case x86_register::spl:
            case x86_register::esp:
            case x86_register::rsp:
                return {
                    .name = WHvX64RegisterRsp,
                    .kind = register_kind::reg64,
                    .logical_size = sizeof(uint64_t),
                };
            case x86_register::ip:
            case x86_register::eip:
            case x86_register::rip:
                return {
                    .name = WHvX64RegisterRip,
                    .kind = register_kind::reg64,
                    .logical_size = sizeof(uint64_t),
                };
            case x86_register::r8:
            case x86_register::r8d:
            case x86_register::r8w:
            case x86_register::r8b:
                return {
                    .name = WHvX64RegisterR8,
                    .kind = register_kind::reg64,
                    .logical_size = sizeof(uint64_t),
                };
            case x86_register::r9:
            case x86_register::r9d:
            case x86_register::r9w:
            case x86_register::r9b:
                return {
                    .name = WHvX64RegisterR9,
                    .kind = register_kind::reg64,
                    .logical_size = sizeof(uint64_t),
                };
            case x86_register::r10:
            case x86_register::r10d:
            case x86_register::r10w:
            case x86_register::r10b:
                return {
                    .name = WHvX64RegisterR10,
                    .kind = register_kind::reg64,
                    .logical_size = sizeof(uint64_t),
                };
            case x86_register::r11:
            case x86_register::r11d:
            case x86_register::r11w:
            case x86_register::r11b:
                return {
                    .name = WHvX64RegisterR11,
                    .kind = register_kind::reg64,
                    .logical_size = sizeof(uint64_t),
                };
            case x86_register::r12:
            case x86_register::r12d:
            case x86_register::r12w:
            case x86_register::r12b:
                return {
                    .name = WHvX64RegisterR12,
                    .kind = register_kind::reg64,
                    .logical_size = sizeof(uint64_t),
                };
            case x86_register::r13:
            case x86_register::r13d:
            case x86_register::r13w:
            case x86_register::r13b:
                return {
                    .name = WHvX64RegisterR13,
                    .kind = register_kind::reg64,
                    .logical_size = sizeof(uint64_t),
                };
            case x86_register::r14:
            case x86_register::r14d:
            case x86_register::r14w:
            case x86_register::r14b:
                return {
                    .name = WHvX64RegisterR14,
                    .kind = register_kind::reg64,
                    .logical_size = sizeof(uint64_t),
                };
            case x86_register::r15:
            case x86_register::r15d:
            case x86_register::r15w:
            case x86_register::r15b:
                return {
                    .name = WHvX64RegisterR15,
                    .kind = register_kind::reg64,
                    .logical_size = sizeof(uint64_t),
                };
            case x86_register::flags:
                return {
                    .name = WHvX64RegisterRflags,
                    .kind = register_kind::reg64,
                    .logical_size = sizeof(uint16_t),
                };
            case x86_register::eflags:
                return {
                    .name = WHvX64RegisterRflags,
                    .kind = register_kind::reg64,
                    .logical_size = sizeof(uint32_t),
                };
            case x86_register::rflags:
                return {
                    .name = WHvX64RegisterRflags,
                    .kind = register_kind::reg64,
                    .logical_size = sizeof(uint64_t),
                };
            case x86_register::cs:
                return {
                    .name = WHvX64RegisterCs,
                    .kind = register_kind::segment,
                    .logical_size = sizeof(uint16_t),
                };
            case x86_register::ss:
                return {
                    .name = WHvX64RegisterSs,
                    .kind = register_kind::segment,
                    .logical_size = sizeof(uint16_t),
                };
            case x86_register::ds:
                return {
                    .name = WHvX64RegisterDs,
                    .kind = register_kind::segment,
                    .logical_size = sizeof(uint16_t),
                };
            case x86_register::es:
                return {
                    .name = WHvX64RegisterEs,
                    .kind = register_kind::segment,
                    .logical_size = sizeof(uint16_t),
                };
            case x86_register::fs:
                return {
                    .name = WHvX64RegisterFs,
                    .kind = register_kind::segment,
                    .logical_size = sizeof(uint16_t),
                };
            case x86_register::fs_base:
                return {
                    .name = WHvX64RegisterFs,
                    .kind = register_kind::segment,
                    .logical_size = sizeof(uint64_t),
                };
            case x86_register::gs:
                return {
                    .name = WHvX64RegisterGs,
                    .kind = register_kind::segment,
                    .logical_size = sizeof(uint16_t),
                };
            case x86_register::gs_base:
                return {
                    .name = WHvX64RegisterGs,
                    .kind = register_kind::segment,
                    .logical_size = sizeof(uint64_t),
                };
            case x86_register::gdtr:
                return {
                    .name = WHvX64RegisterGdtr,
                    .kind = register_kind::table,
                    .logical_size = sizeof(WHV_X64_TABLE_REGISTER),
                };
            case x86_register::idtr:
                return {
                    .name = WHvX64RegisterIdtr,
                    .kind = register_kind::table,
                    .logical_size = sizeof(WHV_X64_TABLE_REGISTER),
                };
            case x86_register::cr0:
                return {
                    .name = WHvX64RegisterCr0,
                    .kind = register_kind::reg64,
                    .logical_size = sizeof(uint64_t),
                };
            case x86_register::cr2:
                return {
                    .name = WHvX64RegisterCr2,
                    .kind = register_kind::reg64,
                    .logical_size = sizeof(uint64_t),
                };
            case x86_register::cr3:
                return {
                    .name = WHvX64RegisterCr3,
                    .kind = register_kind::reg64,
                    .logical_size = sizeof(uint64_t),
                };
            case x86_register::cr4:
                return {
                    .name = WHvX64RegisterCr4,
                    .kind = register_kind::reg64,
                    .logical_size = sizeof(uint64_t),
                };
            case x86_register::dr0:
                return {
                    .name = WHvX64RegisterDr0,
                    .kind = register_kind::reg64,
                    .logical_size = sizeof(uint64_t),
                };
            case x86_register::dr1:
                return {
                    .name = WHvX64RegisterDr1,
                    .kind = register_kind::reg64,
                    .logical_size = sizeof(uint64_t),
                };
            case x86_register::dr2:
                return {
                    .name = WHvX64RegisterDr2,
                    .kind = register_kind::reg64,
                    .logical_size = sizeof(uint64_t),
                };
            case x86_register::dr3:
                return {
                    .name = WHvX64RegisterDr3,
                    .kind = register_kind::reg64,
                    .logical_size = sizeof(uint64_t),
                };
            case x86_register::dr6:
                return {
                    .name = WHvX64RegisterDr6,
                    .kind = register_kind::reg64,
                    .logical_size = sizeof(uint64_t),
                };
            case x86_register::dr7:
                return {
                    .name = WHvX64RegisterDr7,
                    .kind = register_kind::reg64,
                    .logical_size = sizeof(uint64_t),
                };
            case x86_register::fpcw:
            case x86_register::fpsw:
            case x86_register::fptag:
                return {
                    .name = WHvX64RegisterFpControlStatus,
                    .kind = register_kind::fp_control,
                    .logical_size = sizeof(uint16_t),
                };
            case x86_register::fip:
            case x86_register::fcs:
            case x86_register::fop:
                return {
                    .name = WHvX64RegisterFpControlStatus,
                    .kind = register_kind::fp_last_instruction,
                    .logical_size = sizeof(uint32_t),
                };
            case x86_register::fdp:
            case x86_register::fds:
                return {
                    .name = WHvX64RegisterXmmControlStatus,
                    .kind = register_kind::fp_last_data,
                    .logical_size = sizeof(uint32_t),
                };
            case x86_register::mxcsr:
                return {
                    .name = WHvX64RegisterXmmControlStatus,
                    .kind = register_kind::xmm_control,
                    .logical_size = sizeof(uint32_t),
                };
            case x86_register::msr:
                return {
                    .name = WHvX64RegisterEfer,
                    .kind = register_kind::reg64,
                    .logical_size = sizeof(uint64_t),
                };
            default:
                break;
            }

            if (reg >= x86_register::xmm0 && reg <= x86_register::xmm15)
            {
                const auto index = static_cast<int>(reg) - static_cast<int>(x86_register::xmm0);
                return {
                    .name = static_cast<WHV_REGISTER_NAME>(WHvX64RegisterXmm0 + index),
                    .kind = register_kind::reg128,
                    .logical_size = sizeof(WHV_UINT128),
                };
            }

            // WHP's AVX register layout does not match the debugger-facing YMM register
            // layout. Until we translate it explicitly, expose YMM as synthetic zero-filled
            // state rather than returning malformed data.
            if (reg >= x86_register::ymm0 && reg <= x86_register::ymm15)
            {
                return {
                    .name = {}, // no WHV_REGISTER_NAME exists for this
                    .kind = register_kind::zero,
                    .logical_size = 32,
                };
            }

            if (reg >= x86_register::st0 && reg <= x86_register::st7)
            {
                const auto index = static_cast<int>(reg) - static_cast<int>(x86_register::st0);
                return {
                    .name = static_cast<WHV_REGISTER_NAME>(WHvX64RegisterFpMmx0 + index),
                    .kind = register_kind::fp,
                    .logical_size = 10,
                };
            }

            throw std::runtime_error("Unsupported WHP register");
        }

        class whp_x86_64_emulator;

        struct pending_mmio_step
        {
            uint64_t page_base{};
            uint64_t original_rflags{};
            bool had_trap_flag{};
            bool allow_debug_delivery{};
            bool is_write{};
            std::vector<std::byte> old_page{};
        };

        struct pending_execution_step
        {
            std::optional<uint64_t> page_base{};
            std::optional<uint64_t> patched_breakpoint{};
            bool had_trap_flag{};
            bool stop_after_step{};
        };

        class whp_vcpu final : public x86_64_cpu
        {
          public:
            whp_vcpu(whp_x86_64_emulator& emulator, const WHV_PARTITION_HANDLE partition, const UINT32 index)
                : emulator_(emulator),
                  partition_(partition),
                  vp_index_(index),
                  vp_(partition, index)
            {
            }

            size_t index() const override
            {
                return this->vp_index_;
            }

            memory_interface& memory() override;
            const memory_interface& memory() const override;

            void start(size_t count) override;

            void stop() override
            {
                this->stop_requested_ = true;
                if (this->run_active_)
                {
                    WHP_CHECK_HR(WHvCancelRunVirtualProcessor(this->partition_, this->vp_index_, 0));
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
                case register_kind::fp_last_instruction:
                    if (static_cast<x86_register>(reg) == x86_register::fip)
                    {
                        std::memcpy(&current.FpControlStatus.LastFpEip, value, (std::min)(size, sizeof(current.FpControlStatus.LastFpEip)));
                    }
                    else if (static_cast<x86_register>(reg) == x86_register::fcs)
                    {
                        std::memcpy(&current.FpControlStatus.LastFpCs, value, (std::min)(size, sizeof(current.FpControlStatus.LastFpCs)));
                    }
                    else
                    {
                        std::memcpy(&current.FpControlStatus.LastFpOp, value, (std::min)(size, sizeof(current.FpControlStatus.LastFpOp)));
                    }
                    break;
                case register_kind::fp_last_data:
                    if (static_cast<x86_register>(reg) == x86_register::fdp)
                    {
                        std::memcpy(&current.XmmControlStatus.LastFpDp, value, (std::min)(size, sizeof(current.XmmControlStatus.LastFpDp)));
                    }
                    else
                    {
                        std::memcpy(&current.XmmControlStatus.LastFpDs, value, (std::min)(size, sizeof(current.XmmControlStatus.LastFpDs)));
                    }
                    break;
                case register_kind::xmm_control:
                    std::memcpy(&current.XmmControlStatus.XmmStatusControl, value,
                                (std::min)(size, sizeof(current.XmmControlStatus.XmmStatusControl)));
                    current.XmmControlStatus.XmmStatusControlMask = 0xFFFFFFFF;
                    break;
                case register_kind::zero:
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
                case register_kind::fp_last_instruction:
                    if (static_cast<x86_register>(reg) == x86_register::fip)
                    {
                        std::memcpy(value, &current.FpControlStatus.LastFpEip, (std::min)(size, sizeof(current.FpControlStatus.LastFpEip)));
                    }
                    else if (static_cast<x86_register>(reg) == x86_register::fcs)
                    {
                        uint32_t fp_cs = current.FpControlStatus.LastFpCs;
                        std::memcpy(value, &fp_cs, (std::min)(size, sizeof(fp_cs)));
                    }
                    else
                    {
                        uint32_t fp_op = current.FpControlStatus.LastFpOp;
                        std::memcpy(value, &fp_op, (std::min)(size, sizeof(fp_op)));
                    }
                    break;
                case register_kind::fp_last_data:
                    if (static_cast<x86_register>(reg) == x86_register::fdp)
                    {
                        std::memcpy(value, &current.XmmControlStatus.LastFpDp, (std::min)(size, sizeof(current.XmmControlStatus.LastFpDp)));
                    }
                    else
                    {
                        uint32_t fp_ds = current.XmmControlStatus.LastFpDs;
                        std::memcpy(value, &fp_ds, (std::min)(size, sizeof(fp_ds)));
                    }
                    break;
                case register_kind::xmm_control:
                    std::memcpy(value, &current.XmmControlStatus.XmmStatusControl,
                                (std::min)(size, sizeof(current.XmmControlStatus.XmmStatusControl)));
                    break;
                case register_kind::zero:
                    std::memset(value, 0, (std::min)(size, mapping.logical_size));
                    break;
                case register_kind::reg128:
                    std::memcpy(value, &current.Reg128, (std::min)(size, sizeof(current.Reg128)));
                    break;
                }

                return mapping.logical_size;
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

            std::vector<std::byte> save_registers() const override
            {
                auto names = snapshot_register_names();
                std::vector<WHV_REGISTER_VALUE> values(names.size());
                WHP_CHECK_HR(WHvGetVirtualProcessorRegisters(this->partition_, this->vp_index_, names.data(),
                                                             static_cast<UINT32>(names.size()), values.data()));

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

                WHP_CHECK_HR(WHvSetVirtualProcessorRegisters(this->partition_, this->vp_index_, names.data(),
                                                             static_cast<UINT32>(names.size()), values.data()));
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

            WHV_REGISTER_VALUE get_register(const WHV_REGISTER_NAME name) const
            {
                WHV_REGISTER_VALUE value{};
                WHP_CHECK_HR(WHvGetVirtualProcessorRegisters(this->partition_, this->vp_index_, &name, 1, &value));
                return value;
            }

            template <size_t N>
            std::array<WHV_REGISTER_VALUE, N> get_registers(const std::array<WHV_REGISTER_NAME, N>& names) const
            {
                std::array<WHV_REGISTER_VALUE, N> values{};
                WHP_CHECK_HR(WHvGetVirtualProcessorRegisters(this->partition_, this->vp_index_, names.data(),
                                                             static_cast<UINT32>(names.size()), values.data()));
                return values;
            }

            void set_register(const WHV_REGISTER_NAME name, const WHV_REGISTER_VALUE& value)
            {
                WHP_CHECK_HR(WHvSetVirtualProcessorRegisters(this->partition_, this->vp_index_, &name, 1, &value));
            }

            template <size_t N>
            void set_registers(const std::array<WHV_REGISTER_NAME, N>& names, const std::array<WHV_REGISTER_VALUE, N>& values)
            {
                WHP_CHECK_HR(WHvSetVirtualProcessorRegisters(this->partition_, this->vp_index_, names.data(),
                                                             static_cast<UINT32>(names.size()), values.data()));
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

          private:
            friend class whp_x86_64_emulator;

            whp_x86_64_emulator& emulator_;
            WHV_PARTITION_HANDLE partition_{};
            UINT32 vp_index_{};
            virtual_processor_handle vp_;

            std::atomic_bool stop_requested_{false};
            std::atomic_bool run_active_{false};
            std::atomic_bool pending_tlb_flush_{false};

            // Tracks the last (page, rip) we re-mapped and retried, plus how many times in a row.
            // A backed page whose permissions allow the access can only fault spuriously (a stale
            // translation or a peer transiently re-clearing/reprotecting it) - never a genuine
            // violation, which fails the permission check and is delivered without a repair. So we
            // retry many times to ride out concurrent re-clears at high vCPU counts, but still cap it
            // to bound the one case that could otherwise loop: an unrecoverable-exit fault whose real
            // access type we had to guess (see the unrecoverable handler).
            uint64_t last_repair_page_{unmapped_guest_page};
            uint64_t last_repair_rip_{};
            uint32_t last_repair_count_{};
            std::optional<pending_execution_step> pending_execution_step_{};
            std::optional<pending_mmio_step> pending_mmio_step_{};
            std::optional<uint64_t> deferred_patched_breakpoint_{};
            std::optional<uint64_t> deferred_execution_page_{};
        };

        class whp_x86_64_emulator : public x86_64_emulator
        {
          public:
            explicit whp_x86_64_emulator(const size_t vcpu_count)
            {
                this->ensure_platform_support();
                this->configure_partition(static_cast<UINT32>(vcpu_count));
                this->initialize_long_mode_page_tables();
                this->initialize_syscall_intercept_page();

                this->vcpus_.reserve(vcpu_count);
                for (size_t i = 0; i < vcpu_count; ++i)
                {
                    auto vcpu = std::make_unique<whp_vcpu>(*this, this->partition_, static_cast<UINT32>(i));
                    this->initialize_virtual_processor_state(*vcpu);
                    this->vcpus_.push_back(std::move(vcpu));
                }
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

            void set_memory_execution_hook_mode(const memory_execution_hook_mode mode) override
            {
                std::unique_lock lock(this->partition_mutex_);
                this->memory_execution_hook_mode_ = mode;
            }

            size_t vcpu_count() const override
            {
                return this->vcpus_.size();
            }

            x86_64_cpu& get_cpu(const size_t index) override
            {
                if (index >= this->vcpus_.size())
                {
                    throw std::out_of_range("Invalid vCPU index");
                }

                return *this->vcpus_[index];
            }

            void start(const size_t count) override
            {
                this->vcpus_[0]->start(count);
            }

            void stop() override
            {
                this->vcpus_[0]->stop();
            }

            size_t write_raw_register(const int reg, const void* value, const size_t size) override
            {
                return this->vcpus_[0]->write_raw_register(reg, value, size);
            }

            size_t read_raw_register(const int reg, void* value, const size_t size) override
            {
                return this->vcpus_[0]->read_raw_register(reg, value, size);
            }

            bool read_descriptor_table(const int reg, descriptor_table_register& table) override
            {
                return this->vcpus_[0]->read_descriptor_table(reg, table);
            }

            std::vector<std::byte> save_registers() const override
            {
                return this->vcpus_[0]->save_registers();
            }

            void restore_registers(const std::vector<std::byte>& register_data) override
            {
                this->vcpus_[0]->restore_registers(register_data);
            }

            void load_gdt(const pointer_type address, const uint32_t limit) override
            {
                this->vcpus_[0]->load_gdt(address, limit);
            }

            void set_segment_base(const x86_register base, const pointer_type value) override
            {
                this->vcpus_[0]->set_segment_base(base, value);
            }

            pointer_type get_segment_base(const x86_register base) override
            {
                return this->vcpus_[0]->get_segment_base(base);
            }

            void map_mmio(const uint64_t address, const size_t size, mmio_read_callback read_cb, mmio_write_callback write_cb) override
            {
                if (!is_page_aligned(address) || !is_page_aligned(size))
                {
                    throw std::runtime_error("WHP MMIO mappings must be page aligned");
                }

                std::unique_lock lock(this->partition_mutex_);

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
                        this->assign_guest_physical_page(guest_address, *page);
                        page->permissions = memory_permission::none;
                        this->apply_patched_execution_breakpoints(guest_address);
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
                        this->assign_guest_physical_page(guest_address, *page);
                        this->apply_patched_execution_breakpoints(guest_address);
                    }

                    page->permissions = memory_permission::none;
                    this->remap_page(*page);
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

                std::unique_lock lock(this->partition_mutex_);

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
                        this->assign_guest_physical_page(guest_address, *page);
                        page->permissions = permissions;
                        this->apply_patched_execution_breakpoints(guest_address);
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
                        this->assign_guest_physical_page(guest_address, *page);
                        this->apply_patched_execution_breakpoints(guest_address);
                    }

                    page->permissions = permissions;
                    this->remap_page(*page);
                    this->ensure_virtual_mapping(guest_address);
                }
            }

            void map_host_memory(const uint64_t address, const size_t size, void* host_pointer,
                                 const memory_permission permissions) override
            {
                if (!is_page_aligned(address) || !is_page_aligned(size))
                {
                    throw std::runtime_error("WHP host memory mappings must be page aligned");
                }
                if ((reinterpret_cast<uintptr_t>(host_pointer) % page_size) != 0)
                {
                    throw std::runtime_error("WHP host memory mappings require a page-aligned host pointer");
                }

                std::unique_lock lock(this->partition_mutex_);

                // Back each guest page with the caller's host memory (owned_page stays null so unmap never
                // frees it). WHvMapGpaRange then aliases the guest physical page directly onto that host page,
                // so guest reads and writes hit it coherently.
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
                    this->assign_guest_physical_page(guest_address, *page);
                    page->permissions = permissions;
                    this->apply_patched_execution_breakpoints(guest_address);
                    this->ensure_virtual_mapping(guest_address);
                }

                this->remap_pages(address, size);
            }

            void unmap_memory(const uint64_t address, const size_t size) override
            {
                if (!is_page_aligned(address) || !is_page_aligned(size))
                {
                    throw std::runtime_error("WHP memory unmappings must be page aligned");
                }

                std::unique_lock lock(this->partition_mutex_);

                // Coalesce into one WHvUnmapGpaRange per contiguous guest-physical run: each call is an EPT flush,
                // so freeing a large region page-by-page stalls the guest for seconds. The LIFO GPA free list
                // scatters/reverses a region's GPAs, hence the sort. Free high-to-low so the indices pop back
                // ascending on the next allocation, keeping remap_pages' map-side coalescing effective too.
                std::vector<uint64_t> unmap_gpas;
                // Keep each page's host backing alive until after the unmaps below: erasing the mapped_page drops
                // its (shared) owned_page, which can free memory the WHP partition still maps by GPA.
                std::vector<std::shared_ptr<uint8_t>> retired_backings;
                bool flushed_virtual_mappings = false;
                for (size_t offset = size; offset > 0; offset -= page_size)
                {
                    const auto guest_address = address + offset - page_size;
                    this->revoke_mmio_read_grace(guest_address);

                    const auto entry = this->mapped_pages_.find(guest_address);
                    if (entry == this->mapped_pages_.end())
                    {
                        continue;
                    }

                    if (entry->second->map_flags != 0)
                    {
                        unmap_gpas.push_back(entry->second->guest_physical_address);
                    }

                    flushed_virtual_mappings = this->clear_virtual_mapping(guest_address) || flushed_virtual_mappings;
                    this->mark_patched_execution_breakpoints_unmapped(guest_address);
                    this->release_guest_physical_page(entry->second->guest_physical_address);
                    retired_backings.push_back(std::move(entry->second->owned_page));
                    this->mapped_pages_.erase(entry);
                }

                std::ranges::sort(unmap_gpas);
                for (size_t i = 0; i < unmap_gpas.size();)
                {
                    size_t j = i;
                    while (j + 1 < unmap_gpas.size() && unmap_gpas[j + 1] == unmap_gpas[j] + page_size)
                    {
                        ++j;
                    }

                    WHP_CHECK_HR(WHvUnmapGpaRange(this->partition_, unmap_gpas[i], (j - i + 1) * page_size));
                    i = j + 1;
                }

                if (flushed_virtual_mappings)
                {
                    this->flush_virtual_address_mappings();
                }

                const auto mmio = this->mmio_regions_.find(address);
                if (mmio != this->mmio_regions_.end() && mmio->second.size == size)
                {
                    this->mmio_regions_.erase(mmio);
                }
            }

            bool try_read_memory(const uint64_t address, void* data, const size_t size) const override
            {
                std::shared_lock lock(this->partition_mutex_);

                if (!this->access_memory(address, data, size, false))
                {
                    return false;
                }

                this->overlay_patched_breakpoints(address, data, size);
                return true;
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
                // Exclusive: the breakpoint overlay below may update patched-breakpoint bookkeeping.
                std::unique_lock lock(this->partition_mutex_);

                if (!this->access_memory(address, const_cast<void*>(data), size, true))
                {
                    return false;
                }

                this->overlay_patched_breakpoints(address, data, size);
                return true;
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

                std::unique_lock lock(this->partition_mutex_);

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

            emulator_hook* hook_instruction(const int instruction_type, instruction_hook_callback callback) override
            {
                std::unique_lock lock(this->partition_mutex_);

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
                std::unique_lock lock(this->partition_mutex_);

                auto* hook = this->make_hook();
                this->basic_block_hooks_[hook] = std::move(callback);
                return hook;
            }

            emulator_hook* hook_interrupt(interrupt_hook_callback callback) override
            {
                std::unique_lock lock(this->partition_mutex_);

                auto* hook = this->make_hook();
                this->interrupt_hooks_[hook] = std::move(callback);
                return hook;
            }

            emulator_hook* hook_memory_violation(memory_violation_hook_callback callback) override
            {
                std::unique_lock lock(this->partition_mutex_);

                auto* hook = this->make_hook();
                this->memory_violation_hooks_[hook] = std::move(callback);
                return hook;
            }

            emulator_hook* hook_memory_execution(memory_execution_hook_callback callback) override
            {
                // NOTE: Global memory execution hooks are registered but not currently honored.
                std::unique_lock lock(this->partition_mutex_);

                auto* hook = this->make_hook();
                this->memory_execution_hooks_[hook] = execution_hook_entry{.address = std::nullopt, .callback = std::move(callback)};
                return hook;
            }

            emulator_hook* hook_memory_range_execution(const uint64_t address, const uint64_t size,
                                                       memory_execution_hook_callback callback) override
            {
                if (size == 1)
                {
                    return this->hook_memory_execution(address, std::move(callback));
                }

                std::unique_lock lock(this->partition_mutex_);

                auto* hook = this->make_hook();
                this->memory_execution_hooks_[hook] =
                    execution_hook_entry{.address = address, .size = size, .callback = std::move(callback)};
                this->increment_execution_hook_count(address, size);
                this->remap_hook_pages(address, size);
                return hook;
            }

            emulator_hook* hook_memory_execution(const uint64_t address, memory_execution_hook_callback callback) override
            {
                std::unique_lock lock(this->partition_mutex_);

                auto* hook = this->make_hook();

                switch (this->memory_execution_hook_mode_)
                {
                case memory_execution_hook_mode::automatic:
                    this->memory_execution_hooks_[hook] = execution_hook_entry{.address = address, .callback = std::move(callback)};
                    if (const auto entry = this->mapped_pages_.find(align_down_to_page(address));
                        entry != this->mapped_pages_.end() && entry->second)
                    {
                        ++entry->second->page_execution_hook_count;
                    }

                    this->remap_hook_page(address);
                    return hook;

                case memory_execution_hook_mode::int3:
                    this->install_patched_execution_breakpoint(address);
                    this->memory_execution_hooks_[hook] =
                        execution_hook_entry{.address = address, .patched_breakpoint = true, .callback = std::move(callback)};
                    return hook;
                }

                throw std::runtime_error("Unknown memory execution hook mode");
            }

            emulator_hook* hook_memory_read(const uint64_t address, const uint64_t size, memory_access_hook_callback callback) override
            {
                std::unique_lock lock(this->partition_mutex_);

                auto* hook = this->make_hook();
                this->memory_read_hooks_[hook] =
                    memory_access_hook_entry{.address = address, .size = size, .callback = std::move(callback)};
                return hook;
            }

            emulator_hook* hook_memory_write(const uint64_t address, const uint64_t size, memory_access_hook_callback callback) override
            {
                std::unique_lock lock(this->partition_mutex_);

                auto* hook = this->make_hook();
                this->memory_write_hooks_[hook] =
                    memory_access_hook_entry{.address = address, .size = size, .callback = std::move(callback)};
                return hook;
            }

            void delete_hook(emulator_hook* hook) override
            {
                std::unique_lock lock(this->partition_mutex_);

                const auto instruction_it = this->instruction_hooks_.find(hook);
                if (instruction_it != this->instruction_hooks_.end() && &instruction_it->second == this->syscall_hook_)
                {
                    this->syscall_hook_ = nullptr;
                }

                this->instruction_hooks_.erase(hook);
                this->basic_block_hooks_.erase(hook);
                this->interrupt_hooks_.erase(hook);
                this->memory_violation_hooks_.erase(hook);

                if (const auto execution_it = this->memory_execution_hooks_.find(hook); execution_it != this->memory_execution_hooks_.end())
                {
                    this->deactivate_execution_hook(hook);
                    this->memory_execution_hooks_.erase(execution_it);
                }

                this->memory_read_hooks_.erase(hook);
                this->memory_write_hooks_.erase(hook);
            }

            void serialize_state(utils::buffer_serializer& buffer, const bool) const override
            {
                if (this->vcpus_.size() > 1)
                {
                    throw std::runtime_error("Multi-vCPU snapshots are not supported yet");
                }

                buffer.write_vector(this->vcpus_[0]->save_registers());
            }

            void deserialize_state(utils::buffer_deserializer& buffer, const bool) override
            {
                if (this->vcpus_.size() > 1)
                {
                    throw std::runtime_error("Multi-vCPU snapshots are not supported yet");
                }

                this->vcpus_[0]->restore_registers(buffer.read_vector<std::byte>());
            }

            bool has_violation() const override
            {
                return this->vcpus_[0]->has_violation();
            }

            std::string get_name() const override
            {
                return "Windows Hypervisor Platform";
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
                return true;
            }

            bool supports_global_memory_execution_hooks() const override
            {
                return false;
            }

          private:
            friend class whp_vcpu;

            struct instruction_hook_entry
            {
                x86_hookable_instructions type = x86_hookable_instructions::invalid;
                instruction_hook_callback callback{};
            };

            struct execution_hook_entry
            {
                std::optional<uint64_t> address{};
                uint64_t size{1};
                bool patched_breakpoint{false};
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

            struct mmio_write_chunk
            {
                size_t offset{};
                std::vector<std::byte> data{};
            };

            struct patched_execution_breakpoint
            {
                std::optional<std::byte> original_byte{};
                size_t hook_count{};
                bool applied{};
            };

            partition_handle partition_{};
            WHV_EXTENDED_VM_EXITS supported_exits_{};
            WHV_PROCESSOR_XSAVE_FEATURES supported_xsave_features_{};
            bool has_supported_xsave_features_ = false;

            // Guards all partition-shared state below (page/GPA tables, MMIO regions, hook containers).
            // Discipline: acquired only at public entry points and at the top-level exit handlers of the
            // run loop; interior helpers assume it is already held. Non-recursive — never re-acquired.
            // Never held while invoking a user callback or across WHvRunVirtualProcessor. Construction
            // runs single-threaded, before any vCPU can execute, and takes no lock.
            mutable std::shared_mutex partition_mutex_{};

            std::unordered_map<uint64_t, std::unique_ptr<mapped_page>> mapped_pages_{};
            std::vector<uint64_t> guest_pages_by_gpa_{};
            // Ordered free list of guest-physical page indices. Allocating the *lowest* free index (rather
            // than LIFO) hands a freed contiguous GPA block back in ascending order to the next mapping's
            // ascending guest pages, so a contiguous guest region keeps monotonic contiguous GPAs. That lets
            // remap_pages fold the EPT mapping into a few large WHvMapGpaRange calls instead of thousands of
            // single-page flushes, which otherwise dominates frames that map/unmap memory heavily.
            std::set<size_t> free_guest_gpa_indices_{};
            std::unordered_map<uint64_t, uint64_t*> page_table_views_{};
            uint64_t pml4_gpa_ = 0;
            uint64_t next_internal_gpa_ = internal_page_table_base;
            uint64_t syscall_hook_page_ = 0;
            size_t next_hook_id_ = 1;

            std::unordered_map<emulator_hook*, instruction_hook_entry> instruction_hooks_{};
            std::unordered_map<emulator_hook*, basic_block_hook_callback> basic_block_hooks_{};
            std::unordered_map<emulator_hook*, interrupt_hook_callback> interrupt_hooks_{};
            std::unordered_map<emulator_hook*, memory_violation_hook_callback> memory_violation_hooks_{};
            std::unordered_map<emulator_hook*, execution_hook_entry> memory_execution_hooks_{};
            std::unordered_map<emulator_hook*, memory_access_hook_entry> memory_read_hooks_{};
            std::unordered_map<emulator_hook*, memory_access_hook_entry> memory_write_hooks_{};
            std::unordered_map<uint64_t, patched_execution_breakpoint> patched_execution_breakpoints_{};
            std::unordered_map<uint64_t, mmio_region> mmio_regions_{};
            std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> mmio_read_grace_deadlines_{};
            // Installed once during setup, before any vCPU runs, and read-only afterwards; the syscall
            // dispatch hot path deliberately reads it without taking partition_mutex_.
            instruction_hook_entry* syscall_hook_ = nullptr;
            memory_execution_hook_mode memory_execution_hook_mode_ = memory_execution_hook_mode::automatic;

            std::vector<std::unique_ptr<whp_vcpu>> vcpus_{};

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

            void configure_partition(const UINT32 processor_count)
            {
                WHP_CHECK_HR(WHvSetPartitionProperty(this->partition_, WHvPartitionPropertyCodeProcessorCount, &processor_count,
                                                     sizeof(processor_count)));

                WHV_EXTENDED_VM_EXITS enabled_exits{};
                enabled_exits.ExceptionExit = this->supported_exits_.ExceptionExit ? 1 : 0;
                enabled_exits.X64CpuidExit = this->supported_exits_.X64CpuidExit ? 1 : 0;
                enabled_exits.X64RdtscExit = this->supported_exits_.X64RdtscExit ? 1 : 0;

                WHP_CHECK_HR(WHvSetPartitionProperty(this->partition_, WHvPartitionPropertyCodeExtendedVmExits, &enabled_exits,
                                                     sizeof(enabled_exits)));

                if (enabled_exits.ExceptionExit)
                {
                    WHV_PARTITION_PROPERTY exception_exit_bitmap{};
                    exception_exit_bitmap.ExceptionExitBitmap =
                        (1ull << WHvX64ExceptionTypeDebugTrapOrFault) | (1ull << WHvX64ExceptionTypeBreakpointTrap) |
                        (1ull << WHvX64ExceptionTypeInvalidOpcodeFault) | (1ull << WHvX64ExceptionTypePageFault) |
                        (1ull << WHvX64ExceptionTypeFloatingPointErrorFault) | (1ull << WHvX64ExceptionTypeSimdFloatingPointFault);

                    WHP_CHECK_HR(WHvSetPartitionProperty(this->partition_, WHvPartitionPropertyCodeExceptionExitBitmap,
                                                         &exception_exit_bitmap, sizeof(exception_exit_bitmap)));
                }

                WHP_CHECK_HR(WHvSetupPartition(this->partition_));
            }

            void initialize_virtual_processor_state(whp_vcpu& vcpu)
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
                    WHvX64RegisterLstar,
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
                values[15].Reg64 = this->syscall_hook_page_;

                if (this->has_supported_xsave_features_)
                {
                    names.push_back(WHvX64RegisterXCr0);
                    WHV_REGISTER_VALUE xcr0{};
                    xcr0.Reg64 = 0x3ull;
                    if (utils::cpu_features::avx_enabled())
                    {
                        xcr0.Reg64 |= 0x4ull; // Enable YMM state
                    }
                    values.push_back(xcr0);
                }

                WHP_CHECK_HR(WHvSetVirtualProcessorRegisters(this->partition_, vcpu.vp_index_, names.data(),
                                                             static_cast<UINT32>(names.size()), values.data()));
            }

            void initialize_syscall_intercept_page()
            {
                this->syscall_hook_page_ = this->allocate_internal_page(true);
                auto* code = static_cast<uint8_t*>(this->mapped_pages_.at(this->syscall_hook_page_)->host_page);
                code[0] = 0xF4;
            }

            // Hot path: reads only registers and the setup-frozen syscall hook, so it takes no lock.
            bool handle_syscall_halt(whp_vcpu& vcpu)
            {
                if (!this->syscall_hook_)
                {
                    return false;
                }

                constexpr std::array<WHV_REGISTER_NAME, 7> entry_names = {
                    WHvX64RegisterRip,    WHvX64RegisterRcx, WHvX64RegisterR10, WHvX64RegisterR11,
                    WHvX64RegisterRflags, WHvX64RegisterCs,  WHvX64RegisterSs,
                };
                auto entry_values = vcpu.get_registers(entry_names);

                const auto post_syscall_rcx = entry_values[1].Reg64;
                const auto post_syscall_r10 = entry_values[2].Reg64;
                const auto saved_rflags = entry_values[3].Reg64;

                const auto pre_syscall_rip = post_syscall_rcx - syscall_instruction_size;

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
                vcpu.set_registers(pre_hook_names, pre_hook_values);

                const auto continuation = this->syscall_hook_->callback(vcpu, 0);

                constexpr std::array<WHV_REGISTER_NAME, 1> post_hook_rip_name = {WHvX64RegisterRip};
                auto post_hook_rip_value = vcpu.get_registers(post_hook_rip_name);
                if (continuation != instruction_hook_continuation::finalized_instruction_pointer)
                {
                    if (continuation == instruction_hook_continuation::skip_instruction && post_hook_rip_value[0].Reg64 == pre_syscall_rip)
                    {
                        post_hook_rip_value[0].Reg64 = post_syscall_rcx;
                    }
                    else
                    {
                        post_hook_rip_value[0].Reg64 += syscall_instruction_size;
                    }
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
                vcpu.set_registers(post_hook_names, post_hook_values);

                return true;
            }

            void initialize_long_mode_page_tables()
            {
                this->pml4_gpa_ = this->allocate_internal_page(false, false);
            }

            // Assumes partition_mutex_ is held exclusively (or single-threaded construction).
            uint64_t allocate_internal_page(const bool executable = false, const bool map_into_guest = true)
            {
                auto backing = allocate_backing_memory(page_size);
                auto* raw_page = backing.get();

                const auto page_gpa = this->next_internal_gpa_;
                this->next_internal_gpa_ += page_size;

                auto page = std::make_unique<mapped_page>();
                page->owned_page = std::move(backing);
                page->host_page = raw_page;
                page->guest_physical_address = page_gpa;
                page->permissions = executable ? memory_permission::all : memory_permission::read_write;

                this->mapped_pages_[page_gpa] = std::move(page);
                this->remap_page(*this->mapped_pages_[page_gpa]);
                this->page_table_views_[page_gpa] = reinterpret_cast<uint64_t*>(raw_page);

                if (map_into_guest)
                {
                    this->ensure_virtual_mapping(page_gpa);
                }

                return page_gpa;
            }

            // Assumes partition_mutex_ is held exclusively.
            uint64_t allocate_guest_physical_page()
            {
                size_t page_index = 0;
                if (!this->free_guest_gpa_indices_.empty())
                {
                    const auto lowest = this->free_guest_gpa_indices_.begin();
                    page_index = *lowest;
                    this->free_guest_gpa_indices_.erase(lowest);
                    if (this->guest_pages_by_gpa_[page_index] != unmapped_guest_page)
                    {
                        throw std::runtime_error("WHP guest physical free list corruption");
                    }
                }
                else
                {
                    page_index = this->guest_pages_by_gpa_.size();
                    const auto page_gpa = guest_physical_memory_base + (static_cast<uint64_t>(page_index) * page_size);
                    if (page_gpa >= internal_page_table_base)
                    {
                        throw std::runtime_error("WHP guest physical address space exhausted");
                    }

                    this->guest_pages_by_gpa_.push_back(unmapped_guest_page);
                }

                return guest_physical_memory_base + (static_cast<uint64_t>(page_index) * page_size);
            }

            // Assumes partition_mutex_ is held exclusively.
            void assign_guest_physical_page(const uint64_t guest_address, mapped_page& page)
            {
                if (page.guest_physical_address != unmapped_guest_page)
                {
                    throw std::runtime_error("WHP guest physical page already assigned");
                }

                page.guest_page_base = align_down_to_page(guest_address);
                page.guest_physical_address = this->allocate_guest_physical_page();
                page.page_execution_hook_count = std::ranges::count_if(this->memory_execution_hooks_, [&](const auto& entry) {
                    const auto& hook = entry.second;
                    return hook.address && !hook.patched_breakpoint && hook.size != 0 &&
                           regions_with_length_intersect(*hook.address, hook.size, page.guest_page_base, page_size);
                });
                this->guest_pages_by_gpa_[this->guest_physical_page_index(page.guest_physical_address)] = page.guest_page_base;
            }

            // Assumes partition_mutex_ is held exclusively.
            void release_guest_physical_page(const uint64_t guest_physical_address)
            {
                if (guest_physical_address == unmapped_guest_page)
                {
                    throw std::runtime_error("WHP guest physical page released before assignment");
                }

                if (guest_physical_address < guest_physical_memory_base || guest_physical_address >= internal_page_table_base)
                {
                    return;
                }

                const auto page_index = this->guest_physical_page_index(guest_physical_address);
                if (this->guest_pages_by_gpa_[page_index] == unmapped_guest_page)
                {
                    throw std::runtime_error("WHP guest physical page double release");
                }

                this->guest_pages_by_gpa_[page_index] = unmapped_guest_page;
                this->free_guest_gpa_indices_.insert(page_index);
            }

            // Assumes partition_mutex_ is held (shared is sufficient).
            std::optional<uint64_t> translate_guest_physical_address(const uint64_t guest_physical_address) const
            {
                if (guest_physical_address < guest_physical_memory_base)
                {
                    return std::nullopt;
                }

                const auto page_base = align_down_to_page(guest_physical_address);
                const auto page_index = static_cast<size_t>((page_base - guest_physical_memory_base) / page_size);
                if (page_index >= this->guest_pages_by_gpa_.size())
                {
                    return std::nullopt;
                }

                const auto guest_page = this->guest_pages_by_gpa_[page_index];
                if (guest_page == unmapped_guest_page)
                {
                    return std::nullopt;
                }

                return guest_page + (guest_physical_address - page_base);
            }

            size_t guest_physical_page_index(const uint64_t guest_physical_address) const
            {
                if (guest_physical_address < guest_physical_memory_base || !is_page_aligned(guest_physical_address))
                {
                    throw std::runtime_error("Invalid WHP guest physical page");
                }

                const auto page_index = static_cast<size_t>((guest_physical_address - guest_physical_memory_base) / page_size);
                if (page_index >= this->guest_pages_by_gpa_.size())
                {
                    throw std::runtime_error("Unknown WHP guest physical page");
                }

                return page_index;
            }

            // Assumes partition_mutex_ is held. All pending-step mutation happens under the exclusive
            // lock, so scanning the other vCPUs' step state here is safe.
            bool is_page_being_stepped(const uint64_t page_base) const
            {
                for (const auto& vcpu : this->vcpus_)
                {
                    const auto& step = vcpu->pending_execution_step_;
                    if (step && step->page_base == page_base)
                    {
                        return true;
                    }
                }

                return false;
            }

            // Assumes partition_mutex_ is held.
            memory_permission effective_page_permissions(const mapped_page& page) const
            {
                auto permissions = page.permissions;
                if (page.page_execution_hook_count != 0 && !this->is_page_being_stepped(page.guest_page_base))
                {
                    permissions &= ~memory_permission::exec;
                }

                return permissions;
            }

            // Assumes partition_mutex_ is held exclusively.
            void remap_page(mapped_page& page)
            {
                if (page.guest_physical_address == unmapped_guest_page)
                {
                    throw std::runtime_error("WHP page remapped without assigned guest physical address");
                }

                if (page.map_flags != 0)
                {
                    WHP_CHECK_HR(WHvUnmapGpaRange(this->partition_, page.guest_physical_address, page_size));
                }

                page.map_flags = to_whp_map_flags(this->effective_page_permissions(page));
                if (page.map_flags == 0)
                {
                    return;
                }

                WHP_CHECK_HR(WHvMapGpaRange(this->partition_, page.host_page, page.guest_physical_address, page_size,
                                            static_cast<WHV_MAP_GPA_RANGE_FLAGS>(page.map_flags)));
            }

            // Assumes partition_mutex_ is held exclusively.
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
                    const auto run_gpa = entry->second->guest_physical_address;
                    const auto run_flags = to_whp_map_flags(this->effective_page_permissions(*entry->second));
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

                        const auto next_flags = to_whp_map_flags(this->effective_page_permissions(*next_entry->second));
                        const auto next_had_mapping = next_entry->second->map_flags != 0;
                        auto* const expected_host = run_host_base + run_size;
                        const auto expected_gpa = run_gpa + run_size;
                        if (next_entry->second->host_page != expected_host || next_flags != run_flags ||
                            next_had_mapping != run_had_mapping || next_entry->second->guest_physical_address != expected_gpa)
                        {
                            break;
                        }

                        run_size += page_size;
                    }

                    this->remap_page_range(run_gpa, run_size, run_host_base, run_flags, run_had_mapping);
                    for (size_t run_offset = 0; run_offset < run_size; run_offset += page_size)
                    {
                        this->mapped_pages_.at(run_address + run_offset)->map_flags = run_flags;
                    }

                    offset += run_size;
                }
            }

            void remap_page_range(const uint64_t guest_physical_address, const size_t size, void* host_base, const uint32_t map_flags,
                                  const bool had_mapping)
            {
                if (had_mapping)
                {
                    WHP_CHECK_HR(WHvUnmapGpaRange(this->partition_, guest_physical_address, size));
                }

                if (map_flags != 0)
                {
                    WHP_CHECK_HR(WHvMapGpaRange(this->partition_, host_base, guest_physical_address, size,
                                                static_cast<WHV_MAP_GPA_RANGE_FLAGS>(map_flags)));
                }
            }

            // Assumes partition_mutex_ is held exclusively.
            void remap_hook_page(const uint64_t address)
            {
                const auto page_base = align_down_to_page(address);
                const auto entry = this->mapped_pages_.find(page_base);
                if (entry != this->mapped_pages_.end() && entry->second && entry->second->host_page != nullptr)
                {
                    this->remap_page(*entry->second);
                }
            }

            // Assumes partition_mutex_ is held exclusively.
            void remap_hook_pages(const uint64_t address, const uint64_t size)
            {
                if (size == 0)
                {
                    return;
                }

                // Coalesce the EPT remap over the whole range: each per-page unmap+map is a hypervisor
                // EPT flush, so flipping a large module section (thousands of pages) page-by-page stalls
                // for seconds. remap_pages merges contiguous runs into one WHvMapGpaRange, which cuts the
                // per-page cost by ~70x for the section-first-execution hooks that cover module code.
                const auto start = align_down_to_page(address);
                const auto end = align_up(address + size, page_size);
                this->remap_pages(start, static_cast<size_t>(end - start));
            }

            // Assumes partition_mutex_ is held exclusively.
            void increment_execution_hook_count(const uint64_t address, const uint64_t size)
            {
                if (size == 0)
                {
                    return;
                }

                const auto start = align_down_to_page(address);
                const auto end = align_up(address + size, page_size);
                for (auto page = start; page < end; page += page_size)
                {
                    const auto entry = this->mapped_pages_.find(page);
                    if (entry == this->mapped_pages_.end() || !entry->second)
                    {
                        continue;
                    }

                    ++entry->second->page_execution_hook_count;
                }
            }

            // Assumes partition_mutex_ is held exclusively.
            void decrement_execution_hook_count(const uint64_t address, const uint64_t size)
            {
                if (size == 0)
                {
                    return;
                }

                const auto start = align_down_to_page(address);
                const auto end = align_up(address + size, page_size);
                for (auto page = start; page < end; page += page_size)
                {
                    const auto entry = this->mapped_pages_.find(page);
                    if (entry == this->mapped_pages_.end() || !entry->second)
                    {
                        continue;
                    }

                    if (entry->second->page_execution_hook_count != 0)
                    {
                        --entry->second->page_execution_hook_count;
                    }
                }
            }

            // Assumes partition_mutex_ is held exclusively.
            void deactivate_execution_hook(emulator_hook* hook)
            {
                const auto execution_it = this->memory_execution_hooks_.find(hook);
                if (execution_it == this->memory_execution_hooks_.end())
                {
                    return;
                }

                auto& execution = execution_it->second;
                if (!execution.address)
                {
                    return;
                }

                if (execution.patched_breakpoint)
                {
                    this->uninstall_patched_execution_breakpoint(*execution.address);
                }
                else
                {
                    this->decrement_execution_hook_count(*execution.address, execution.size);
                    this->remap_hook_pages(*execution.address, execution.size);
                }
            }

            // Assumes partition_mutex_ is held exclusively.
            void try_apply_patched_execution_breakpoint(const uint64_t address, patched_execution_breakpoint& breakpoint)
            {
                const auto page_base = align_down_to_page(address);
                const auto entry = this->mapped_pages_.find(page_base);
                if (entry == this->mapped_pages_.end() || !entry->second || entry->second->host_page == nullptr)
                {
                    breakpoint.applied = false;
                    return;
                }

                const auto offset = static_cast<size_t>(address - page_base);
                auto* page = static_cast<std::byte*>(entry->second->host_page);
                if (breakpoint.applied && page[offset] == std::byte{0xCC})
                {
                    return;
                }

                breakpoint.original_byte = page[offset];
                page[offset] = std::byte{0xCC};
                breakpoint.applied = true;
            }

            // Assumes partition_mutex_ is held exclusively.
            void apply_patched_execution_breakpoints(const uint64_t page_base)
            {
                for (auto& [address, breakpoint] : this->patched_execution_breakpoints_)
                {
                    if (align_down_to_page(address) == page_base)
                    {
                        this->try_apply_patched_execution_breakpoint(address, breakpoint);
                    }
                }
            }

            // Assumes partition_mutex_ is held exclusively.
            void mark_patched_execution_breakpoints_unmapped(const uint64_t page_base)
            {
                for (auto& [address, breakpoint] : this->patched_execution_breakpoints_)
                {
                    if (align_down_to_page(address) == page_base)
                    {
                        breakpoint.original_byte.reset();
                        breakpoint.applied = false;
                    }
                }
            }

            // Assumes partition_mutex_ is held exclusively.
            void install_patched_execution_breakpoint(const uint64_t address)
            {
                auto existing = this->patched_execution_breakpoints_.find(address);
                if (existing != this->patched_execution_breakpoints_.end())
                {
                    ++existing->second.hook_count;
                    if (!existing->second.applied)
                    {
                        this->try_apply_patched_execution_breakpoint(address, existing->second);
                    }
                    return;
                }

                auto breakpoint = patched_execution_breakpoint{.hook_count = 1};
                this->try_apply_patched_execution_breakpoint(address, breakpoint);
                this->patched_execution_breakpoints_[address] = breakpoint;
            }

            // Assumes partition_mutex_ is held exclusively.
            void uninstall_patched_execution_breakpoint(const uint64_t address)
            {
                auto existing = this->patched_execution_breakpoints_.find(address);
                if (existing == this->patched_execution_breakpoints_.end())
                {
                    return;
                }

                if (existing->second.hook_count > 1)
                {
                    --existing->second.hook_count;
                    return;
                }

                if (existing->second.applied && existing->second.original_byte)
                {
                    auto original = *existing->second.original_byte;
                    (void)this->access_memory(address, &original, sizeof(original), true);
                }

                this->patched_execution_breakpoints_.erase(existing);
            }

            // Assumes partition_mutex_ is held exclusively.
            void set_patched_execution_breakpoint_state(const uint64_t address, const bool applied)
            {
                const auto existing = this->patched_execution_breakpoints_.find(address);
                if (existing == this->patched_execution_breakpoints_.end())
                {
                    return;
                }

                if (applied && !existing->second.original_byte)
                {
                    this->try_apply_patched_execution_breakpoint(address, existing->second);
                    return;
                }

                if (!existing->second.original_byte)
                {
                    return;
                }

                auto value = applied ? std::byte{0xCC} : *existing->second.original_byte;
                if (!this->access_memory(address, &value, sizeof(value), true))
                {
                    existing->second.applied = false;
                    existing->second.original_byte.reset();
                    return;
                }

                existing->second.applied = applied;
            }

            // Assumes partition_mutex_ is held exclusively.
            bool arm_execution_single_step(whp_vcpu& vcpu, const std::optional<uint64_t> page_base, const bool stop_after_step)
            {
                if (vcpu.pending_execution_step_)
                {
                    if (!page_base || vcpu.pending_execution_step_->page_base)
                    {
                        return false;
                    }

                    vcpu.pending_execution_step_->page_base = page_base;
                }
                else
                {
                    auto rflags = vcpu.get_register(WHvX64RegisterRflags);
                    pending_execution_step state{};
                    state.page_base = page_base;
                    state.had_trap_flag = (rflags.Reg64 & trap_flag_bit) != 0;
                    state.stop_after_step = stop_after_step;

                    rflags.Reg64 |= trap_flag_bit;
                    vcpu.set_register(WHvX64RegisterRflags, rflags);
                    vcpu.pending_execution_step_ = state;
                }

                if (page_base)
                {
                    const auto entry = this->mapped_pages_.find(*page_base);
                    if (entry != this->mapped_pages_.end() && entry->second)
                    {
                        this->remap_page(*entry->second);
                    }
                }

                return true;
            }

            // Assumes partition_mutex_ is held exclusively.
            bool arm_patched_breakpoint_single_step(whp_vcpu& vcpu, const uint64_t address, const bool stop_after_step)
            {
                if (!this->arm_execution_single_step(vcpu, std::nullopt, stop_after_step))
                {
                    return false;
                }

                vcpu.pending_execution_step_->patched_breakpoint = address;
                vcpu.deferred_patched_breakpoint_.reset();
                return true;
            }

            // Assumes partition_mutex_ is held exclusively.
            bool complete_execution_step(whp_vcpu& vcpu)
            {
                if (!vcpu.pending_execution_step_)
                {
                    return false;
                }

                const auto state = std::exchange(vcpu.pending_execution_step_, std::nullopt);

                auto rflags = vcpu.get_register(WHvX64RegisterRflags);
                if (state->had_trap_flag)
                {
                    rflags.Reg64 |= trap_flag_bit;
                }
                else
                {
                    rflags.Reg64 &= ~trap_flag_bit;
                }
                vcpu.set_register(WHvX64RegisterRflags, rflags);

                if (state->page_base)
                {
                    this->remap_hook_page(*state->page_base);
                }
                if (state->patched_breakpoint)
                {
                    this->set_patched_execution_breakpoint_state(*state->patched_breakpoint, true);
                }

                if (state->stop_after_step)
                {
                    vcpu.stop_requested_ = true;
                }

                return !state->had_trap_flag;
            }

            // Assumes partition_mutex_ is held.
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

            // Assumes partition_mutex_ is held.
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

            // Assumes partition_mutex_ is held exclusively.
            void grant_mmio_read_grace(const uint64_t page_base)
            {
                constexpr std::chrono::milliseconds mmio_read_grace_period_{4};
                this->mmio_read_grace_deadlines_[page_base] = std::chrono::steady_clock::now() + mmio_read_grace_period_;
            }

            // Assumes partition_mutex_ is held exclusively.
            void revoke_mmio_read_grace(const uint64_t page_base)
            {
                this->mmio_read_grace_deadlines_.erase(page_base);
            }

            void expire_mmio_read_grace_pages(whp_vcpu& vcpu)
            {
                if (vcpu.pending_mmio_step_.has_value())
                {
                    return;
                }

                {
                    std::shared_lock lock(this->partition_mutex_);
                    if (this->mmio_read_grace_deadlines_.empty())
                    {
                        return;
                    }
                }

                const auto now = std::chrono::steady_clock::now();

                std::unique_lock lock(this->partition_mutex_);
                for (auto it = this->mmio_read_grace_deadlines_.begin(); it != this->mmio_read_grace_deadlines_.end();)
                {
                    const auto page_base = it->first;
                    const auto deadline = it->second;

                    if (deadline > now)
                    {
                        ++it;
                        continue;
                    }

                    auto page_it = this->mapped_pages_.find(page_base);
                    if (page_it != this->mapped_pages_.end() && page_it->second)
                    {
                        if (this->find_mmio_region(page_base) != nullptr)
                        {
                            page_it->second->permissions = memory_permission::none;
                            this->remap_page(*page_it->second);
                        }
                    }

                    it = this->mmio_read_grace_deadlines_.erase(it);
                }
            }

            // Assumes partition_mutex_ is held exclusively.
            bool arm_mmio_single_step(whp_vcpu& vcpu, const uint64_t page_base, const bool is_write)
            {
                if (vcpu.pending_mmio_step_.has_value())
                {
                    return false;
                }

                auto rflags = vcpu.get_register(WHvX64RegisterRflags);
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
                vcpu.set_register(WHvX64RegisterRflags, rflags);
                vcpu.pending_mmio_step_ = std::move(state);
                return true;
            }

            // Called without the partition lock held; takes it internally so the MMIO write callback
            // can be invoked outside of it.
            bool complete_pending_mmio_step(whp_vcpu& vcpu)
            {
                if (!vcpu.pending_mmio_step_.has_value())
                {
                    return false;
                }

                const auto state = std::move(*vcpu.pending_mmio_step_);
                vcpu.pending_mmio_step_.reset();

                mmio_write_callback write_cb{};
                std::vector<mmio_write_chunk> write_chunks{};

                {
                    std::unique_lock lock(this->partition_mutex_);
                    this->revoke_mmio_read_grace(state.page_base);

                    const auto entry = this->mapped_pages_.find(state.page_base);
                    if (entry == this->mapped_pages_.end() || !entry->second)
                    {
                        throw std::runtime_error("MMIO page backing is missing");
                    }

                    if (state.is_write)
                    {
                        if (const auto* region = this->find_mmio_region(state.page_base))
                        {
                            if (entry->second->host_page == nullptr)
                            {
                                throw std::runtime_error("MMIO page backing is missing");
                            }

                            write_cb = region->write_cb;

                            const auto* current_page = static_cast<const std::byte*>(entry->second->host_page);
                            const auto region_offset = static_cast<size_t>(state.page_base - region->address);
                            const auto bytes_in_region = (std::min)(static_cast<size_t>(page_size), region->size - region_offset);

                            size_t index = 0;
                            while (index < bytes_in_region)
                            {
                                if (state.old_page[index] == current_page[index])
                                {
                                    ++index;
                                    continue;
                                }

                                const auto start = index;
                                while (index < bytes_in_region && state.old_page[index] != current_page[index])
                                {
                                    ++index;
                                }

                                write_chunks.push_back(mmio_write_chunk{
                                    .offset = region_offset + start,
                                    .data = std::vector<std::byte>(current_page + start, current_page + index),
                                });
                            }
                        }
                    }

                    entry->second->permissions = memory_permission::none;
                    this->remap_page(*entry->second);
                }

                for (const auto& chunk : write_chunks)
                {
                    write_cb(chunk.offset, chunk.data.data(), chunk.data.size());
                }

                auto rflags = vcpu.get_register(WHvX64RegisterRflags);

                if (state.had_trap_flag)
                {
                    rflags.Reg64 |= 0x100ull;
                }
                else
                {
                    rflags.Reg64 &= ~0x100ull;
                }

                vcpu.set_register(WHvX64RegisterRflags, rflags);

                return !state.allow_debug_delivery;
            }

            uint64_t* get_page_table_entries(const uint64_t page_gpa)
            {
                return this->page_table_views_.at(page_gpa);
            }

            // Assumes partition_mutex_ is held exclusively.
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

            // Assumes partition_mutex_ is held exclusively.
            void ensure_virtual_mapping(const uint64_t guest_address)
            {
                const auto page_base = align_down_to_page(guest_address);
                const auto mapped_page = this->mapped_pages_.find(page_base);
                if (mapped_page == this->mapped_pages_.end() || !mapped_page->second)
                {
                    throw std::runtime_error("WHP virtual mapping requested for missing page");
                }

                const auto pml4_index = static_cast<size_t>((page_base >> 39) & 0x1FF);
                const auto pdpt_index = static_cast<size_t>((page_base >> 30) & 0x1FF);
                const auto pd_index = static_cast<size_t>((page_base >> 21) & 0x1FF);
                const auto pt_index = static_cast<size_t>((page_base >> 12) & 0x1FF);

                const auto pdpt_gpa = this->ensure_child_table(this->pml4_gpa_, pml4_index);
                const auto pd_gpa = this->ensure_child_table(pdpt_gpa, pdpt_index);
                const auto pt_gpa = this->ensure_child_table(pd_gpa, pd_index);

                auto* const pt_entries = this->get_page_table_entries(pt_gpa);
                pt_entries[pt_index] = mapped_page->second->guest_physical_address | page_table_entry_present | page_table_entry_writable |
                                       page_table_entry_user;
            }

            // A fault can be spurious under multiple vCPUs: the backend has the page backed
            // (a peer mapped/committed it, or a protection/COW remap transiently cleared the
            // guest PTE) but this vCPU's page-table walk missed it. If the page is backed,
            // re-establish its mapping, flush this vCPU's TLB and retry. A per-vCPU (page,rip)
            // guard ensures a genuine violation (unbacked page, wrong permissions) is still
            // delivered rather than looping forever.
            // operation_known is false only for the unrecoverable-exit path, whose fault carries no
            // access type (we assume a read). There a backed+readable page could still be a genuine
            // write-to-read-only, so consecutive repairs are capped to avoid an endless retry loop.
            // For the well-typed paths (#PF, memory access) a backed page whose permissions allow the
            // faulting operation can only fault transiently (a peer's stale-translation re-clear), so
            // it is retried without a cap - the re-clears are finite and always resolve.
            bool try_repair_spurious_fault(whp_vcpu& vcpu, const uint64_t fault_address, const memory_operation operation,
                                           const bool operation_known)
            {
                constexpr uint32_t max_consecutive_repairs = 256;

                const auto page_base = align_down_to_page(fault_address);
                const auto rip = vcpu.read_instruction_pointer();
                const auto same_as_last = vcpu.last_repair_page_ == page_base && vcpu.last_repair_rip_ == rip;
                const auto guard_tripped = !operation_known && same_as_last && vcpu.last_repair_count_ >= max_consecutive_repairs;

                bool repaired = false;

                {
                    std::unique_lock lock(this->partition_mutex_);
                    const auto it = this->mapped_pages_.find(page_base);
                    const auto backed = it != this->mapped_pages_.end() && it->second && it->second->host_page != nullptr;

                    // Only a page that is backed AND whose intended permissions allow the faulting
                    // operation can be a spurious (stale-translation) fault. Guard and no-access pages
                    // map with permission 'none', so they still reach the violation hook, as do genuine
                    // protection violations (write to read-only, etc.).
                    const auto perms = backed ? it->second->permissions : memory_permission::none;
                    const auto permitted = (perms & operation) == operation;

                    if (!guard_tripped && backed && permitted)
                    {
                        this->ensure_virtual_mapping(page_base);
                        this->remap_page(*it->second);
                        repaired = true;
                    }
                }

                if (guard_tripped)
                {
                    // Repaired this exact fault too many times in a row without progress: give up and
                    // deliver it (guards the unrecoverable-exit read-assumption from a write-to-RO loop).
                    vcpu.last_repair_page_ = unmapped_guest_page;
                    vcpu.last_repair_count_ = 0;
                    return false;
                }

                if (!repaired)
                {
                    return false;
                }

                vcpu.last_repair_count_ = same_as_last ? vcpu.last_repair_count_ + 1 : 1;
                vcpu.last_repair_page_ = page_base;
                vcpu.last_repair_rip_ = rip;
                vcpu.set_register(WHvX64RegisterCr3, vcpu.get_register(WHvX64RegisterCr3));
                return true;
            }

            // Assumes partition_mutex_ is held exclusively.
            bool clear_virtual_mapping(const uint64_t guest_address)
            {
                const auto page_base = align_down_to_page(guest_address);
                const auto pml4_index = static_cast<size_t>((page_base >> 39) & 0x1FF);
                const auto pdpt_index = static_cast<size_t>((page_base >> 30) & 0x1FF);
                const auto pd_index = static_cast<size_t>((page_base >> 21) & 0x1FF);
                const auto pt_index = static_cast<size_t>((page_base >> 12) & 0x1FF);

                auto* pml4_entries = this->get_page_table_entries(this->pml4_gpa_);
                const auto pml4_entry = pml4_entries[pml4_index];
                if ((pml4_entry & page_table_entry_present) == 0)
                {
                    return false;
                }

                auto* pdpt_entries = this->get_page_table_entries(pml4_entry & page_table_entry_address_mask);
                const auto pdpt_entry = pdpt_entries[pdpt_index];
                if ((pdpt_entry & page_table_entry_present) == 0)
                {
                    return false;
                }

                auto* pd_entries = this->get_page_table_entries(pdpt_entry & page_table_entry_address_mask);
                const auto pd_entry = pd_entries[pd_index];
                if ((pd_entry & page_table_entry_present) == 0)
                {
                    return false;
                }

                auto* pt_entries = this->get_page_table_entries(pd_entry & page_table_entry_address_mask);
                auto& pt_entry = pt_entries[pt_index];
                if ((pt_entry & page_table_entry_present) == 0)
                {
                    return false;
                }

                pt_entry = 0;
                return true;
            }

            // Assumes partition_mutex_ is held exclusively. Every vCPU shares the same page
            // tables; each one flushes its own cached translations by reloading CR3 at the
            // top of its run loop. Registers of a potentially running vCPU cannot be touched
            // from here, so the flush is requested via a flag plus a cancel: a running vCPU
            // exits and re-enters through the loop top, an idle one applies the flag before
            // its next run.
            void flush_virtual_address_mappings()
            {
                for (const auto& vcpu : this->vcpus_)
                {
                    vcpu->pending_tlb_flush_ = true;
                    (void)WHvCancelRunVirtualProcessor(this->partition_, vcpu->vp_index_, 0);
                }
            }

            // Assumes partition_mutex_ is held (shared is sufficient for reads, exclusive for writes
            // only because callers that write also update breakpoint bookkeeping).
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

            // Assumes partition_mutex_ is held (shared).
            void overlay_patched_breakpoints(const uint64_t address, void* data, const size_t size) const
            {
                auto current_address = address;
                auto* cursor = static_cast<std::byte*>(data);
                auto remaining = size;

                while (remaining > 0)
                {
                    const auto page_base = align_down_to_page(current_address);
                    const auto offset = static_cast<size_t>(current_address - page_base);
                    const auto chunk = (std::min)(remaining, static_cast<size_t>(page_size - offset));

                    for (const auto& [breakpoint_address, breakpoint] : this->patched_execution_breakpoints_)
                    {
                        if (!breakpoint.original_byte || breakpoint_address < current_address ||
                            breakpoint_address >= current_address + chunk)
                        {
                            continue;
                        }

                        const auto breakpoint_offset = static_cast<size_t>(breakpoint_address - current_address);
                        cursor[breakpoint_offset] = *breakpoint.original_byte;
                    }

                    current_address += chunk;
                    cursor += chunk;
                    remaining -= chunk;
                }
            }

            // Assumes partition_mutex_ is held exclusively.
            void overlay_patched_breakpoints(const uint64_t address, const void* data, const size_t size)
            {
                auto current_address = address;
                const auto* cursor = static_cast<const std::byte*>(data);
                auto remaining = size;

                while (remaining > 0)
                {
                    const auto page_base = align_down_to_page(current_address);
                    const auto entry = this->mapped_pages_.find(page_base);
                    if (entry == this->mapped_pages_.end() || !entry->second || entry->second->host_page == nullptr)
                    {
                        return;
                    }

                    const auto offset = static_cast<size_t>(current_address - page_base);
                    const auto chunk = (std::min)(remaining, static_cast<size_t>(page_size - offset));
                    auto* page_ptr = static_cast<std::byte*>(entry->second->host_page) + offset;

                    for (auto& [breakpoint_address, breakpoint] : this->patched_execution_breakpoints_)
                    {
                        if (breakpoint_address < current_address || breakpoint_address >= current_address + chunk)
                        {
                            continue;
                        }

                        const auto breakpoint_offset = static_cast<size_t>(breakpoint_address - current_address);
                        breakpoint.original_byte = cursor[breakpoint_offset];
                        page_ptr[breakpoint_offset] = std::byte{0xCC};
                        breakpoint.applied = true;
                    }

                    current_address += chunk;
                    cursor += chunk;
                    remaining -= chunk;
                }
            }

            // Assumes partition_mutex_ is held exclusively.
            emulator_hook* make_hook()
            {
                return reinterpret_cast<emulator_hook*>(this->next_hook_id_++);
            }

            interrupt_hook_callback copy_first_interrupt_hook() const
            {
                std::shared_lock lock(this->partition_mutex_);
                if (this->interrupt_hooks_.empty())
                {
                    return {};
                }

                return this->interrupt_hooks_.begin()->second;
            }

            std::vector<instruction_hook_callback> copy_instruction_hooks(const x86_hookable_instructions type) const
            {
                std::vector<instruction_hook_callback> callbacks{};

                std::shared_lock lock(this->partition_mutex_);
                for (const auto& [_, hook] : this->instruction_hooks_)
                {
                    if (hook.type == type)
                    {
                        callbacks.push_back(hook.callback);
                    }
                }

                return callbacks;
            }

            std::vector<memory_violation_hook_callback> copy_memory_violation_hooks() const
            {
                std::vector<memory_violation_hook_callback> callbacks{};

                std::shared_lock lock(this->partition_mutex_);
                callbacks.reserve(this->memory_violation_hooks_.size());
                for (const auto& [_, hook] : this->memory_violation_hooks_)
                {
                    callbacks.push_back(hook);
                }

                return callbacks;
            }

            void run(whp_vcpu& vcpu, const size_t count)
            {
                if (count != 0 && count != 1)
                {
                    throw std::runtime_error("WHP backend does not support exact instruction counts yet");
                }

                vcpu.stop_requested_ = false;

                {
                    std::unique_lock lock(this->partition_mutex_);

                    bool armed_single_step = false;
                    if (vcpu.deferred_patched_breakpoint_)
                    {
                        if (vcpu.read_instruction_pointer() == *vcpu.deferred_patched_breakpoint_)
                        {
                            if (!this->arm_patched_breakpoint_single_step(vcpu, *vcpu.deferred_patched_breakpoint_, count == 1))
                            {
                                throw std::runtime_error("Nested WHP execution single-step state is not supported");
                            }

                            armed_single_step = true;
                        }
                        else
                        {
                            this->set_patched_execution_breakpoint_state(*vcpu.deferred_patched_breakpoint_, true);
                            vcpu.deferred_patched_breakpoint_.reset();
                        }
                    }

                    if (!armed_single_step && vcpu.deferred_execution_page_)
                    {
                        if (!this->arm_execution_single_step(vcpu, *vcpu.deferred_execution_page_, count == 1))
                        {
                            throw std::runtime_error("Nested WHP execution single-step state is not supported");
                        }

                        vcpu.deferred_execution_page_.reset();
                        armed_single_step = true;
                    }

                    if (!armed_single_step && count == 1 && !this->arm_execution_single_step(vcpu, std::nullopt, true))
                    {
                        throw std::runtime_error("Nested WHP execution single-step state is not supported");
                    }
                }

                while (!vcpu.stop_requested_)
                {
                    if (vcpu.pending_tlb_flush_.exchange(false))
                    {
                        // Reload CR3 to flush this vCPU's cached translations. Safe here:
                        // this vCPU is not inside WHvRunVirtualProcessor.
                        vcpu.set_register(WHvX64RegisterCr3, vcpu.get_register(WHvX64RegisterCr3));
                    }

                    this->expire_mmio_read_grace_pages(vcpu);
                    WHV_RUN_VP_EXIT_CONTEXT exit_context{};
                    const auto start_rip = vcpu.read_instruction_pointer();
                    vcpu.run_active_ = true;
                    const auto run_hr = WHvRunVirtualProcessor(this->partition_, vcpu.vp_index_, &exit_context, sizeof(exit_context));
                    vcpu.run_active_ = false;
                    if (FAILED(run_hr))
                    {
                        throw_hr(run_hr, "WHvRunVirtualProcessor");
                    }
                    switch (exit_context.ExitReason)
                    {
                    case WHvRunVpExitReasonCanceled: {
                        if (vcpu.stop_requested_)
                        {
                            return;
                        }

                        // A cancel without a stop request comes from flush_virtual_address_mappings
                        // (another vCPU changed the shared page tables) - never from a real stop, which
                        // always sets stop_requested_ first. Re-enter so the pending TLB flush is applied
                        // at the loop top; do not treat it as a stop regardless of whether rip advanced.
                        (void)start_rip;
                        continue;
                    }
                    case WHvRunVpExitReasonX64Halt:
                        if (this->syscall_hook_ && exit_context.VpContext.Rip == (this->syscall_hook_page_ + 1))
                        {
                            if (this->handle_syscall_halt(vcpu))
                            {
                                continue;
                            }
                        }
                        else
                        {
                            return;
                        }

                        if (vcpu.stop_requested_)
                        {
                            return;
                        }

                        if (this->syscall_hook_)
                        {
                            continue;
                        }
                        return;
                    case WHvRunVpExitReasonMemoryAccess:
                        if (this->handle_memory_access(vcpu, exit_context.MemoryAccess))
                        {
                            continue;
                        }
                        return;
                    case WHvRunVpExitReasonException:
                        if (this->handle_exception(vcpu, exit_context))
                        {
                            vcpu.clear_pending_exception_state();
                            continue;
                        }
                        return;
                    case WHvRunVpExitReasonX64Cpuid:
                        if (this->handle_instruction_exit(vcpu, x86_hookable_instructions::cpuid, exit_context, 2))
                        {
                            continue;
                        }
                        throw std::runtime_error("Unhandled CPUID exit");
                    case WHvRunVpExitReasonX64Rdtsc:
                        if (this->handle_instruction_exit(vcpu,
                                                          exit_context.ReadTsc.RdtscInfo.IsRdtscp ? x86_hookable_instructions::rdtscp
                                                                                                  : x86_hookable_instructions::rdtsc,
                                                          exit_context, 2))
                        {
                            continue;
                        }
                        throw std::runtime_error("Unhandled RDTSC/RDTSCP exit");
                    case WHvRunVpExitReasonUnrecoverableException:
                        if (this->handle_unrecoverable_exception(vcpu))
                        {
                            vcpu.clear_pending_exception_state();
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

            static void apply_default_instruction_exit(whp_vcpu& vcpu, const x86_hookable_instructions type,
                                                       const WHV_RUN_VP_EXIT_CONTEXT& exit_context)
            {
                switch (type)
                {
                case x86_hookable_instructions::cpuid: {
                    const auto& cpuid = exit_context.CpuidAccess;
                    vcpu.reg(x86_register::rax, static_cast<uint64_t>(cpuid.DefaultResultRax));
                    vcpu.reg(x86_register::rbx, static_cast<uint64_t>(cpuid.DefaultResultRbx));
                    vcpu.reg(x86_register::rcx, static_cast<uint64_t>(cpuid.DefaultResultRcx));
                    vcpu.reg(x86_register::rdx, static_cast<uint64_t>(cpuid.DefaultResultRdx));
                    break;
                }
                case x86_hookable_instructions::rdtsc:
                case x86_hookable_instructions::rdtscp: {
                    const auto tsc = exit_context.ReadTsc.Tsc;
                    vcpu.reg(x86_register::rax, static_cast<uint32_t>(tsc & 0xFFFFFFFFull));
                    vcpu.reg(x86_register::rdx, static_cast<uint32_t>(tsc >> 32));

                    if (type == x86_hookable_instructions::rdtscp)
                    {
                        vcpu.reg(x86_register::rcx, static_cast<uint32_t>(exit_context.ReadTsc.TscAux & 0xFFFFFFFFull));
                    }

                    break;
                }
                default:
                    break;
                }
            }

            bool handle_instruction_exit(whp_vcpu& vcpu, const x86_hookable_instructions type, const WHV_RUN_VP_EXIT_CONTEXT& exit_context,
                                         const uint64_t instruction_size)
            {
                const auto callbacks = this->copy_instruction_hooks(type);
                const bool handled = !callbacks.empty();
                bool skip = false;

                for (const auto& callback : callbacks)
                {
                    if (callback(vcpu, 0) == instruction_hook_continuation::skip_instruction)
                    {
                        skip = true;
                    }
                }

                if (!handled || !skip)
                {
                    apply_default_instruction_exit(vcpu, type, exit_context);
                }

                vcpu.advance_rip(instruction_size);

                return handled || type == x86_hookable_instructions::cpuid || type == x86_hookable_instructions::rdtsc ||
                       type == x86_hookable_instructions::rdtscp;
            }

            bool handle_unrecoverable_exception(whp_vcpu& vcpu)
            {
                const auto rip = vcpu.read_instruction_pointer();

                std::array<std::byte, 2> opcode{};
                bool opcode_read = false;
                {
                    std::shared_lock lock(this->partition_mutex_);
                    opcode_read = this->access_memory(rip, opcode.data(), opcode.size(), false);
                }

                if (opcode_read && opcode[0] == std::byte{0xCD})
                {
                    if (const auto hook = this->copy_first_interrupt_hook())
                    {
                        hook(vcpu, std::to_integer<uint8_t>(opcode[1]));

                        if (vcpu.read_instruction_pointer() == rip)
                        {
                            vcpu.advance_rip(2);
                        }

                        return true;
                    }
                }
                else if (opcode_read && opcode[0] == std::byte{0x0F} && opcode[1] == std::byte{0x0B})
                {
                    bool skip = false;
                    bool consumed = false;

                    for (const auto& callback : this->copy_instruction_hooks(x86_hookable_instructions::invalid))
                    {
                        if (callback(vcpu, 0) == instruction_hook_continuation::skip_instruction)
                        {
                            skip = true;
                            consumed = true;
                        }
                    }

                    if (skip && vcpu.read_instruction_pointer() == rip)
                    {
                        vcpu.advance_rip(2);
                    }

                    if (consumed)
                    {
                        return true;
                    }

                    if (const auto hook = this->copy_first_interrupt_hook())
                    {
                        hook(vcpu, 6);
                        return true;
                    }

                    return false;
                }

                if (vcpu.pending_mmio_step_.has_value())
                {
                    const auto swallow = this->complete_pending_mmio_step(vcpu);
                    if (swallow)
                    {
                        return true;
                    }
                }

                const auto rflags = vcpu.reg<uint64_t>(x86_register::rflags);
                if ((rflags & 0x100ull) != 0)
                {
                    if (const auto hook = this->copy_first_interrupt_hook())
                    {
                        hook(vcpu, 1);
                        return true;
                    }
                }

                const auto fault_address = vcpu.reg<uint64_t>(x86_register::cr2);

                if (fault_address != 0)
                {
                    // Guest page faults escalate here (the guest IDT cannot dispatch them). Under
                    // multiple vCPUs the faulting page may in fact be backed with permissions that
                    // allow the access - a peer mapped/committed/reprotected it after this vCPU cached
                    // a stale translation. Repair and retry rather than delivering a spurious access
                    // violation. The exit carries no access type, so assume a read; a genuine
                    // write-to-read-only self-corrects via the per-vCPU retry guard.
                    if (this->try_repair_spurious_fault(vcpu, fault_address, memory_operation::read, false))
                    {
                        return true;
                    }

                    for (const auto& hook : this->copy_memory_violation_hooks())
                    {
                        const auto result = hook(vcpu, fault_address, 1, memory_operation::read, memory_violation_type::unmapped);
                        if (result == memory_violation_continuation::resume || result == memory_violation_continuation::restart)
                        {
                            return true;
                        }
                    }
                }

                if (const auto hook = this->copy_first_interrupt_hook())
                {
                    hook(vcpu, 14);
                    return true;
                }

                return false;
            }

            bool handle_execution_hook(whp_vcpu& vcpu, const uint64_t address)
            {
                const auto page_base = align_down_to_page(address);
                std::vector<memory_execution_hook_callback> callbacks{};

                {
                    std::shared_lock lock(this->partition_mutex_);

                    const auto entry = this->mapped_pages_.find(page_base);
                    if (entry == this->mapped_pages_.end() || !entry->second || !is_executable(entry->second->permissions) ||
                        entry->second->page_execution_hook_count == 0)
                    {
                        return false;
                    }

                    for (const auto& [_, hook] : this->memory_execution_hooks_)
                    {
                        if (hook.address && !hook.patched_breakpoint && hook.size != 0 &&
                            is_within_start_and_length(address, *hook.address, hook.size))
                        {
                            callbacks.push_back(hook.callback);
                        }
                    }
                }

                for (const auto& callback : callbacks)
                {
                    callback(vcpu, address);
                }

                if (vcpu.stop_requested_)
                {
                    vcpu.deferred_execution_page_ = page_base;
                    return true;
                }

                std::unique_lock lock(this->partition_mutex_);

                const auto current_entry = this->mapped_pages_.find(page_base);
                if (current_entry == this->mapped_pages_.end() || !current_entry->second ||
                    !is_executable(current_entry->second->permissions) || current_entry->second->page_execution_hook_count == 0)
                {
                    return true;
                }

                if (!this->arm_execution_single_step(vcpu, page_base, false))
                {
                    throw std::runtime_error("Nested WHP execution single-step state is not supported");
                }

                return true;
            }

            bool handle_memory_access(whp_vcpu& vcpu, const WHV_MEMORY_ACCESS_CONTEXT& memory_access)
            {
                const auto access_type = static_cast<WHV_MEMORY_ACCESS_TYPE>(memory_access.AccessInfo.AccessType);

                auto resolved_address = memory_access.Gva;
                if (!memory_access.AccessInfo.GvaValid)
                {
                    std::shared_lock lock(this->partition_mutex_);
                    resolved_address = this->translate_guest_physical_address(memory_access.Gpa).value_or(memory_access.Gpa);
                }

                if (access_type == WHvMemoryAccessExecute && this->handle_execution_hook(vcpu, resolved_address))
                {
                    return true;
                }

                const auto mmio_address = resolved_address;
                const auto operation = map_memory_operation(access_type);

                bool found_region = false;
                mmio_read_callback region_read_cb{};
                uint64_t region_address = 0;
                size_t region_size = 0;

                {
                    std::shared_lock lock(this->partition_mutex_);
                    if (const auto* region = this->find_mmio_region(mmio_address))
                    {
                        found_region = true;
                        region_read_cb = region->read_cb;
                        region_address = region->address;
                        region_size = region->size;
                    }
                }

                if (found_region)
                {
                    const auto page_base = align_down_to_page(mmio_address);
                    const auto is_write = operation == memory_operation::write;

                    // Refresh the page content through the region callback outside the lock, then
                    // publish it into the backing page under it.
                    std::vector<std::byte> refreshed_page(page_size);
                    const auto region_offset = static_cast<size_t>(page_base - region_address);
                    const auto bytes_to_read = (std::min)(static_cast<size_t>(page_size), region_size - region_offset);
                    region_read_cb(region_offset, refreshed_page.data(), bytes_to_read);

                    std::unique_lock lock(this->partition_mutex_);

                    auto page_it = this->mapped_pages_.find(page_base);
                    if (page_it == this->mapped_pages_.end() || !page_it->second || page_it->second->host_page == nullptr)
                    {
                        throw std::runtime_error("MMIO page backing is missing");
                    }

                    std::memcpy(page_it->second->host_page, refreshed_page.data(), page_size);

                    if (is_write)
                    {
                        this->revoke_mmio_read_grace(page_base);
                        page_it->second->permissions = memory_permission::read_write;
                    }
                    else
                    {
                        page_it->second->permissions = memory_permission::read;
                    }

                    this->remap_page(*page_it->second);

                    if (!is_write)
                    {
                        this->grant_mmio_read_grace(page_base);
                        return true;
                    }

                    if (!this->arm_mmio_single_step(vcpu, page_base, is_write))
                    {
                        throw std::runtime_error("Nested WHP MMIO single-step state is not supported");
                    }

                    return true;
                }

                // Spurious fault on an actually-backed page whose mapping is momentarily stale for
                // this vCPU (peer map/commit/reprotect): repair and retry. Guard/no-access pages
                // (permission 'none') and genuine violations still reach the hook.
                if (this->try_repair_spurious_fault(vcpu, resolved_address, operation, true))
                {
                    return true;
                }

                const auto violation_hooks = this->copy_memory_violation_hooks();
                if (violation_hooks.empty())
                {
                    throw std::runtime_error("Unhandled WHP memory access violation");
                }

                const auto type =
                    memory_access.AccessInfo.GpaUnmapped ? memory_violation_type::unmapped : memory_violation_type::protection;

                for (const auto& hook : violation_hooks)
                {
                    const auto result = hook(vcpu, mmio_address, 1, operation, type);
                    if (result == memory_violation_continuation::resume || result == memory_violation_continuation::restart)
                    {
                        return true;
                    }
                }

                return false;
            }

            bool handle_patched_execution_breakpoint(whp_vcpu& vcpu, const uint64_t address)
            {
                std::vector<memory_execution_hook_callback> callbacks{};

                {
                    std::unique_lock lock(this->partition_mutex_);

                    if (!this->patched_execution_breakpoints_.contains(address))
                    {
                        return false;
                    }

                    auto rip_value = vcpu.get_register(WHvX64RegisterRip);
                    rip_value.Reg64 = address;
                    vcpu.set_register(WHvX64RegisterRip, rip_value);
                    this->set_patched_execution_breakpoint_state(address, false);

                    for (const auto& [_, hook] : this->memory_execution_hooks_)
                    {
                        if (hook.address && hook.patched_breakpoint && *hook.address == address)
                        {
                            callbacks.push_back(hook.callback);
                        }
                    }
                }

                for (const auto& callback : callbacks)
                {
                    callback(vcpu, address);
                }

                vcpu.deferred_patched_breakpoint_ = address;
                if (!vcpu.stop_requested_)
                {
                    std::unique_lock lock(this->partition_mutex_);
                    if (!this->arm_patched_breakpoint_single_step(vcpu, address, false))
                    {
                        throw std::runtime_error("Nested WHP execution single-step state is not supported");
                    }
                }

                return true;
            }

            bool handle_exception(whp_vcpu& vcpu, const WHV_RUN_VP_EXIT_CONTEXT& exit_context)
            {
                const auto& exception = exit_context.VpException;

                if (exception.ExceptionType == WHvX64ExceptionTypePageFault)
                {
                    const auto fault_address = exception.ExceptionParameter;
                    const bool is_write = (exception.ErrorCode & 0x2u) != 0;
                    const bool is_present = (exception.ErrorCode & 0x1u) != 0;
                    const auto operation = is_write ? memory_operation::write : memory_operation::read;
                    const auto type = is_present ? memory_violation_type::protection : memory_violation_type::unmapped;

                    // Under multiple vCPUs a peer may have mapped/committed/reprotected this page
                    // after this vCPU cached a stale translation, producing a spurious fault on an
                    // actually-backed page whose permissions allow the access. Repair and retry rather
                    // than delivering a bogus access violation. Guard/no-access pages and genuine
                    // violations (permission mismatch) still reach the violation hook.
                    if (this->try_repair_spurious_fault(vcpu, fault_address, operation, true))
                    {
                        return true;
                    }

                    for (const auto& hook : this->copy_memory_violation_hooks())
                    {
                        const auto result = hook(vcpu, fault_address, 1, operation, type);
                        if (result == memory_violation_continuation::resume || result == memory_violation_continuation::restart)
                        {
                            return true;
                        }
                    }

                    if (const auto hook = this->copy_first_interrupt_hook())
                    {
                        hook(vcpu, 14);
                        return true;
                    }

                    return false;
                }

                if (exception.ExceptionType == WHvX64ExceptionTypeDebugTrapOrFault)
                {
                    {
                        std::unique_lock lock(this->partition_mutex_);
                        if (this->complete_execution_step(vcpu))
                        {
                            return true;
                        }
                    }

                    if (vcpu.pending_mmio_step_.has_value())
                    {
                        const auto swallow = this->complete_pending_mmio_step(vcpu);
                        if (swallow)
                        {
                            return true;
                        }
                    }

                    // Not one of sogen's internal single-steps: a guest-armed debug breakpoint (DR0-3) or a
                    // guest trap-flag single-step. Fall through to the generic interrupt delivery below, which
                    // forwards it to the guest as #DB (vector 1 == WHvX64ExceptionTypeDebugTrapOrFault).
                }

                if (exception.ExceptionType == WHvX64ExceptionTypeBreakpointTrap)
                {
                    const auto rip = exit_context.VpContext.Rip;
                    if (this->handle_patched_execution_breakpoint(vcpu, rip - 1) || this->handle_patched_execution_breakpoint(vcpu, rip))
                    {
                        return true;
                    }

                    if (this->syscall_hook_ != nullptr)
                    {
                        if (rip == this->syscall_hook_page_ || rip == (this->syscall_hook_page_ + 1))
                        {
                            return this->handle_syscall_halt(vcpu);
                        }
                    }
                }

                if (exception.ExceptionType == WHvX64ExceptionTypeInvalidOpcodeFault)
                {
                    const auto rip = vcpu.read_instruction_pointer();
                    bool consumed = false;

                    for (const auto& callback : this->copy_instruction_hooks(x86_hookable_instructions::invalid))
                    {
                        if (callback(vcpu, 0) == instruction_hook_continuation::skip_instruction)
                        {
                            if (vcpu.read_instruction_pointer() == rip)
                            {
                                vcpu.advance_rip(exception.InstructionByteCount);
                            }
                            consumed = true;
                        }
                    }

                    if (consumed)
                    {
                        return true;
                    }
                }

                if (exception.ExceptionType == WHvX64ExceptionTypeSimdFloatingPointFault ||
                    exception.ExceptionType == WHvX64ExceptionTypeFloatingPointErrorFault)
                {
                    // With no guest IDT, an unhandled FP exception faults at IDT[vec] (e.g. 0x130 for #XM). The
                    // guest only reaches here with its FP exception masks cleared (a corrupted MXCSR/x87 control
                    // word); restore the masked Windows defaults and re-run the instruction so it completes as it
                    // would natively.
                    const auto mxcsr = vcpu.reg<uint32_t>(x86_register::mxcsr);
                    vcpu.reg(x86_register::mxcsr, (mxcsr | 0x1F80u) & ~0x3Fu);
                    const auto fpcw = vcpu.reg<uint16_t>(x86_register::fpcw);
                    vcpu.reg(x86_register::fpcw, static_cast<uint16_t>(fpcw | 0x3Fu));
                    // Also clear the x87 status-word exception flags / summary (ES) bit, or an FWAIT or following
                    // x87 op would re-raise #MF on the rerun.
                    const auto fpsw = vcpu.reg<uint16_t>(x86_register::fpsw);
                    vcpu.reg(x86_register::fpsw, static_cast<uint16_t>(fpsw & ~0x80FFu));
                    return true;
                }

                if (const auto hook = this->copy_first_interrupt_hook())
                {
                    hook(vcpu, exception.ExceptionType);
                    return true;
                }

                return false;
            }
        };

        void whp_vcpu::start(const size_t count)
        {
            this->emulator_.run(*this, count);
        }

        memory_interface& whp_vcpu::memory()
        {
            return this->emulator_;
        }

        const memory_interface& whp_vcpu::memory() const
        {
            return this->emulator_;
        }
    }

    std::unique_ptr<x86_64_emulator> create_x86_64_emulator(const size_t vcpu_count)
    {
        if (vcpu_count < 1 || vcpu_count > maximum_vcpu_count)
        {
            throw std::invalid_argument("WHP vCPU count must be between 1 and " + std::to_string(maximum_vcpu_count));
        }

        return std::make_unique<whp_x86_64_emulator>(vcpu_count);
    }
} // namespace sogen::whp
