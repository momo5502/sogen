#include "std_include.hpp"
#include "emulator_thread.hpp"

#include "cpu_context.hpp"
#include "process_context.hpp"
#include "io_completion_wait.hpp"
#include "syscall_utils.hpp"

namespace sogen
{

    namespace
    {
        enum class wait_state
        {
            not_signaled,
            signaled,
            abandoned,
        };

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
            descriptor |= (0x40ULL << 48);                                        // G=0 (byte), D=1 (32-bit), L=0, AVL=0
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
                const uint64_t stack_end = stack_base + stack_size - sizeof(WOW64_CPURESERVED) - 0x1578;
                emu.reg(x86_register::rsp, stack_end);
            }
        }

        wait_state observe_object_signal(process_context& c, const handle h, const uint32_t current_thread_id)
        {
            const auto type = h.value.type;

            switch (type)
            {
            default:
                break;

            case handle_types::process:
                if (h == GUEST_PROCESS_HANDLE && c.exit_status.has_value())
                {
                    return wait_state::signaled;
                }

                break;

            case handle_types::event: {
                if (h.value.is_pseudo)
                {
                    return wait_state::signaled;
                }

                const auto* e = c.events.get(h);
                if (e && e->signaled)
                {
                    return wait_state::signaled;
                }

                break;
            }

            case handle_types::mutant: {
                const auto* e = c.mutants.get(h);
                if (e && e->is_signaled(current_thread_id))
                {
                    return e->abandoned ? wait_state::abandoned : wait_state::signaled;
                }

                break;
            }

            case handle_types::timer: {
                return wait_state::signaled; // TODO
            }

            case handle_types::semaphore: {
                const auto* s = c.semaphores.get(h);
                if (s && s->current_count > 0)
                {
                    return wait_state::signaled;
                }

                break;
            }

            case handle_types::port: {
                const auto* p = c.ports.get(h);
                if (p && p->has_pending_reply())
                {
                    return wait_state::signaled;
                }

                break;
            }

            case handle_types::thread: {
                const auto* t = c.threads.get(h);
                if (t && t->is_terminated())
                {
                    return wait_state::signaled;
                }

                break;
            }
            }

            return wait_state::not_signaled;
        }

        std::optional<wait_state> consume_object_signal(process_context& c, const handle h, const uint32_t current_thread_id)
        {
            switch (h.value.type)
            {
            case handle_types::process: {
                if (h != GUEST_PROCESS_HANDLE || !c.exit_status.has_value())
                {
                    return std::nullopt;
                }

                return wait_state::signaled;
            }

            case handle_types::event: {
                if (h.value.is_pseudo)
                {
                    return wait_state::signaled;
                }

                auto* event = c.events.get(h);
                if (!event || !event->signaled)
                {
                    return std::nullopt;
                }

                if (event->type == SynchronizationEvent)
                {
                    event->signaled = false;
                }

                return wait_state::signaled;
            }

            case handle_types::mutant: {
                auto* mutant = c.mutants.get(h);
                if (!mutant)
                {
                    return std::nullopt;
                }

                const auto acquired = mutant->try_lock(current_thread_id);
                if (!acquired.has_value())
                {
                    return std::nullopt;
                }

                return *acquired ? wait_state::abandoned : wait_state::signaled;
            }

            case handle_types::timer:
                return wait_state::signaled; // TODO

            case handle_types::semaphore: {
                auto* semaphore = c.semaphores.get(h);
                if (!semaphore || !semaphore->try_lock())
                {
                    return std::nullopt;
                }

                return wait_state::signaled;
            }

            case handle_types::port: {
                auto* port = c.ports.get(h);
                if (!port || !port->has_pending_reply())
                {
                    return std::nullopt;
                }

                return wait_state::signaled;
            }

            case handle_types::thread: {
                const auto* thread = c.threads.get(h);
                if (!thread || !thread->is_terminated())
                {
                    return std::nullopt;
                }

                return wait_state::signaled;
            }

            default:
                throw std::runtime_error("Bad object: " + std::to_string(h.value.type));
            }
        }

        uint32_t get_message_queue_status_bits(const msg& queued_message)
        {
            switch (queued_message.message)
            {
            case WM_TIMER:
                return QS_TIMER;

            case WM_PAINT:
                return QS_PAINT;

            case WM_KEYDOWN:
            case WM_KEYUP:
                return QS_KEY;

            default:
                return QS_POSTMESSAGE | QS_ALLPOSTMESSAGE;
            }
        }

        template <typename F>
        void for_each_queue_status_bit(uint32_t mask, F&& callback)
        {
            while (mask != 0)
            {
                const auto bit = mask & (0 - mask);
                const auto index = std::countr_zero(bit);
                callback(bit, index);
                mask &= ~bit;
            }
        }
    }

    emulator_thread::emulator_thread(memory_manager& memory, const process_context& context, const uint64_t start_address,
                                     const uint64_t argument, const uint64_t stack_size, const uint32_t create_flags, const uint32_t id,
                                     const bool initial_thread)
        : memory_ptr(&memory),
          // stack_size(page_align_up(std::max(stack_size, static_cast<uint64_t>(STACK_SIZE)))),
          start_address(start_address),
          argument(argument),
          id(id),
          create_flags(create_flags),
          last_registers(context.default_register_set)
    {
        this->suspended = create_flags & THREAD_CREATE_FLAGS_CREATE_SUSPENDED;

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
                teb_obj.DeallocationStack = this->stack_base;
                teb_obj.NtTib.StackLimit = this->stack_base;
                teb_obj.NtTib.StackBase = this->stack_base + this->stack_size;
                teb_obj.NtTib.Self = this->teb64->value();
                teb_obj.CurrentLocale = 0x409;
                teb_obj.ProcessEnvironmentBlock = context.peb64.value();
                teb_obj.SameTebFlags.InitialThread = initial_thread;
                teb_obj.SameTebFlags.SkipThreadAttach = (create_flags & THREAD_CREATE_FLAGS_SKIP_THREAD_ATTACH) ? 1 : 0;
                teb_obj.SameTebFlags.LoaderWorker = (create_flags & THREAD_CREATE_FLAGS_LOADER_WORKER) ? 1 : 0;
                teb_obj.SameTebFlags.SkipLoaderInit = (create_flags & THREAD_CREATE_FLAGS_SKIP_LOADER_INIT) ? 1 : 0;
                teb_obj.StaticUnicodeString.Length = 0;
                teb_obj.StaticUnicodeString.MaximumLength = sizeof(teb_obj.StaticUnicodeBuffer);
                teb_obj.StaticUnicodeString.Buffer = this->teb64->value() + offsetof(TEB64, StaticUnicodeBuffer);

                const auto desktop_info_obj = this->gs_segment->reserve<USER_DESKTOPINFO>();
                desktop_info_obj.access([&](USER_DESKTOPINFO& info) {
                    if (const auto* wnd = context.windows.get(context.default_desktop_window_handle))
                    {
                        info.spwndDesktop = wnd->guest.value();
                    }
                });
                teb_obj.Win32ClientInfo.arr[4] = desktop_info_obj.value();
            });

            return;
        }

        // Default native size of wow64 is 256KB
        this->stack_size = WOW64_NATIVE_STACK_SIZE;
        this->wow64_stack_size = page_align_up(std::max(stack_size, static_cast<uint64_t>(STACK_SIZE)));

        // Set the default memory allocation address to the specified 32-bit address
        memory.set_default_allocation_address(DEFAULT_ALLOCATION_ADDRESS_32BIT);

        // Calculate required GS segment size for WOW64 (64-bit TEB + 32-bit TEB)
        constexpr auto teb64_size = sizeof(TEB64);
        constexpr auto teb32_size = sizeof(TEB32); // 4120 bytes
        constexpr auto wow_teb_offset = page_align_up(teb64_size);
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

        const uint64_t wow64_cpureserved_base = this->stack_base + this->stack_size - sizeof(WOW64_CPURESERVED) - 0x1030;

        // Initialize 64-bit TEB first
        this->teb64->access([&](TEB64& teb_obj) {
            // Skips GetCurrentNlsCache
            // This hack can be removed once this is fixed:
            // https://github.com/momo5502/emulator/issues/128
            reinterpret_cast<uint8_t*>(&teb_obj)[0x179C] = 1;

            teb_obj.ClientId.UniqueProcess = 1ul;
            teb_obj.ClientId.UniqueThread = static_cast<uint64_t>(this->id);

            // Native 64-bit stack
            teb_obj.DeallocationStack = this->stack_base;
            teb_obj.NtTib.StackLimit = this->stack_base;
            teb_obj.NtTib.StackBase = wow64_cpureserved_base;
            teb_obj.NtTib.Self = this->teb64->value();
            teb_obj.CurrentLocale = 0x409;

            teb_obj.ProcessEnvironmentBlock = context.peb64.value();
            teb_obj.SameTebFlags.InitialThread = initial_thread;
            teb_obj.SameTebFlags.SkipThreadAttach = (create_flags & THREAD_CREATE_FLAGS_SKIP_THREAD_ATTACH) ? 1 : 0;
            teb_obj.SameTebFlags.LoaderWorker = (create_flags & THREAD_CREATE_FLAGS_LOADER_WORKER) ? 1 : 0;
            teb_obj.SameTebFlags.SkipLoaderInit = (create_flags & THREAD_CREATE_FLAGS_SKIP_LOADER_INIT) ? 1 : 0;
            teb_obj.StaticUnicodeString.Length = 0;
            teb_obj.StaticUnicodeString.MaximumLength = sizeof(teb_obj.StaticUnicodeBuffer);
            teb_obj.StaticUnicodeString.Buffer = this->teb64->value() + offsetof(TEB64, StaticUnicodeBuffer);

            // Set WowTebOffset to point to 32-bit TEB offset
            teb_obj.WowTebOffset = static_cast<int32_t>(wow_teb_offset); // 0x2000

            // Set TLS slot [1] to point to WOW64_CPURESERVED structure
            teb_obj.TlsSlots.arr[1 /* WOW64_TLS_CPURESERVED */] = wow64_cpureserved_base;

            // Note: TLS slot [10] (WOW64_INFO_PTR) will be set by wow64.dll during initialization

            // WoW64 32-bit user32 reads its CLIENTINFO from the 64-bit TEB (pci = TEB32 - WowTebOffset = TEB64),
            // so it needs the desktop-info pointer published here just like the native path does. Without it,
            // user32!GetDesktopWindow dereferences a null Win32ClientInfo[4] on the first UI call of every thread.
            const auto desktop_info_obj = this->gs_segment->reserve<USER_DESKTOPINFO>();
            desktop_info_obj.access([&](USER_DESKTOPINFO& info) {
                if (const auto* wnd = context.windows.get(context.default_desktop_window_handle))
                {
                    info.spwndDesktop = wnd->guest.value();
                }
            });
            teb_obj.Win32ClientInfo.arr[4] = desktop_info_obj.value();
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

            teb32_obj.WowTebOffset = -static_cast<int32_t>(wow_teb_offset);
            teb32_obj.InitialThread = initial_thread;
            teb32_obj.SkipThreadAttach = (create_flags & THREAD_CREATE_FLAGS_SKIP_THREAD_ATTACH) ? 1 : 0;
            teb32_obj.LoaderWorker = (create_flags & THREAD_CREATE_FLAGS_LOADER_WORKER) ? 1 : 0;
            teb32_obj.SkipLoaderInit = (create_flags & THREAD_CREATE_FLAGS_SKIP_LOADER_INIT) ? 1 : 0;
            teb32_obj.StaticUnicodeString.Length = 0;
            teb32_obj.StaticUnicodeString.MaximumLength = sizeof(teb32_obj.StaticUnicodeBuffer);
            teb32_obj.StaticUnicodeString.Buffer = static_cast<uint32_t>(teb32_addr + offsetof(TEB32, StaticUnicodeBuffer));

            // Note: CurrentLocale and other fields will be initialized by WOW64 runtime
        });

        this->teb64->access([&](TEB64& teb_obj) {
            // teb64.ExceptionList initially points to teb32
            teb_obj.NtTib.ExceptionList = teb32_addr;
        });

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

            // Seed WOW64 floating-point state with sane defaults
            memset(&ctx.Context.FloatSave, 0, sizeof(ctx.Context.FloatSave));
            ctx.Context.FloatSave.ControlWord = 0x037F;
            ctx.Context.FloatSave.StatusWord = 0;
            ctx.Context.FloatSave.TagWord = 0xFFFF;
            ctx.Context.FloatSave.Cr0NpxState = 0;

            XMM_SAVE_AREA32 xmm_state{};
            xmm_state.ControlWord = 0x037F;
            xmm_state.StatusWord = 0;
            // FXSAVE abridged x87 tag: 1 bit per register, 1=valid, 0=empty. A fresh FPU has an empty stack,
            // so this must be 0x00. 0xFF marks all 8 registers valid (a full stack), which makes the first
            // guest x87 FLD overflow and produce the x87 indefinite (NaN) under hardware (WHP) execution.
            xmm_state.TagWord = 0x00;
            xmm_state.MxCsr = 0x1F80;
            xmm_state.MxCsr_Mask = 0xFFFFFFFF;

            memset(&ctx.Context.ExtendedRegisters, 0, sizeof(ctx.Context.ExtendedRegisters));
            static_assert(sizeof(xmm_state) <= sizeof(ctx.Context.ExtendedRegisters));
            memcpy(ctx.Context.ExtendedRegisters, &xmm_state, sizeof(xmm_state));
        });
    }

    void emulator_thread::mark_as_ready(const NTSTATUS status)
    {
        this->pending_status = status;
        this->await_time = {};
        this->await_objects = {};
        this->await_msg = {};
        this->await_msg_mask = {};
        this->await_io_completion = {};

        // TODO: Find out if this is correct
        if (this->waiting_for_alert)
        {
            this->alerted = false;
        }

        this->waiting_for_alert = false;
    }

    user_timer* emulator_thread::find_user_timer(const hwnd hwnd, const uint64_t timer_id)
    {
        const auto it = this->user_timers.find(user_timer_key{
            .hwnd = hwnd,
            .timer_id = timer_id,
        });

        if (it == this->user_timers.end())
        {
            return nullptr;
        }

        return &it->second;
    }

    user_timer& emulator_thread::create_user_timer(process_context& process, const hwnd hwnd, uint64_t timer_id, const uint64_t timer_proc,
                                                   const std::chrono::milliseconds interval,
                                                   const std::chrono::steady_clock::time_point now)
    {
        if (hwnd == 0)
        {
            timer_id = this->next_user_timer_id;
            while (this->user_timers.contains(user_timer_key{
                .hwnd = 0,
                .timer_id = timer_id,
            }))
            {
                ++timer_id;
                if (timer_id == 0)
                {
                    timer_id = 1;
                }
            }
            this->next_user_timer_id = timer_id + 1;
            if (this->next_user_timer_id == 0)
            {
                this->next_user_timer_id = 1;
            }
        }
        else if (const auto* window = process.windows.get(hwnd); window->thread_id != this->id)
        {
            throw std::runtime_error("Attempted to create a user timer in a thread that doesn't own the target window");
        }

        user_timer timer{};
        timer.hwnd = hwnd;
        timer.timer_id = timer_id;
        timer.timer_proc = timer_proc;
        timer.interval = interval;
        timer.due_time = now + interval;

        const auto [it, inserted] = this->user_timers.emplace(
            user_timer_key{
                .hwnd = hwnd,
                .timer_id = timer_id,
            },
            std::move(timer));

        if (!inserted)
        {
            throw std::runtime_error("User timer already exists");
        }

        return it->second;
    }

    bool emulator_thread::delete_user_timer(const hwnd hwnd, const uint64_t timer_id)
    {
        const auto it = this->user_timers.find(user_timer_key{
            .hwnd = hwnd,
            .timer_id = timer_id,
        });

        if (it == this->user_timers.end())
        {
            return false;
        }

        this->user_timers.erase(it);
        return true;
    }

    bool emulator_thread::synthesize_due_user_timer(utils::clock& clock, const hwnd hwnd_filter, const UINT filter_min,
                                                    const UINT filter_max)
    {
        const auto now = clock.steady_now();
        for (auto& user_timer : this->user_timers | std::views::values)
        {
            if (!user_timer.due_time.has_value() || user_timer.due_time.value() > now)
            {
                continue;
            }

            msg timer_message{};
            timer_message.window = user_timer.hwnd;
            timer_message.message = WM_TIMER;
            timer_message.wParam = user_timer.timer_id;
            timer_message.lParam = user_timer.timer_proc;

            if (hwnd_filter != 0 && hwnd_filter != static_cast<hwnd>(-1) && timer_message.window != hwnd_filter)
            {
                continue;
            }

            if (hwnd_filter == static_cast<hwnd>(-1) && timer_message.window != 0)
            {
                continue;
            }

            if ((filter_min != 0 || filter_max != 0) && (timer_message.message < filter_min || timer_message.message > filter_max))
            {
                continue;
            }

            this->post_message(timer_message);
            user_timer.due_time = now + user_timer.interval;
            return true;
        }

        return false;
    }

    uint32_t emulator_thread::get_message_queue_status(utils::clock& clock)
    {
        if (this->await_msg_mask && (*this->await_msg_mask & QS_TIMER) != 0)
        {
            (void)this->synthesize_due_user_timer(clock);
        }

        return this->message_queue_status_bits;
    }

    namespace
    {
        // GetMessage(hWnd) retrieves messages for hWnd and all of its children (IsChild semantics),
        // so a message targeted at a child control must match a filter naming any of its ancestors.
        bool window_matches_filter(const process_context& process, const hwnd target, const hwnd filter)
        {
            auto current = target;
            while (current != 0)
            {
                if (current == filter)
                {
                    return true;
                }

                const auto* win = process.windows.get(current);
                if (!win)
                {
                    break;
                }

                current = win->parent_handle;
            }

            return false;
        }
    }

    std::optional<msg> emulator_thread::peek_pending_message(const process_context& process, utils::clock& clock, hwnd hwnd_filter,
                                                             UINT filter_min, UINT filter_max, bool remove)
    {
        (void)this->synthesize_due_user_timer(clock, hwnd_filter, filter_min, filter_max);

        for (auto it = message_queue.begin(); it != message_queue.end(); ++it)
        {
            if (hwnd_filter == static_cast<hwnd>(-1))
            {
                if (it->window != 0)
                {
                    continue;
                }
            }
            else if (hwnd_filter != 0 && !window_matches_filter(process, it->window, hwnd_filter))
            {
                continue;
            }

            if ((filter_min != 0 || filter_max != 0) && (it->message < filter_min || it->message > filter_max))
            {
                continue;
            }

            auto msg = *it;
            if (remove)
            {
                const auto removed_bits = get_message_queue_status_bits(msg);

                for_each_queue_status_bit(removed_bits, [this](const uint32_t bit, const size_t index) {
                    if (this->message_queue_status_bit_counts[index] <= 1)
                    {
                        this->message_queue_status_bit_counts[index] = 0;
                        this->message_queue_status_bits &= ~bit;
                    }
                    else
                    {
                        --this->message_queue_status_bit_counts[index];
                    }
                });

                message_queue.erase(it);
            }
            return msg;
        }

        return std::nullopt;
    }

    void emulator_thread::post_message(const msg& msg)
    {
        const auto bits = get_message_queue_status_bits(msg);

        for_each_queue_status_bit(bits, [this](const uint32_t /*bit*/, const size_t index) { //
            ++this->message_queue_status_bit_counts[index];
        });

        this->message_queue_status_bits |= bits;
        this->queue_status_changed_bits |= bits;
        message_queue.push_back(msg);
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

        const auto complete_if_timed_out = [&](const NTSTATUS status) {
            if (!this->is_await_time_over(clock) || this->has_pending_alertable_apc())
            {
                return false;
            }

            this->mark_as_ready(status);
            return true;
        };

        if (this->waiting_for_alert)
        {
            if (this->alerted)
            {
                this->mark_as_ready(STATUS_ALERTED);
                return true;
            }

            return complete_if_timed_out(STATUS_TIMEOUT);
        }

        if (this->await_msg_mask.has_value() || !this->await_objects.empty())
        {
            bool all_signaled = this->await_objects.empty();
            std::optional<uint32_t> abandoned_index{};

            if (!this->await_objects.empty())
            {
                all_signaled = true;
                for (uint32_t i = 0; i < this->await_objects.size(); ++i)
                {
                    const auto& obj = this->await_objects[i];

                    const auto state = observe_object_signal(process, obj, this->id);
                    const auto signaled = state != wait_state::not_signaled;
                    all_signaled &= signaled;

                    if (state == wait_state::abandoned && !abandoned_index.has_value())
                    {
                        abandoned_index = i;
                    }

                    if (signaled && this->await_any)
                    {
                        const auto consumed_state = consume_object_signal(process, obj, this->id);
                        if (!consumed_state.has_value())
                        {
                            throw std::runtime_error("Failed to consume object signal!");
                        }

                        if (this->await_msg_mask.has_value())
                        {
                            this->queue_status_changed_bits |= QS_POSTMESSAGE | QS_ALLPOSTMESSAGE;
                        }

                        this->mark_as_ready(*consumed_state == wait_state::abandoned ? (STATUS_ABANDONED_WAIT_0 + i) : (STATUS_WAIT_0 + i));
                        return true;
                    }
                }
            }

            bool message_ready = false;
            if (this->await_msg_mask.has_value())
            {
                const auto current_message_bits = this->get_message_queue_status(clock);
                message_ready = (current_message_bits & *this->await_msg_mask) != 0;
            }

            if (this->await_msg_mask.has_value() && message_ready && (this->await_any || this->await_objects.empty()))
            {
                this->mark_as_ready(static_cast<NTSTATUS>(STATUS_WAIT_0 + this->await_objects.size()));
                return true;
            }

            if (!this->await_any && all_signaled && (!this->await_msg_mask.has_value() || message_ready))
            {
                for (const auto& obj : this->await_objects)
                {
                    const auto consumed_state = consume_object_signal(process, obj, this->id);
                    if (!consumed_state.has_value())
                    {
                        throw std::runtime_error("Failed to consume object signal!");
                    }
                }

                this->mark_as_ready(abandoned_index.has_value() ? (STATUS_ABANDONED_WAIT_0 + *abandoned_index) : STATUS_SUCCESS);
                return true;
            }

            return complete_if_timed_out(STATUS_TIMEOUT);
        }

        if (this->await_time.has_value())
        {
            return complete_if_timed_out(STATUS_SUCCESS);
        }

        if (this->await_io_completion.has_value())
        {
            auto timeout_expired = [&](const pending_io_completion_wait& wait) {
                constexpr auto infinite = std::chrono::steady_clock::time_point::min();
                return wait.timeout.has_value() && wait.timeout.value() != infinite && wait.timeout.value() < clock.steady_now();
            };

            const auto& wait = *this->await_io_completion;
            if (!process.io_completions.get(wait.io_completion_handle))
            {
                this->mark_as_ready(STATUS_INVALID_HANDLE);
                return true;
            }

            if (wait.type == io_completion_wait_type::remove_single)
            {
                io_completion_message message{};
                if (io_completion_wait::dequeue_io_completion_message(process, wait.io_completion_handle, message))
                {
                    emulator_object<emulator_pointer>{*this->memory_ptr, wait.key_context_ptr}.write_if_valid(message.key_context);
                    emulator_object<emulator_pointer>{*this->memory_ptr, wait.apc_context_ptr}.write_if_valid(message.apc_context);
                    emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>{*this->memory_ptr, wait.io_status_block_ptr}.write_if_valid(
                        message.io_status_block);

                    this->mark_as_ready(STATUS_SUCCESS);
                    return true;
                }

                if (timeout_expired(wait) && !this->has_pending_alertable_apc())
                {
                    this->mark_as_ready(STATUS_TIMEOUT);
                    return true;
                }

                return false;
            }

            if (wait.type == io_completion_wait_type::remove_multiple)
            {
                const auto removed = io_completion_wait::dequeue_io_completion_entries(
                    process, wait.io_completion_handle,
                    emulator_object<FILE_IO_COMPLETION_INFORMATION<EmulatorTraits<Emu64>>>{*this->memory_ptr, wait.completion_entries_ptr},
                    wait.max_entries);

                if (removed > 0)
                {
                    emulator_object<ULONG>{*this->memory_ptr, wait.entries_removed_ptr}.write_if_valid(removed);
                    this->mark_as_ready(STATUS_SUCCESS);
                    return true;
                }

                if (timeout_expired(wait) && !this->has_pending_alertable_apc())
                {
                    emulator_object<ULONG>{*this->memory_ptr, wait.entries_removed_ptr}.write_if_valid(0);
                    this->mark_as_ready(STATUS_TIMEOUT);
                    return true;
                }

                return false;
            }

            this->mark_as_ready(STATUS_INVALID_PARAMETER);
            return true;
        }

        if (this->await_msg.has_value())
        {
            if (const auto m = this->peek_pending_message(process, clock, this->await_msg->hwnd_filter, this->await_msg->filter_min,
                                                          this->await_msg->filter_max, true))
            {
                this->await_msg->message.write(*m);

                uint64_t active_handle = 0;
                uint64_t active_window_ptr = 0;
                if (const auto* win = process.windows.get(m->window))
                {
                    active_handle = win->handle;
                    active_window_ptr = win->guest.value();
                }

                if (this->teb64)
                {
                    this->teb64->access([&](TEB64& teb) {
                        teb.Win32ClientInfo.arr[8] = active_handle;
                        teb.Win32ClientInfo.arr[9] = active_window_ptr;
                    });
                }

                if (process.is_wow64_process && this->teb32)
                {
                    uint32_t active_handle32 = 0;
                    uint32_t active_window_ptr32 = 0;

                    if (active_handle <= std::numeric_limits<uint32_t>::max())
                    {
                        active_handle32 = static_cast<uint32_t>(active_handle);
                    }

                    if (active_window_ptr <= std::numeric_limits<uint32_t>::max())
                    {
                        active_window_ptr32 = static_cast<uint32_t>(active_window_ptr);
                    }

                    this->teb32->access([&](TEB32& teb) {
                        teb.Win32ClientInfo[8] = active_handle32;
                        teb.Win32ClientInfo[9] = active_window_ptr32;
                    });
                }

                this->mark_as_ready(m->message != WM_QUIT ? TRUE : FALSE);
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
                    ctx.Context.Ebx = static_cast<uint32_t>(this->argument);
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

    void emulator_thread::refresh_execution_context(x86_64_emulator& emu) const
    {
        (void)emu;

        if (this->teb32.has_value())
        {
            // Refresh GDT entry for FS selector on context switch
            setup_wow64_fs_segment(*this->memory_ptr, this->teb32->value());
        }
    }

    callback_frame::callback_frame() = default;

    callback_frame::callback_frame(callback_id callback_id, std::unique_ptr<completion_state> completion_state)
    {
        this->handler_id = callback_id;
        this->state = std::move(completion_state);
    }

    callback_frame::callback_frame(callback_frame&& obj) noexcept = default;
    callback_frame& callback_frame::operator=(callback_frame&& obj) noexcept = default;

    callback_frame::~callback_frame() = default;

    void callback_frame::save_registers(x86_64_emulator& emu)
    {
        if (this->rip != 0)
        {
            throw std::runtime_error("Attempt to overwrite callback frame register snapshot");
        }

        this->rip = emu.reg(x86_register::rip);
        this->rbp = emu.reg(x86_register::rbp);
        this->rdi = emu.reg(x86_register::rdi);
        this->rsi = emu.reg(x86_register::rsi);
        this->rsp = emu.reg(x86_register::rsp);
        this->r10 = emu.reg(x86_register::r10);
        this->r11 = emu.reg(x86_register::r11);
        this->r12 = emu.reg(x86_register::r12);
        this->r13 = emu.reg(x86_register::r13);
        this->r14 = emu.reg(x86_register::r14);
        this->r15 = emu.reg(x86_register::r15);
        this->rax = emu.reg(x86_register::rax);
        this->rbx = emu.reg(x86_register::rbx);
        this->rcx = emu.reg(x86_register::rcx);
        this->rdx = emu.reg(x86_register::rdx);
        this->r8 = emu.reg(x86_register::r8);
        this->r9 = emu.reg(x86_register::r9);
        this->cs = emu.reg<uint16_t>(x86_register::cs);
        this->ss = emu.reg<uint16_t>(x86_register::ss);
        this->ds = emu.reg<uint16_t>(x86_register::ds);
        this->es = emu.reg<uint16_t>(x86_register::es);
        this->fs = emu.reg<uint16_t>(x86_register::fs);
        this->gs = emu.reg<uint16_t>(x86_register::gs);
    }

    void callback_frame::restore_registers(x86_64_emulator& emu) const
    {
        if (this->rip == 0)
        {
            throw std::runtime_error("Attempt to restore registers from an uninitialized callback frame");
        }

        emu.reg<uint16_t>(x86_register::cs, this->cs);
        emu.reg<uint16_t>(x86_register::ss, this->ss);
        emu.reg<uint16_t>(x86_register::ds, this->ds);
        emu.reg<uint16_t>(x86_register::es, this->es);
        emu.reg<uint16_t>(x86_register::fs, this->fs);
        emu.reg<uint16_t>(x86_register::gs, this->gs);
        emu.reg(x86_register::rip, this->rip);
        emu.reg(x86_register::rbp, this->rbp);
        emu.reg(x86_register::rdi, this->rdi);
        emu.reg(x86_register::rsi, this->rsi);
        emu.reg(x86_register::rsp, this->rsp);
        emu.reg(x86_register::r10, this->r10);
        emu.reg(x86_register::r11, this->r11);
        emu.reg(x86_register::r12, this->r12);
        emu.reg(x86_register::r13, this->r13);
        emu.reg(x86_register::r14, this->r14);
        emu.reg(x86_register::r15, this->r15);
        emu.reg(x86_register::rax, this->rax);
        emu.reg(x86_register::rbx, this->rbx);
        emu.reg(x86_register::rcx, this->rcx);
        emu.reg(x86_register::rdx, this->rdx);
        emu.reg(x86_register::r8, this->r8);
        emu.reg(x86_register::r9, this->r9);
    }

    void callback_frame::serialize(utils::buffer_serializer& buffer) const
    {
        buffer.write(this->handler_id);
        buffer.write(this->rip);
        buffer.write(this->rbp);
        buffer.write(this->rdi);
        buffer.write(this->rsi);
        buffer.write(this->rsp);
        buffer.write(this->r10);
        buffer.write(this->r11);
        buffer.write(this->r12);
        buffer.write(this->r13);
        buffer.write(this->r14);
        buffer.write(this->r15);
        buffer.write(this->rax);
        buffer.write(this->rbx);
        buffer.write(this->rcx);
        buffer.write(this->rdx);
        buffer.write(this->r8);
        buffer.write(this->r9);
        buffer.write(this->cs);
        buffer.write(this->ss);
        buffer.write(this->ds);
        buffer.write(this->es);
        buffer.write(this->fs);
        buffer.write(this->gs);

        buffer.write(static_cast<bool>(this->state));
        if (this->state)
        {
            buffer.write(*this->state);
        }
    }

    void callback_frame::deserialize(utils::buffer_deserializer& buffer)
    {
        buffer.read(this->handler_id);
        buffer.read(this->rip);
        buffer.read(this->rbp);
        buffer.read(this->rdi);
        buffer.read(this->rsi);
        buffer.read(this->rsp);
        buffer.read(this->r10);
        buffer.read(this->r11);
        buffer.read(this->r12);
        buffer.read(this->r13);
        buffer.read(this->r14);
        buffer.read(this->r15);
        buffer.read(this->rax);
        buffer.read(this->rbx);
        buffer.read(this->rcx);
        buffer.read(this->rdx);
        buffer.read(this->r8);
        buffer.read(this->r9);
        buffer.read(this->cs);
        buffer.read(this->ss);
        buffer.read(this->ds);
        buffer.read(this->es);
        buffer.read(this->fs);
        buffer.read(this->gs);

        bool has_state{};
        buffer.read(has_state);

        if (has_state)
        {
            this->state = syscall_dispatcher::create_completion_state(this->handler_id);
            if (!this->state)
            {
                throw std::runtime_error(
                    "Serialized data indicates a completion state is present, but state creation failed for this callback id");
            }
            buffer.read(*this->state);
        }
    }

} // namespace sogen
