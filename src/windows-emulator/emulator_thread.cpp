#include "std_include.hpp"
#include "emulator_thread.hpp"

#include "cpu_context.hpp"
#include "process_context.hpp"

namespace
{
    void setup_wow64_fs_segment(memory_manager& memory, uint64_t teb32_addr)
    {
        const uint64_t base = teb32_addr;
        const uint32_t limit = 0xFFF; // 4KB - size of TEB32 (matching Windows)

        // Build the GDT descriptor matching Windows format exactly
        // Format: | Base[31:24] | G|D|L|AVL | Limit[19:16] | P|DPL|S|Type | Base[23:16] | Base[15:0] | Limit[15:0] |
        uint64_t descriptor = 0;
        descriptor |= (limit & 0xFFFF);                                       // Limit[15:0]
        descriptor |= ((base & 0xFFFF) << 16);                                // Base[15:0]
        descriptor |= ((base & 0xFF0000) << 16);                              // Base[23:16]
        descriptor |= (0xF3ULL << 40);                                        // P=1, DPL=3, S=1, Type=3 (Data RW Accessed)
        descriptor |= (static_cast<uint64_t>((limit & 0xF0000) >> 16) << 48); // Limit[19:16]
        descriptor |= (0x40ULL << 52);                                        // G=0 (byte), D=1 (32-bit), L=0, AVL=0
        descriptor |= ((base & 0xFF000000) << 32);                            // Base[31:24]

        // Write the updated descriptor to GDT index 10 (selector 0x53)
        constexpr uint64_t fs_gdt_offset = GDT_ADDR + 10 * sizeof(uint64_t);
        memory.write_memory(fs_gdt_offset, &descriptor, sizeof(descriptor));
    }

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

    void setup_stack(x86_64_emulator& emu, const process_context& context, const uint64_t stack_base, const size_t stack_size)
    {
        if (!context.is_wow64_process)
        {
            const uint64_t stack_end = stack_base + stack_size;
            emu.reg(x86_register::rsp, stack_end);
        }
        else
        {
            const uint64_t stack_end = stack_base + stack_size - sizeof(WOW64_CPURESERVED) - 0x548;
            emu.reg(x86_register::rsp, stack_end);
        }
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
      // stack_size(page_align_up(std::max(stack_size, static_cast<uint64_t>(STACK_SIZE)))),
      start_address(start_address),
      argument(argument),
      id(id),
      suspended(suspended),
      last_registers(context.default_register_set)
{
    // native 64-bit
    if (!context.is_wow64_process)
    {
        this->stack_size = page_align_up(std::max(stack_size, static_cast<uint64_t>(STACK_SIZE)));
        this->stack_base = memory.allocate_memory(static_cast<size_t>(this->stack_size), memory_permission::read_write);

        this->gs_segment = emulator_allocator{
            memory,
            memory.allocate_memory(GS_SEGMENT_SIZE, memory_permission::read_write),
            GS_SEGMENT_SIZE,
        };

        this->teb64 = this->gs_segment->reserve<TEB64>();

        this->teb64->access([&](TEB64& teb_obj) {
            // Skips GetCurrentNlsCache
            // This hack can be removed once this is fixed:
            // https://github.com/momo5502/emulator/issues/128
            reinterpret_cast<uint8_t*>(&teb_obj)[0x179C] = 1;

            teb_obj.ClientId.UniqueProcess = 1ul;
            teb_obj.ClientId.UniqueThread = static_cast<uint64_t>(this->id);
            teb_obj.NtTib.StackLimit = this->stack_base;
            teb_obj.NtTib.StackBase = this->stack_base + this->stack_size;
            teb_obj.NtTib.Self = this->teb64->value();
            teb_obj.CurrentLocale = 0x409;
            teb_obj.ProcessEnvironmentBlock = context.peb64.value();
        });

        return;
    }

    // Default native size of wow64 is 256KB
    this->stack_size = WOW64_NATIVE_STACK_SIZE;
    this->wow64_stack_size = page_align_up(std::max(stack_size, static_cast<uint64_t>(STACK_SIZE)));

    // Set the default memory allocation address to the specified 32-bit address
    memory.set_default_allocation_address(DEFAULT_ALLOCATION_ADDRESS_32BIT);

    // Calculate required GS segment size for WOW64 (64-bit TEB + 32-bit TEB)
    constexpr uint64_t wow_teb_offset = 0x2000;
    constexpr size_t teb64_size = sizeof(TEB64);
    constexpr size_t teb32_size = sizeof(TEB32);                                // 4120 bytes
    const uint64_t required_gs_size = teb64_size + wow_teb_offset + teb32_size; // Need space for both TEBs
    const auto actual_gs_size =
        static_cast<size_t>((required_gs_size > GS_SEGMENT_SIZE) ? page_align_up(required_gs_size) : GS_SEGMENT_SIZE);

    // Allocate GS segment to hold both TEB32 and TEB64 for WOW64 process
    this->gs_segment = emulator_allocator{
        memory,
        memory.allocate_memory(actual_gs_size, memory_permission::read_write),
        actual_gs_size,
    };

    // Reserve and initialize 64-bit TEB first
    this->teb64 = this->gs_segment->reserve<TEB64>();

    // Allocate memory for native stack + WOW64_CPURESERVED structure
    this->stack_base = memory.allocate_memory(WOW64_NATIVE_STACK_SIZE, memory_permission::read_write);
    if (this->stack_base == 0)
    {
        throw std::runtime_error("Failed to allocate native stack + WOW64_CPURESERVED memory region");
        return;
    }

    uint64_t wow64_cpureserved_base = this->stack_base + this->stack_size - sizeof(WOW64_CPURESERVED);

    // Initialize 64-bit TEB first
    this->teb64->access([&](TEB64& teb_obj) {
        // Skips GetCurrentNlsCache
        // This hack can be removed once this is fixed:
        // https://github.com/momo5502/emulator/issues/128
        reinterpret_cast<uint8_t*>(&teb_obj)[0x179C] = 1;

        teb_obj.ClientId.UniqueProcess = 1ul;
        teb_obj.ClientId.UniqueThread = static_cast<uint64_t>(this->id);

        // Native 64-bit stack
        teb_obj.NtTib.StackLimit = this->stack_base;
        teb_obj.NtTib.StackBase = wow64_cpureserved_base;
        teb_obj.NtTib.Self = this->teb64->value();
        teb_obj.CurrentLocale = 0x409;

        teb_obj.ProcessEnvironmentBlock = context.peb64.value();
        teb_obj.StaticUnicodeString.MaximumLength = sizeof(teb_obj.StaticUnicodeBuffer);
        teb_obj.StaticUnicodeString.Buffer = this->teb64->value() + offsetof(TEB64, StaticUnicodeBuffer);

        // Set WowTebOffset to point to 32-bit TEB offset
        teb_obj.WowTebOffset = static_cast<int32_t>(wow_teb_offset); // 0x2000

        // Set TLS slot [1] to point to WOW64_CPURESERVED structure
        teb_obj.TlsSlots.arr[1 /* WOW64_TLS_CPURESERVED */] = wow64_cpureserved_base;

        // Note: TLS slot [10] (WOW64_INFO_PTR) will be set by wow64.dll during initialization
    });

    // Allocate dynamic 32-bit stack for WOW64 thread
    this->wow64_stack_base = memory.allocate_memory(static_cast<size_t>(this->wow64_stack_size.value()), memory_permission::read_write);

    // Create and initialize 32-bit TEB for WOW64
    // According to WinDbg: 32-bit TEB = 64-bit TEB + WowTebOffset (0x2000)
    const uint64_t teb64_addr = this->teb64->value(); // Base address of the 64-bit TEB.
    const uint64_t teb32_addr = teb64_addr + wow_teb_offset;
    uint64_t teb32_peb = 0;
    uint64_t nttib32_stack_base = this->wow64_stack_base.value() + this->wow64_stack_size.value();
    uint64_t nttib32_stack_limit = this->wow64_stack_base.value();

    // Create 32-bit TEB at the calculated offset within GS segment
    // We need to create it as an emulator_object at a specific address
    this->teb32 = emulator_object<TEB32>{memory, teb32_addr};

    // Initialize 32-bit TEB
    this->teb32->access([&](TEB32& teb32_obj) {
        // Set NT_TIB32 fields
        teb32_obj.NtTib.Self = static_cast<uint32_t>(teb32_addr);                // Self pointer to 32-bit TEB
        teb32_obj.NtTib.StackBase = static_cast<uint32_t>(nttib32_stack_base);   // Top of 32-bit stack (High address)
        teb32_obj.NtTib.StackLimit = static_cast<uint32_t>(nttib32_stack_limit); // Bottom of 32-bit stack (Low address)
        teb32_obj.NtTib.ExceptionList = static_cast<uint32_t>(0xffffffff);       // Must be 0xffffffff on 32-bit TEB
        teb32_obj.NtTib.SubSystemTib = static_cast<uint32_t>(0x0);
        teb32_obj.NtTib.FiberData = static_cast<uint32_t>(0x1e00);
        teb32_obj.NtTib.ArbitraryUserPointer = static_cast<uint32_t>(0x0);

        // Set ClientId for 32-bit TEB
        teb32_obj.ClientId.UniqueProcess = 1;
        teb32_obj.ClientId.UniqueThread = this->id;

        // Set 32-bit PEB pointer
        if (context.peb32.has_value())
        {
            teb32_obj.ProcessEnvironmentBlock = static_cast<uint32_t>(context.peb32->value());
            teb32_peb = teb32_obj.ProcessEnvironmentBlock;
        }
        else
        {
            // Fallback: WOW64 initialization will set this
            teb32_obj.ProcessEnvironmentBlock = 0;
        }

        teb32_obj.WowTebOffset = -0x2000;

        // Note: CurrentLocale and other fields will be initialized by WOW64 runtime
    });

    // CRITICAL: Setup FS segment (0x53) to point to 32-bit TEB for accurate WOW64 emulation
    // This mimics what Windows kernel does during NtCreateUserProcess for WOW64 processes
    // Without this, FS:0 won't correctly access the 32-bit TEB
    //
    // NOTE: We cannot use set_segment_base() here because that sets the FS_BASE MSR
    // which is for 64-bit flat addressing. 32-bit code uses actual GDT-based segmentation
    // with selector 0x53, so we must modify the GDT entry directly.
    setup_wow64_fs_segment(memory, teb32_addr);

    // Use the allocator to reserve memory for CONTEXT64
    this->wow64_cpu_reserved = emulator_object<WOW64_CPURESERVED>{memory, wow64_cpureserved_base};

    // Initialize with a WOW64_CONTEXT that represents the WOW64 initial state
    this->wow64_cpu_reserved->access([&](WOW64_CPURESERVED& ctx) {
        memset(&ctx, 0, sizeof(ctx));

        ctx.Flags = 0;
        ctx.MachineType = IMAGE_FILE_MACHINE_I386;

        // Set context flags for all state
        ctx.Context.ContextFlags = CONTEXT32_ALL;

        // Debug registers - all zero for initial state
        ctx.Context.Dr0 = 0;
        ctx.Context.Dr1 = 0;
        ctx.Context.Dr2 = 0;
        ctx.Context.Dr3 = 0;
        ctx.Context.Dr6 = 0;
        ctx.Context.Dr7 = 0;

        // Segment registers - WOW64 values
        ctx.Context.SegGs = 0x2b; // Standard 32-bit data segment
        ctx.Context.SegFs = 0x53; // WOW64 FS selector pointing to TEB32
        ctx.Context.SegEs = 0x2b; // Standard 32-bit data segment
        ctx.Context.SegDs = 0x2b; // Standard 32-bit data segment
        ctx.Context.SegCs = 0x23; // Standard 32-bit code segment
        ctx.Context.SegSs = 0x2b; // Standard 32-bit stack segment

        // General purpose registers - zero-extended 32-bit values
        ctx.Context.Edi = 0;
        ctx.Context.Esi = 0;
        ctx.Context.Edx = 0;
        ctx.Context.Ecx = 0;
        ctx.Context.Ebp = 0;

        // EBX - 32-bit PEB address
        ctx.Context.Ebx = static_cast<uint32_t>(teb32_peb);

        // EAX - thread entry point
        ctx.Context.Eax = static_cast<uint32_t>(this->start_address);

        // ESP - Fixed stack pointer at top of allocated stack
        ctx.Context.Esp = static_cast<uint32_t>(nttib32_stack_base - 0x10); // Leaving 0x10 bytes at top as per WinDbg

        // EIP - will be set to RtlUserThreadStart during setup_registers()
        ctx.Context.Eip = 0;

        // EFlags - standard initial flags
        ctx.Context.EFlags = 0x202; // IF (Interrupt Flag) set

        // Extended state - initialize to zero
        memset(&ctx.Context.FloatSave, 0, sizeof(ctx.Context.FloatSave));
        memset(&ctx.Context.ExtendedRegisters, 0, sizeof(ctx.Context.ExtendedRegisters));
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

    // Handle WOW64 process setup
    if (context.is_wow64_process && this->wow64_cpu_reserved.has_value())
    {
        // Set up WOW64 context with proper EIP
        this->wow64_cpu_reserved->access([&](WOW64_CPURESERVED& ctx) {
            // Set EIP to RtlUserThreadStart in 32-bit ntdll if available
            if (context.rtl_user_thread_start32.has_value())
            {
                ctx.Context.Eip = static_cast<uint32_t>(context.rtl_user_thread_start32.value());
            }
        });

        // For WOW64, also set FS segment base to point to 32-bit TEB
        // Windows kernel sets both GDT descriptor and FS_BASE MSR during thread creation
        if (this->teb32.has_value())
        {
            emu.set_segment_base(x86_register::fs, this->teb32->value());
        }
    }

    // Native 64-bit process setup
    setup_stack(emu, context, this->stack_base, static_cast<size_t>(this->stack_size));
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
