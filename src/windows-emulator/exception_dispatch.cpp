#include "exception_dispatch.hpp"
#include "process_context.hpp"
#include "context_frame.hpp"

#include <platform/status.hpp>

namespace
{
    using exception_record = EMU_EXCEPTION_RECORD<EmulatorTraits<Emu64>>;
    using exception_record_map = std::unordered_map<const exception_record*, emulator_object<exception_record>>;

    emulator_object<exception_record> save_exception_record(emulator_allocator& allocator,
                                                            const exception_record& record,
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
                nested_record_obj = save_exception_record(
                    allocator, *reinterpret_cast<exception_record*>(record.ExceptionRecord), record_mapping);
            }

            record_obj.access([&](exception_record& r) {
                r.ExceptionRecord = reinterpret_cast<EmulatorTraits<Emu64>::PVOID>(nested_record_obj.ptr());
            });
        }

        return record_obj;
    }

    emulator_object<exception_record> save_exception_record(emulator_allocator& allocator,
                                                            const exception_record& record)
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
        case memory_operation::exec:
            return 1;
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

    struct machine_frame
    {
        uint64_t rip;
        uint64_t cs;
        uint64_t eflags;
        uint64_t rsp;
        uint64_t ss;
    };

    void dispatch_exception_pointers(x64_emulator& emu, const uint64_t dispatcher,
                                     const EMU_EXCEPTION_POINTERS<EmulatorTraits<Emu64>> pointers)
    {
        constexpr auto mach_frame_size = 0x40;
        constexpr auto context_record_size = 0x4F0;
        const auto exception_record_size =
            calculate_exception_record_size(*reinterpret_cast<exception_record*>(pointers.ExceptionRecord));
        const auto combined_size = align_up(exception_record_size + context_record_size, 0x10);

        assert(combined_size == 0x590);

        const auto allocation_size = combined_size + mach_frame_size;

        const auto initial_sp = emu.reg(x64_register::rsp);
        const auto new_sp = align_down(initial_sp - allocation_size, 0x100);

        const auto total_size = initial_sp - new_sp;
        assert(total_size >= allocation_size);

        std::vector<uint8_t> zero_memory{};
        zero_memory.resize(total_size, 0);

        emu.write_memory(new_sp, zero_memory.data(), zero_memory.size());

        emu.reg(x64_register::rsp, new_sp);
        emu.reg(x64_register::rip, dispatcher);

        const emulator_object<CONTEXT64> context_record_obj{emu, new_sp};
        context_record_obj.write(*reinterpret_cast<CONTEXT64*>(pointers.ContextRecord));

        emulator_allocator allocator{emu, new_sp + context_record_size, exception_record_size};
        const auto exception_record_obj =
            save_exception_record(allocator, *reinterpret_cast<exception_record*>(pointers.ExceptionRecord));

        if (exception_record_obj.value() != allocator.get_base())
        {
            throw std::runtime_error("Bad exception record position on stack");
        }

        const emulator_object<machine_frame> machine_frame_obj{emu, new_sp + combined_size};
        machine_frame_obj.access([&](machine_frame& frame) {
            const auto& record = *reinterpret_cast<CONTEXT64*>(pointers.ContextRecord);
            frame.rip = record.Rip;
            frame.rsp = record.Rsp;
            frame.ss = record.SegSs;
            frame.cs = record.SegCs;
            frame.eflags = record.EFlags;
        });
    }
}

void dispatch_exception(x64_emulator& emu, const process_context& proc, const DWORD status,
                        const std::vector<EmulatorTraits<Emu64>::ULONG_PTR>& parameters)
{
    CONTEXT64 ctx{};
    ctx.ContextFlags = CONTEXT64_ALL;
    context_frame::save(emu, ctx);

    exception_record record{};
    memset(&record, 0, sizeof(record));
    record.ExceptionCode = status;
    record.ExceptionFlags = 0;
    record.ExceptionRecord = 0;
    record.ExceptionAddress = emu.read_instruction_pointer();
    record.NumberParameters = static_cast<DWORD>(parameters.size());

    if (parameters.size() > 15)
    {
        throw std::runtime_error("Too many exception parameters");
    }

    for (size_t i = 0; i < parameters.size(); ++i)
    {
        record.ExceptionInformation[i] = parameters[i];
    }

    EMU_EXCEPTION_POINTERS<EmulatorTraits<Emu64>> pointers{};
    pointers.ContextRecord = reinterpret_cast<EmulatorTraits<Emu64>::PVOID>(&ctx);
    pointers.ExceptionRecord = reinterpret_cast<EmulatorTraits<Emu64>::PVOID>(&record);

    dispatch_exception_pointers(emu, proc.ki_user_exception_dispatcher, pointers);
}

void dispatch_access_violation(x64_emulator& emu, const process_context& proc, const uint64_t address,
                               const memory_operation operation)
{
    dispatch_exception(emu, proc, STATUS_ACCESS_VIOLATION,
                       {
                           map_violation_operation_to_parameter(operation),
                           address,
                       });
}

void dispatch_illegal_instruction_violation(x64_emulator& emu, const process_context& proc)
{
    dispatch_exception(emu, proc, STATUS_ILLEGAL_INSTRUCTION, {});
}

void dispatch_integer_division_by_zero(x64_emulator& emu, const process_context& proc)
{
    dispatch_exception(emu, proc, STATUS_INTEGER_DIVIDE_BY_ZERO, {});
}

void dispatch_single_step(x64_emulator& emu, const process_context& proc)
{
    dispatch_exception(emu, proc, STATUS_SINGLE_STEP, {});
}
