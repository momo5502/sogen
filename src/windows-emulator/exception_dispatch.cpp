#include "std_include.hpp"
#include "exception_dispatch.hpp"
#include "process_context.hpp"
#include "cpu_context.hpp"
#include "windows_emulator.hpp"

#include "segment_utils.hpp"
#include "wow64_heaven_gate.hpp"

namespace sogen
{

    namespace
    {
        using exception_record = EMU_EXCEPTION_RECORD<EmulatorTraits<Emu64>>;
        using exception_record_map = std::unordered_map<const exception_record*, emulator_object<exception_record>>;

        emulator_object<exception_record> save_exception_record(emulator_allocator& allocator, const exception_record& record,
                                                                exception_record_map& record_mapping)
        {
            const auto record_obj = allocator.reserve<exception_record>();
            record_obj.write(record);

            if (record.ExceptionRecord)
            {
                record_mapping.emplace(&record, record_obj);

                emulator_object<exception_record> nested_record_obj{allocator.get_memory()};
                const auto nested_record = record_mapping.find(reinterpret_cast<exception_record*>(record.ExceptionRecord));

                if (nested_record != record_mapping.end())
                {
                    nested_record_obj = nested_record->second;
                }
                else
                {
                    nested_record_obj =
                        save_exception_record(allocator, *reinterpret_cast<exception_record*>(record.ExceptionRecord), record_mapping);
                }

                record_obj.access([&](exception_record& r) {
                    r.ExceptionRecord = nested_record_obj.value(); //
                });
            }

            return record_obj;
        }

        emulator_object<exception_record> save_exception_record(emulator_allocator& allocator, const exception_record& record)
        {
            exception_record_map record_mapping{};
            return save_exception_record(allocator, record, record_mapping);
        }

        uint32_t map_violation_operation_to_parameter(const memory_operation operation)
        {
            switch (operation)
            {
            default:
            case memory_operation::read:
                return 0;
            case memory_operation::write:
                return 1;
            case memory_operation::exec:
                return 8;
            }
        }

        size_t calculate_exception_record_size(const exception_record& record)
        {
            std::unordered_set<const exception_record*> records{};
            size_t total_size = 0;

            const exception_record* current_record = &record;
            while (current_record)
            {
                if (!records.insert(current_record).second)
                {
                    break;
                }

                total_size += sizeof(*current_record);
                current_record = reinterpret_cast<exception_record*>(record.ExceptionRecord);
            }

            return total_size;
        }

        void dispatch_exception_pointers(x86_64_cpu& emu, const emulator_thread& thread, const uint64_t dispatcher,
                                         const EMU_EXCEPTION_POINTERS<EmulatorTraits<Emu64>> pointers)
        {
            constexpr auto mach_frame_size = 0x40;
            constexpr auto context_record_size = 0x4F0;
            const auto exception_record_size =
                calculate_exception_record_size(*reinterpret_cast<exception_record*>(pointers.ExceptionRecord));
            const auto combined_size = align_up(exception_record_size + context_record_size, 0x10);

            assert(combined_size == 0x590);

            const auto allocation_size = combined_size + mach_frame_size;

            const auto initial_sp = emu.reg(x86_register::rsp);
            const auto new_sp = align_down(initial_sp - allocation_size, 0x100);

            const auto total_size = initial_sp - new_sp;
            assert(total_size >= allocation_size);

            std::vector<uint8_t> zero_memory{};
            zero_memory.resize(static_cast<size_t>(total_size), 0);

            thread.grow_stack_for_frame(new_sp, initial_sp);
            emu.write_memory(new_sp, zero_memory.data(), zero_memory.size());

            const emulator_object<CONTEXT64> context_record_obj{emu, new_sp};
            context_record_obj.write(*reinterpret_cast<CONTEXT64*>(pointers.ContextRecord));

            emulator_allocator allocator{emu, new_sp + context_record_size, exception_record_size};
            const auto exception_record_obj =
                save_exception_record(allocator, *reinterpret_cast<exception_record*>(pointers.ExceptionRecord));

            if (exception_record_obj.value() != allocator.get_base())
            {
                throw std::runtime_error("Bad exception record position on stack");
            }

            const emulator_object<MACHINE_FRAME> machine_frame_obj{emu, new_sp + combined_size};
            machine_frame_obj.access([&](MACHINE_FRAME& frame) {
                const auto& record = *reinterpret_cast<CONTEXT64*>(pointers.ContextRecord);
                frame.Rip = record.Rip;
                frame.Rsp = record.Rsp;
                frame.SegSs = record.SegSs;
                frame.SegCs = record.SegCs;
                frame.EFlags = record.EFlags;
            });

            const auto cs_selector = emu.reg<uint16_t>(x86_register::cs);
            const auto bitness = segment_utils::get_segment_bitness(emu, cs_selector);

            if (!bitness || *bitness != segment_utils::segment_bitness::bit32)
            {
                emu.reg(x86_register::rsp, new_sp);
                emu.reg(x86_register::rip, dispatcher);
                return;
            }

            emu.reg(x86_register::rax, dispatcher);
            emu.reg(x86_register::rbx, new_sp);
            emu.reg(x86_register::rcx, static_cast<uint64_t>(wow64::heaven_gate::kUserCodeSelector));
            emu.reg(x86_register::rdx, static_cast<uint64_t>(wow64::heaven_gate::kUserStackSelector));
            emu.reg(x86_register::rsp, wow64::heaven_gate::kStackTop);
            emu.reg(x86_register::rip, wow64::heaven_gate::kCodeBase);
        }

        // ARM64 KiUserExceptionDispatcher finds the CONTEXT at SP and the EXCEPTION_RECORD at SP + 0x3B0,
        // as its RtlDispatchException(record, context) call sequence shows.
        constexpr uint64_t arm64_exception_record_offset = 0x3B0;

        void dispatch_arm64_exception(windows_emulator& win_emu, arm64_cpu& cpu, const emulator_thread& thread,
                                      const exception_record& record, const arm64_context& ctx)
        {
            static_assert(sizeof(arm64_context) <= arm64_exception_record_offset);

            const auto exception_record_size = calculate_exception_record_size(record);
            const auto allocation_size = arm64_exception_record_offset + exception_record_size;
            const auto initial_sp = cpu.read_stack_pointer();
            const auto new_sp = align_down(initial_sp - allocation_size, 0x10);

            std::vector<uint8_t> zero_memory(static_cast<size_t>(initial_sp - new_sp), 0);
            thread.grow_stack_for_frame(new_sp, initial_sp);
            cpu.write_memory(new_sp, zero_memory.data(), zero_memory.size());
            cpu.write_memory(new_sp, ctx);

            emulator_allocator allocator{cpu, new_sp + arm64_exception_record_offset, exception_record_size};
            const auto exception_record_obj = save_exception_record(allocator, record);
            if (exception_record_obj.value() != allocator.get_base())
            {
                throw std::runtime_error("Bad exception record position on stack");
            }

            cpu.reg(arm64_register::sp, new_sp);
            cpu.reg(arm64_register::pc, win_emu.process.ki_user_exception_dispatcher);
        }

        void sync_wow64_cpu_reserved_context(windows_emulator& win_emu, x86_64_cpu& emu, emulator_thread& thread, const CONTEXT64& ctx)
        {
            if (!win_emu.process.is_wow64_process)
            {
                return;
            }

            const auto bitness = segment_utils::get_segment_bitness(emu, ctx.SegCs);
            if (!bitness || *bitness != segment_utils::segment_bitness::bit32)
            {
                return;
            }

            // Wow64PassExceptionToGuest rebuilds the 32-bit context from WOW64_CPURESERVED
            // (TEB64 TLS slot 1), not from the native exception ContextRecord below.
            thread.wow64_cpu_reserved->access([&](WOW64_CPURESERVED& cpu) {
                cpu.Flags |= WOW64_CPURESERVED_FLAG_RESET_STATE;
                cpu.Context = make_wow64_context(ctx);
            });
        }

    }

    WOW64_CONTEXT make_wow64_context(const CONTEXT64& ctx)
    {
        WOW64_CONTEXT result{};
        result.ContextFlags = CONTEXT32_ALL;
        result.Dr0 = static_cast<DWORD>(ctx.Dr0);
        result.Dr1 = static_cast<DWORD>(ctx.Dr1);
        result.Dr2 = static_cast<DWORD>(ctx.Dr2);
        result.Dr3 = static_cast<DWORD>(ctx.Dr3);
        result.Dr6 = static_cast<DWORD>(ctx.Dr6);
        result.Dr7 = static_cast<DWORD>(ctx.Dr7);
        result.SegGs = ctx.SegGs;
        result.SegFs = ctx.SegFs;
        result.SegEs = ctx.SegEs;
        result.SegDs = ctx.SegDs;
        result.Edi = static_cast<DWORD>(ctx.Rdi);
        result.Esi = static_cast<DWORD>(ctx.Rsi);
        result.Ebx = static_cast<DWORD>(ctx.Rbx);
        result.Edx = static_cast<DWORD>(ctx.Rdx);
        result.Ecx = static_cast<DWORD>(ctx.Rcx);
        result.Eax = static_cast<DWORD>(ctx.Rax);
        result.Ebp = static_cast<DWORD>(ctx.Rbp);
        result.Eip = static_cast<DWORD>(ctx.Rip);
        result.SegCs = ctx.SegCs;
        result.EFlags = ctx.EFlags;
        result.Esp = static_cast<DWORD>(ctx.Rsp);
        result.SegSs = ctx.SegSs;
        result.FloatSave.ControlWord = 0x037F;
        result.FloatSave.TagWord = 0xFFFF;

        XMM_SAVE_AREA32 xmm_state{};
        xmm_state.ControlWord = 0x037F;
        // FXSAVE abridged x87 tag: 0x00 = empty stack (fresh FPU). 0xFF (all valid) makes the next x87
        // FLD overflow the stack and yield the x87 indefinite (NaN).
        xmm_state.TagWord = 0x00;
        xmm_state.MxCsr = 0x1F80;
        xmm_state.MxCsr_Mask = 0xFFFFFFFF;
        static_assert(sizeof(xmm_state) <= sizeof(result.ExtendedRegisters));
        memcpy(result.ExtendedRegisters, &xmm_state, sizeof(xmm_state));
        return result;
    }

    bool dispatch_debug_exception(windows_emulator& win_emu, CONTEXT64& ctx, EMU_EXCEPTION_RECORD<EmulatorTraits<Emu64>>& record)
    {
        std::array<uint8_t, 2> ins = {0};

        // CD 2D int 2dh
        if (win_emu.memory.try_read_memory(ctx.Rip, &ins, sizeof(ins)) && ins[0] == 0xCD && ins[1] == 0x2D)
        {
            // skip 2 bytes int 2dh
            ctx.Rip += 2;

            record.NumberParameters = 3;

            record.ExceptionInformation[0] = ctx.Rax;
            record.ExceptionInformation[1] = ctx.Rcx;
            record.ExceptionInformation[2] = ctx.Rdx;

            return true;
        }

        return false;
    }

    void dispatch_exception(windows_emulator& win_emu, vcpu_context& vcpu, const DWORD status,
                            const std::vector<EmulatorTraits<Emu64>::ULONG_PTR>& parameters)
    {
        auto& thread = vcpu.thread();
        thread.next_branch_step.reset();

        win_emu.record_exception_trace({
            .status = static_cast<uint32_t>(status),
            .tid = thread.id,
            .vcpu = static_cast<uint32_t>(vcpu.cpu.index()),
            .rip = vcpu.cpu.read_instruction_pointer(),
            .info = parameters.size() > 1 ? static_cast<uint64_t>(parameters[1]) : 0,
        });

        if (auto* arm64 = dynamic_cast<arm64_cpu*>(&vcpu.cpu))
        {
            arm64_context ctx{};
            ctx.ContextFlags = arm64_context_all;
            cpu_context::save(*arm64, ctx);
            ctx.Pc = win_emu.uses_instruction_precision() ? thread.current_ip : arm64->read_instruction_pointer();

            if (parameters.size() > 15)
            {
                throw std::runtime_error("Too many exception parameters");
            }

            exception_record record{};
            record.ExceptionCode = status;
            record.NumberParameters = static_cast<DWORD>(parameters.size());
            std::ranges::copy(parameters, record.ExceptionInformation);
            record.ExceptionAddress = ctx.Pc;

            if (win_emu.debugger().is_attached())
            {
                arm64->reg(arm64_register::pc, ctx.Pc);
                if (report_exception_to_debugger(win_emu, vcpu, record, true))
                {
                    return;
                }
            }

            dispatch_arm64_exception(win_emu, *arm64, thread, record, ctx);
            return;
        }

        auto& cpu = vcpu.x64_cpu();
        // A step of the backend's own over the raising instruction ends first, so the context holds the guest's own flags.
        cpu.end_own_single_step();
        CONTEXT64 ctx{};
        ctx.ContextFlags = CONTEXT64_ALL;
        cpu_context::save(cpu, ctx);

        // A single step is a trap: it is raised after the instruction, which current_ip still names.
        ctx.Rip = win_emu.uses_instruction_precision() && status != STATUS_SINGLE_STEP //
                      ? thread.current_ip
                      : vcpu.cpu.read_instruction_pointer();

        exception_record record{};
        memset(&record, 0, sizeof(record));
        record.ExceptionCode = status;
        record.ExceptionFlags = 0;
        record.ExceptionRecord = 0;
        record.NumberParameters = 0;

        bool is_debug_exception = false;
        if (status == STATUS_BREAKPOINT)
        {
            is_debug_exception = dispatch_debug_exception(win_emu, ctx, record);
        }

        if (!is_debug_exception)
        {
            record.NumberParameters = static_cast<DWORD>(parameters.size());

            if (parameters.size() > 15)
            {
                throw std::runtime_error("Too many exception parameters");
            }

            for (size_t i = 0; i < parameters.size(); ++i)
            {
                record.ExceptionInformation[i] = parameters[i];
            }
        }

        record.ExceptionAddress = ctx.Rip;

        if (win_emu.debugger().is_attached())
        {
            // A debugger sees a breakpoint with RIP past the int3, so continuing it moves on; the guest's handlers see
            // RIP on the int3.
            uint64_t debugger_ip = ctx.Rip;
            if (status == STATUS_BREAKPOINT && !is_debug_exception)
            {
                std::array<uint8_t, 2> instruction{};
                if (win_emu.memory.try_read_memory(ctx.Rip, instruction.data(), instruction.size()))
                {
                    if (instruction[0] == 0xCC)
                    {
                        debugger_ip += 1;
                    }
                    else if (instruction[0] == 0xCD && instruction[1] == 0x03)
                    {
                        debugger_ip += 2;
                    }
                }
            }

            // Like KiDispatchException, a breakpoint or single step in WoW64's 32-bit code reaches the debugger as its
            // WX86 variant, which a 32-bit debugger's WoW64 layer turns back into the plain one.
            auto debugger_record = record;
            const auto bitness = segment_utils::get_segment_bitness(cpu, ctx.SegCs);
            if (win_emu.process.is_wow64_process && bitness && *bitness == segment_utils::segment_bitness::bit32)
            {
                if (status == STATUS_BREAKPOINT)
                {
                    debugger_record.ExceptionCode = STATUS_WX86_BREAKPOINT;
                }
                else if (status == STATUS_SINGLE_STEP)
                {
                    debugger_record.ExceptionCode = STATUS_WX86_SINGLE_STEP;
                }
            }

            cpu.reg(x86_register::rip, debugger_ip);
            if (report_exception_to_debugger(win_emu, vcpu, debugger_record, true))
            {
                return;
            }
        }

        sync_wow64_cpu_reserved_context(win_emu, cpu, thread, ctx);

        EMU_EXCEPTION_POINTERS<EmulatorTraits<Emu64>> pointers{};
        pointers.ContextRecord = reinterpret_cast<EmulatorTraits<Emu64>::PVOID>(&ctx);
        pointers.ExceptionRecord = reinterpret_cast<EmulatorTraits<Emu64>::PVOID>(&record);
        dispatch_exception_pointers(cpu, thread, win_emu.process.ki_user_exception_dispatcher, pointers);
    }

    bool report_exception_to_debugger(windows_emulator& win_emu, vcpu_context& vcpu, const exception_record& record,
                                      const bool first_chance)
    {
        if (!win_emu.debugger().is_attached())
        {
            return false;
        }

        process_debug_event event{
            .kind = process_debug_event_kind::exception,
            .thread_id = vcpu.thread().id,
            .exception_code = record.ExceptionCode,
            .exception_flags = record.ExceptionFlags,
            .exception_address = record.ExceptionAddress,
            .first_chance = first_chance,
        };
        const auto parameter_count = std::min<size_t>(record.NumberParameters, std::size(record.ExceptionInformation));
        event.exception_parameters.assign(record.ExceptionInformation, record.ExceptionInformation + parameter_count);

        return win_emu.report_debug_event(std::move(event)) != static_cast<uint32_t>(DBG_EXCEPTION_NOT_HANDLED);
    }

    void dispatch_exception_record(windows_emulator& win_emu, vcpu_context& vcpu, exception_record record, CONTEXT64 ctx)
    {
        auto& cpu = vcpu.x64_cpu();

        // A chained record is a guest pointer here, which the dispatcher would read as a host one.
        record.ExceptionRecord = 0;

        // The kernel builds the dispatcher frame below the raising context's stack, not the syscall's.
        cpu.reg(x86_register::rsp, ctx.Rsp);

        // KiDispatchException enters the dispatcher with the 64-bit code selector and the trap flag clear in the trap
        // frame, while the CONTEXT it hands the handlers keeps the raised ones. The system call runs in 64-bit code, so
        // the dispatcher is entered from there, also when a debugger that declined the exception was shown the raised
        // selector.
        cpu.reg<uint16_t>(x86_register::cs, cpu_context::user_code_selector);
        cpu.reg<uint32_t>(x86_register::eflags, cpu.reg<uint32_t>(x86_register::eflags) & ~cpu_context::eflags_trap);
        sync_wow64_cpu_reserved_context(win_emu, cpu, vcpu.thread(), ctx);

        EMU_EXCEPTION_POINTERS<EmulatorTraits<Emu64>> pointers{};
        pointers.ContextRecord = reinterpret_cast<EmulatorTraits<Emu64>::PVOID>(&ctx);
        pointers.ExceptionRecord = reinterpret_cast<EmulatorTraits<Emu64>::PVOID>(&record);
        dispatch_exception_pointers(cpu, vcpu.thread(), win_emu.process.ki_user_exception_dispatcher, pointers);
    }

    void dispatch_exception_record(windows_emulator& win_emu, vcpu_context& vcpu, exception_record record, const arm64_context& ctx)
    {
        auto& cpu = vcpu.arm64_cpu_ref();

        // A chained record is a guest pointer here, which the dispatcher would read as a host one.
        record.ExceptionRecord = 0;

        // The kernel builds the dispatcher frame below the raising context's stack, not the syscall's.
        cpu.reg(arm64_register::sp, ctx.Sp);
        dispatch_arm64_exception(win_emu, cpu, vcpu.thread(), record, ctx);
    }

    void dispatch_access_violation(windows_emulator& win_emu, vcpu_context& vcpu, const uint64_t address, const memory_operation operation)
    {
        dispatch_exception(win_emu, vcpu, STATUS_ACCESS_VIOLATION,
                           {
                               map_violation_operation_to_parameter(operation),
                               address,
                           });
    }

    void dispatch_guard_page_violation(windows_emulator& win_emu, vcpu_context& vcpu, const uint64_t address,
                                       const memory_operation operation)
    {
        dispatch_exception(win_emu, vcpu, STATUS_GUARD_PAGE_VIOLATION,
                           {
                               map_violation_operation_to_parameter(operation),
                               address,
                           });
    }

    void dispatch_stack_overflow(windows_emulator& win_emu, vcpu_context& vcpu, const uint64_t address, const memory_operation operation)
    {
        dispatch_exception(win_emu, vcpu, STATUS_STACK_OVERFLOW,
                           {
                               map_violation_operation_to_parameter(operation),
                               address,
                           });
    }

    void dispatch_illegal_instruction_violation(windows_emulator& win_emu, vcpu_context& vcpu)
    {
        dispatch_exception(win_emu, vcpu, STATUS_ILLEGAL_INSTRUCTION, {});
    }

    namespace
    {
        // The KiOpOneByteTable and KiOpTwoByteTable entries whose KiOp_Priv handler makes a user-mode #GP raise
        // STATUS_PRIVILEGED_INSTRUCTION. The remaining entries serve #UD and #DE, or only act for flagged threads or with
        // CR4.UMIP set, which sogen's guests never have, so KiPreprocessFault leaves those faults as access violations.
        struct privileged_opcode
        {
            uint8_t first{};
            uint8_t count{1};
            uint8_t modrm_reg{0xFF};
            uint8_t modrm_mod{0xFF}; // 3 matches the register form only, any other value the memory form only
            uint8_t modrm_rm{0xFF};
        };

        constexpr std::array<privileged_opcode, 6> one_byte_privileged_opcodes{{
            {.first = 0xFA},             // CLI
            {.first = 0xFB},             // STI
            {.first = 0xE4, .count = 4}, // IN/OUT imm8
            {.first = 0xEC, .count = 4}, // IN/OUT DX
            {.first = 0x6C, .count = 4}, // INS/OUTS
            {.first = 0xF4},             // HLT
        }};

        constexpr std::array<privileged_opcode, 14> two_byte_privileged_opcodes{{
            {.first = 0x00, .modrm_reg = 2},                                // LLDT
            {.first = 0x00, .modrm_reg = 3},                                // LTR
            {.first = 0x01, .modrm_reg = 2, .modrm_mod = 0},                // LGDT
            {.first = 0x01, .modrm_reg = 3, .modrm_mod = 0},                // LIDT
            {.first = 0x01, .modrm_reg = 6},                                // LMSW
            {.first = 0x01, .modrm_reg = 7, .modrm_mod = 0},                // INVLPG
            {.first = 0x01, .modrm_reg = 7, .modrm_mod = 3, .modrm_rm = 0}, // SWAPGS
            {.first = 0x06},                                                // CLTS
            {.first = 0x07},                                                // SYSRET
            {.first = 0x08},                                                // INVD
            {.first = 0x09},                                                // WBINVD
            {.first = 0x20, .count = 4},                                    // MOV to and from CR/DR
            {.first = 0x30, .count = 4},                                    // WRMSR, RDTSC, RDMSR, RDPMC
            {.first = 0x35},                                                // SYSEXIT
        }};

        // KiOpPrefixTable. KiOpIsPrefix fails the decode on a repeated prefix or a second one from the same group.
        struct instruction_prefix
        {
            uint8_t value{};
            uint16_t flag{};
            uint8_t group{};
        };

        constexpr std::array<instruction_prefix, 11> instruction_prefixes{{
            {.value = 0x26, .flag = 0x001, .group = 1},
            {.value = 0x2E, .flag = 0x002, .group = 1},
            {.value = 0x36, .flag = 0x004, .group = 1},
            {.value = 0x3E, .flag = 0x008, .group = 1},
            {.value = 0x64, .flag = 0x010, .group = 1},
            {.value = 0x65, .flag = 0x020, .group = 1},
            {.value = 0x66, .flag = 0x040, .group = 0},
            {.value = 0x67, .flag = 0x080, .group = 0},
            {.value = 0xF0, .flag = 0x100, .group = 0},
            {.value = 0xF2, .flag = 0x200, .group = 2},
            {.value = 0xF3, .flag = 0x400, .group = 2},
        }};

        constexpr uint16_t rex_prefix_flag = 0x800;

        const instruction_prefix* find_instruction_prefix(const uint8_t value)
        {
            for (const auto& prefix : instruction_prefixes)
            {
                if (prefix.value == value)
                {
                    return &prefix;
                }
            }

            return nullptr;
        }

        bool matches_modrm(const privileged_opcode& entry, const uint8_t modrm)
        {
            const auto is_register_form = (modrm & 0xC0) == 0xC0;
            return (entry.modrm_reg == 0xFF || entry.modrm_reg == ((modrm >> 3) & 7)) &&
                   (entry.modrm_rm == 0xFF || entry.modrm_rm == (modrm & 7)) &&
                   (entry.modrm_mod == 0xFF || (entry.modrm_mod == 3) == is_register_form);
        }
    }

    NTSTATUS get_general_protection_fault_status(const std::span<const uint8_t> instruction, const bool is_32bit)
    {
        size_t offset = 0;
        uint16_t prefix_flags = 0;
        uint8_t prefix_groups = 0;

        for (; offset < instruction.size(); ++offset)
        {
            const auto value = instruction[offset];
            uint16_t flag = 0;
            uint8_t group = 0;

            if (!is_32bit && (value & 0xF0) == 0x40)
            {
                flag = rex_prefix_flag;
            }
            else if (const auto* const prefix = find_instruction_prefix(value))
            {
                flag = prefix->flag;
                group = prefix->group;
            }
            else
            {
                break;
            }

            if ((prefix_flags & flag) != 0 || (prefix_groups & group) != 0)
            {
                return STATUS_ACCESS_VIOLATION;
            }

            prefix_flags |= flag;
            prefix_groups |= group;
        }

        const auto byte_at = [&](const size_t index) -> std::optional<uint8_t> {
            return index < instruction.size() ? std::optional{instruction[index]} : std::nullopt;
        };

        auto opcode = byte_at(offset);
        std::span<const privileged_opcode> table = one_byte_privileged_opcodes;
        if (opcode == 0x0F)
        {
            opcode = byte_at(++offset);
            table = two_byte_privileged_opcodes;
        }

        if (!opcode)
        {
            return STATUS_ACCESS_VIOLATION;
        }

        const auto modrm = byte_at(offset + 1);
        for (const auto& entry : table)
        {
            if (*opcode < entry.first || *opcode >= entry.first + entry.count)
            {
                continue;
            }

            if (entry.modrm_reg == 0xFF && entry.modrm_mod == 0xFF && entry.modrm_rm == 0xFF)
            {
                return STATUS_PRIVILEGED_INSTRUCTION;
            }

            if (!modrm)
            {
                return STATUS_ACCESS_VIOLATION;
            }

            if (matches_modrm(entry, *modrm))
            {
                return STATUS_PRIVILEGED_INSTRUCTION;
            }
        }

        return STATUS_ACCESS_VIOLATION;
    }

    void dispatch_general_protection_fault(windows_emulator& win_emu, vcpu_context& vcpu)
    {
        auto& cpu = vcpu.x64_cpu();
        const auto rip = win_emu.uses_instruction_precision() ? vcpu.thread().current_ip : cpu.read_instruction_pointer();

        // KiOpFetchNextByte decodes at most 16 bytes and stops at the first unreadable one.
        std::array<uint8_t, 16> instruction{};
        size_t length = 0;
        while (length < instruction.size() && win_emu.memory.try_read_memory(rip + length, &instruction[length], 1))
        {
            ++length;
        }

        // KiOpDecode only treats 0x40-0x4F as REX outside the 32-bit user code segment.
        const auto is_32bit = cpu.reg<uint16_t>(x86_register::cs) == 0x23;
        const auto status = get_general_protection_fault_status({instruction.data(), length}, is_32bit);
        if (status == STATUS_ACCESS_VIOLATION)
        {
            // KiPreprocessFault reports any other #GP as a read of the all-ones address.
            dispatch_access_violation(win_emu, vcpu, ~uint64_t{0}, memory_operation::read);
            return;
        }

        dispatch_exception(win_emu, vcpu, status, {});
    }

    void dispatch_integer_division_by_zero(windows_emulator& win_emu, vcpu_context& vcpu)
    {
        dispatch_exception(win_emu, vcpu, STATUS_INTEGER_DIVIDE_BY_ZERO, {});
    }

    namespace
    {
        constexpr uint32_t eflags_carry = 0x1;
        constexpr uint32_t eflags_parity = 0x4;
        constexpr uint32_t eflags_zero = 0x40;
        constexpr uint32_t eflags_sign = 0x80;
        constexpr uint32_t eflags_overflow = 0x800;

        constexpr uint64_t dr6_single_step = 0x4000;
        constexpr uint64_t dr7_last_branch = 0x100;
        constexpr uint64_t dr7_branch_trace = 0x200;
        constexpr uint64_t dr7_enable_bits = 0x3;
        constexpr int debug_address_register_count = 4;

        // KiSaveDebugRegisterState reads the last branch from the LBR MSRs, which read 0 on 19045 under Hyper-V.
        constexpr EmulatorTraits<Emu64>::ULONG_PTR unrecorded_branch = 0;

        constexpr uint8_t rex_prefix_mask = 0xF0;
        constexpr uint8_t rex_prefix = 0x40;
        constexpr uint8_t address_size_prefix = 0x67;
        constexpr uint8_t two_byte_escape = 0x0F;
        constexpr uint8_t opcode_jcc_short_first = 0x70;
        constexpr uint8_t opcode_jcc_short_last = 0x7F;
        constexpr uint8_t escaped_opcode_jcc_near_first = 0x80;
        constexpr uint8_t escaped_opcode_jcc_near_last = 0x8F;
        constexpr uint8_t opcode_call_far = 0x9A;
        constexpr uint8_t opcode_ret_imm16 = 0xC2;
        constexpr uint8_t opcode_ret = 0xC3;
        constexpr uint8_t opcode_retf_imm16 = 0xCA;
        constexpr uint8_t opcode_retf = 0xCB;
        constexpr uint8_t opcode_iret = 0xCF;
        constexpr uint8_t opcode_loopne = 0xE0;
        constexpr uint8_t opcode_loope = 0xE1;
        constexpr uint8_t opcode_loop = 0xE2;
        constexpr uint8_t opcode_jrcxz = 0xE3;
        constexpr uint8_t opcode_call = 0xE8;
        constexpr uint8_t opcode_jmp = 0xE9;
        constexpr uint8_t opcode_jmp_far = 0xEA;
        constexpr uint8_t opcode_jmp_short = 0xEB;
        constexpr uint8_t opcode_group5 = 0xFF;
        constexpr uint8_t group5_first_branch = 2;
        constexpr uint8_t group5_last_branch = 5;
        constexpr uint8_t jcc_condition_mask = 0xF;
        constexpr uint8_t jcc_negated = 1;

        enum class jcc_condition : uint8_t
        {
            overflow,
            below,
            equal,
            below_or_equal,
            sign,
            parity,
            less,
            less_or_equal,
        };

        bool hits_an_enabled_breakpoint(const uint64_t dr6, const uint64_t dr7)
        {
            for (int i = 0; i < debug_address_register_count; ++i)
            {
                if ((dr6 & (1ull << i)) != 0 && ((dr7 >> (i * 2)) & dr7_enable_bits) != 0)
                {
                    return true;
                }
            }

            return false;
        }

        bool jcc_condition_holds(const uint8_t condition, const uint32_t eflags)
        {
            const auto carry = (eflags & eflags_carry) != 0;
            const auto parity = (eflags & eflags_parity) != 0;
            const auto zero = (eflags & eflags_zero) != 0;
            const auto sign = (eflags & eflags_sign) != 0;
            const auto overflow = (eflags & eflags_overflow) != 0;

            bool holds{};
            switch (static_cast<jcc_condition>(condition >> 1))
            {
            case jcc_condition::overflow:
                holds = overflow;
                break;
            case jcc_condition::below:
                holds = carry;
                break;
            case jcc_condition::equal:
                holds = zero;
                break;
            case jcc_condition::below_or_equal:
                holds = carry || zero;
                break;
            case jcc_condition::sign:
                holds = sign;
                break;
            case jcc_condition::parity:
                holds = parity;
                break;
            case jcc_condition::less:
                holds = sign != overflow;
                break;
            case jcc_condition::less_or_equal:
                holds = zero || sign != overflow;
                break;
            }

            return (condition & jcc_negated) != 0 ? !holds : holds;
        }

        stepped_instruction read_stepped_instruction(memory_manager& memory, const uint64_t address, const bool is_32bit)
        {
            stepped_instruction instruction{.address = address, .is_32bit = is_32bit};
            while (instruction.length < instruction.bytes.size() &&
                   memory.try_read_memory(address + instruction.length, &instruction.bytes[instruction.length], 1))
            {
                ++instruction.length;
            }

            return instruction;
        }
    }

    // A Jcc or a LOOP leaves the flags as they were, and a LOOP has counted RCX down.
    bool is_taken_branch(const std::span<const uint8_t> instruction, const uint32_t eflags, const uint64_t rcx, const bool is_32bit)
    {
        size_t offset = 0;
        bool address_size_override = false;
        for (; offset < instruction.size(); ++offset)
        {
            const auto value = instruction[offset];
            const auto is_rex = !is_32bit && (value & rex_prefix_mask) == rex_prefix;
            if (!is_rex && !find_instruction_prefix(value))
            {
                break;
            }

            address_size_override |= value == address_size_prefix;
        }

        if (offset >= instruction.size())
        {
            return false;
        }

        const auto opcode = instruction[offset];
        const auto next = offset + 1 < instruction.size() ? std::optional{instruction[offset + 1]} : std::nullopt;

        const auto counter = [&] {
            const auto counter_bits = (is_32bit ? 32 : 64) >> (address_size_override ? 1 : 0);
            return counter_bits == 64 ? rcx : rcx & ((uint64_t{1} << counter_bits) - 1);
        };

        switch (opcode)
        {
        case opcode_call:
        case opcode_jmp:
        case opcode_jmp_short:
        case opcode_ret_imm16:
        case opcode_ret:
        case opcode_retf_imm16:
        case opcode_retf:
        case opcode_iret:
            return true;
        case opcode_call_far:
        case opcode_jmp_far:
            return is_32bit;
        case opcode_loopne:
        case opcode_loope:
        case opcode_loop:
            return counter() != 0 && (opcode == opcode_loop || (opcode == opcode_loope) == ((eflags & eflags_zero) != 0));
        case opcode_jrcxz:
            return counter() == 0;
        case opcode_group5: {
            const auto operation = next ? (*next >> 3) & 7 : 0;
            return operation >= group5_first_branch && operation <= group5_last_branch;
        }
        case two_byte_escape:
            return next && *next >= escaped_opcode_jcc_near_first && *next <= escaped_opcode_jcc_near_last &&
                   jcc_condition_holds(*next & jcc_condition_mask, eflags);
        default:
            return opcode >= opcode_jcc_short_first && opcode <= opcode_jcc_short_last &&
                   jcc_condition_holds(opcode & jcc_condition_mask, eflags);
        }
    }

    // With DR7.GE set, KiRestoreDebugRegisterState sets DEBUGCTL.BTF: the trap flag traps only after a taken branch.
    bool continue_branch_step(windows_emulator& win_emu, vcpu_context& vcpu)
    {
        auto* cpu = dynamic_cast<x86_64_cpu*>(&vcpu.cpu);
        if (!cpu)
        {
            return false;
        }

        auto& thread = vcpu.thread();
        const auto stepped = std::exchange(thread.next_branch_step, std::nullopt);
        const auto dr6 = cpu->reg(x86_register::dr6);
        const auto dr7 = cpu->reg(x86_register::dr7);
        if (!stepped || (dr7 & dr7_branch_trace) == 0 || (dr6 & dr6_single_step) == 0 || hits_an_enabled_breakpoint(dr6, dr7))
        {
            return false;
        }

        if (is_taken_branch({stepped->bytes.data(), stepped->length}, cpu->reg<uint32_t>(x86_register::eflags), cpu->reg(x86_register::rcx),
                            stepped->is_32bit))
        {
            return false;
        }

        // No debug exception, and so no DR6.BS: the processor sets it only for one it raises.
        cpu->reg(x86_register::dr6, dr6 & ~dr6_single_step);
        // With instruction precision the execution hook takes the next instruction before it runs.
        if (!win_emu.uses_instruction_precision())
        {
            remember_branch_step(win_emu, thread, *cpu, cpu->read_instruction_pointer());
        }

        return true;
    }

    void remember_branch_step(windows_emulator& win_emu, emulator_thread& thread, x86_64_cpu& cpu, const uint64_t address)
    {
        thread.steps_on_branches = (cpu.reg(x86_register::dr7) & dr7_branch_trace) != 0;
        if (!thread.steps_on_branches || (cpu.reg<uint32_t>(x86_register::eflags) & cpu_context::eflags_trap) == 0)
        {
            thread.next_branch_step.reset();
            return;
        }

        const auto is_32bit = cpu.reg<uint16_t>(x86_register::cs) == cpu_context::wow64_code_selector;
        thread.next_branch_step = read_stepped_instruction(win_emu.memory, address, is_32bit);
    }

    void dispatch_single_step(windows_emulator& win_emu, vcpu_context& vcpu, const bool raised_by_trap_flag)
    {
        auto* x64 = dynamic_cast<x86_64_cpu*>(&vcpu.cpu);

        // KxDebugTrapOrFault reports a trap flag step under DR7.LE and DR7.GE as a branch trace, with the source and the
        // target of the last branch KiSaveDebugRegisterState read.
        std::vector<EmulatorTraits<Emu64>::ULONG_PTR> parameters{};
        constexpr auto branch_trace = dr7_last_branch | dr7_branch_trace;
        if (raised_by_trap_flag && x64 && (x64->reg(x86_register::dr7) & branch_trace) == branch_trace)
        {
            parameters = {unrecorded_branch, unrecorded_branch};
        }

        dispatch_exception(win_emu, vcpu, STATUS_SINGLE_STEP, parameters);

        // The step's DR6 bits stay in its CONTEXT only: KiExceptionDispatch returns to user mode through
        // KiRestoreDebugRegisterState, which clears DR6.
        if (x64)
        {
            x64->reg(x86_register::dr6, uint64_t{0});
        }
    }

    void dispatch_breakpoint(windows_emulator& win_emu, vcpu_context& vcpu)
    {
        dispatch_exception(win_emu, vcpu, STATUS_BREAKPOINT, {});
    }

} // namespace sogen
