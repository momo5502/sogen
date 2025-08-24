#include "std_include.hpp"
#include "emulator_thread.hpp"

#include "cpu_context.hpp"
#include "process_context.hpp"

namespace
{
    template <typename T>
    emulator_object<T> allocate_object_on_stack(x86_64_emulator& emu)
    {
        const auto old_sp = emu.reg(x86_register::rsp);
        const auto new_sp = align_down(old_sp - sizeof(T), std::max(alignof(T), alignof(x86_64_emulator::pointer_type)));
        emu.reg(x86_register::rsp, new_sp);
        return {emu, new_sp};
    }

    void unalign_stack(x86_64_emulator& emu)
    {
        auto sp = emu.reg(x86_register::rsp);
        sp = align_down(sp - 0x10, 0x10) + 8;
        emu.reg(x86_register::rsp, sp);
    }

    void setup_stack(x86_64_emulator& emu, const uint64_t stack_base, const size_t stack_size)
    {
        const uint64_t stack_end = stack_base + stack_size;
        emu.reg(x86_register::rsp, stack_end);
    }

    bool is_object_signaled(process_context& c, const handle h, const uint32_t current_thread_id)
    {
        const auto type = h.value.type;

        switch (type)
        {
        default:
            break;

        case handle_types::event: {
            if (h.value.is_pseudo)
            {
                return true;
            }

            auto* e = c.events.get(h);
            if (e)
            {
                return e->is_signaled();
            }

            break;
        }

        case handle_types::mutant: {
            auto* e = c.mutants.get(h);
            return !e || e->try_lock(current_thread_id);
        }

        case handle_types::timer: {
            return true; // TODO
        }

        case handle_types::semaphore: {
            auto* s = c.semaphores.get(h);
            if (s)
            {
                return s->try_lock();
            }

            break;
        }

        case handle_types::thread: {
            const auto* t = c.threads.get(h);
            if (t)
            {
                return t->is_terminated();
            }

            break;
        }
        }

        throw std::runtime_error("Bad object: " + std::to_string(h.value.type));
    }
}

emulator_thread::emulator_thread(memory_manager& memory, const process_context& context, const uint64_t start_address,
                                 const uint64_t argument, const uint64_t stack_size, const bool suspended, const uint32_t id)
    : memory_ptr(&memory),
      stack_size(page_align_up(std::max(stack_size, static_cast<uint64_t>(STACK_SIZE)))),
      start_address(start_address),
      argument(argument),
      id(id),
      suspended(suspended),
      last_registers(context.default_register_set)
{
    this->stack_base = memory.allocate_memory(static_cast<size_t>(this->stack_size), memory_permission::read_write);

    this->gs_segment = emulator_allocator{
        memory,
        memory.allocate_memory(GS_SEGMENT_SIZE, memory_permission::read_write),
        GS_SEGMENT_SIZE,
    };

    this->teb = this->gs_segment->reserve<TEB64>();

    this->teb->access([&](TEB64& teb_obj) {
        // Skips GetCurrentNlsCache
        // This hack can be removed once this is fixed:
        // https://github.com/momo5502/emulator/issues/128
        reinterpret_cast<uint8_t*>(&teb_obj)[0x179C] = 1;

        teb_obj.ClientId.UniqueProcess = 1ul;
        teb_obj.ClientId.UniqueThread = static_cast<uint64_t>(this->id);
        teb_obj.NtTib.StackLimit = this->stack_base;
        teb_obj.NtTib.StackBase = this->stack_base + this->stack_size;
        teb_obj.NtTib.Self = this->teb->value();
        teb_obj.CurrentLocale = 0x409;
        teb_obj.ProcessEnvironmentBlock = context.peb.value();
    });
}

void emulator_thread::mark_as_ready(const NTSTATUS status)
{
    this->pending_status = status;
    this->await_time = {};
    this->await_objects = {};

    // TODO: Find out if this is correct
    if (this->waiting_for_alert)
    {
        this->alerted = false;
    }

    this->waiting_for_alert = false;
}

bool emulator_thread::is_terminated() const
{
    return this->exit_status.has_value();
}

bool emulator_thread::is_thread_ready(process_context& process, utils::clock& clock)
{
    if (this->is_terminated() || this->suspended > 0)
    {
        return false;
    }

    if (this->waiting_for_alert)
    {
        if (this->alerted)
        {
            this->mark_as_ready(STATUS_ALERTED);
            return true;
        }
        if (this->is_await_time_over(clock))
        {
            this->mark_as_ready(STATUS_TIMEOUT);
            return true;
        }

        return false;
    }

    if (!this->await_objects.empty())
    {
        bool all_signaled = true;
        for (uint32_t i = 0; i < this->await_objects.size(); ++i)
        {
            const auto& obj = this->await_objects[i];

            const auto signaled = is_object_signaled(process, obj, this->id);
            all_signaled &= signaled;

            if (signaled && this->await_any)
            {
                this->mark_as_ready(STATUS_WAIT_0 + i);
                return true;
            }
        }

        if (!this->await_any && all_signaled)
        {
            this->mark_as_ready(STATUS_SUCCESS);
            return true;
        }

        if (this->is_await_time_over(clock))
        {
            this->mark_as_ready(STATUS_TIMEOUT);
            return true;
        }

        return false;
    }

    if (this->await_time.has_value())
    {
        if (this->is_await_time_over(clock))
        {
            this->mark_as_ready(STATUS_SUCCESS);
            return true;
        }

        return false;
    }

    return true;
}

void emulator_thread::setup_registers(x86_64_emulator& emu, const process_context& context) const
{
    if (!this->gs_segment)
    {
        throw std::runtime_error("Missing GS segment");
    }

    setup_stack(emu, this->stack_base, static_cast<size_t>(this->stack_size));
    emu.set_segment_base(x86_register::gs, this->gs_segment->get_base());

    CONTEXT64 ctx{};
    ctx.ContextFlags = CONTEXT64_ALL;

    unalign_stack(emu);
    cpu_context::save(emu, ctx);

    ctx.Rip = context.rtl_user_thread_start;
    ctx.Rcx = this->start_address;
    ctx.Rdx = this->argument;

    const auto ctx_obj = allocate_object_on_stack<CONTEXT64>(emu);
    ctx_obj.write(ctx);

    unalign_stack(emu);

    emu.reg(x86_register::rcx, ctx_obj.value());
    emu.reg(x86_register::rdx, context.ntdll_image_base);
    emu.reg(x86_register::rip, context.ldr_initialize_thunk);
}
