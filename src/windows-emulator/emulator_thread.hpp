#pragma once

#include "handles.hpp"
#include "emulator_utils.hpp"
#include "memory_manager.hpp"

#include <functional>

#include "process_manager.hpp"
#include <utils/moved_marker.hpp>
#include <platform/arm64_context.hpp>

namespace sogen
{

    struct completion_state;
    struct process_context;
    struct vcpu_context;
    struct windows_arch_abi;

    // A raw-input payload is keyed by the HRAWINPUT token posted in WM_INPUT's lParam.
    struct raw_input_payload
    {
        bool keyboard{}; // false = mouse, true = keyboard
        int32_t dx{};    // mouse relative motion
        int32_t dy{};
        uint16_t mouse_buttons{};     // RI_MOUSE_* button transition flags
        uint16_t mouse_button_data{}; // wheel delta for RI_MOUSE_WHEEL/RI_MOUSE_HWHEEL
        uint16_t vkey{};
        uint16_t scan_code{};   // keyboard scan code
        uint32_t key_message{}; // corresponding WM_KEY* or WM_SYSKEY* message
        bool key_extended{};    // true when the key's scan code has an E0 prefix (lParam bit 24)
    };

    struct user_timer_key
    {
        sogen::hwnd hwnd{};
        uint64_t timer_id{};

        auto operator<=>(const user_timer_key&) const = default;
    };

    struct user_timer
    {
        std::u16string name{};
        sogen::hwnd hwnd{};
        uint64_t timer_id{};
        uint64_t timer_proc{};
        bool is_system{};
        std::optional<std::chrono::steady_clock::time_point> due_time{};
        std::chrono::steady_clock::duration interval{};

        void serialize(utils::buffer_serializer& buffer) const
        {
            buffer.write(this->name);
            buffer.write(this->hwnd);
            buffer.write(this->timer_id);
            buffer.write(this->timer_proc);
            buffer.write(this->is_system);
            buffer.write_optional(this->due_time);
            buffer.write(this->interval);
        }

        void deserialize(utils::buffer_deserializer& buffer)
        {
            buffer.read(this->name);
            buffer.read(this->hwnd);
            buffer.read(this->timer_id);
            buffer.read(this->timer_proc);
            buffer.read(this->is_system);
            buffer.read_optional(this->due_time);
            buffer.read(this->interval);
        }
    };

    struct user_thread_hook
    {
        uint64_t handle{};
        uint64_t proc{};
        bool ansi{};

        void serialize(utils::buffer_serializer& buffer) const
        {
            buffer.write(this->handle);
            buffer.write(this->proc);
            buffer.write(this->ansi);
        }

        void deserialize(utils::buffer_deserializer& buffer)
        {
            buffer.read(this->handle);
            buffer.read(this->proc);
            buffer.read(this->ansi);
        }
    };

    struct user_low_level_hook
    {
        uint64_t handle{};
        int id{};
        uint64_t proc{};

        void serialize(utils::buffer_serializer& buffer) const
        {
            buffer.write(this->handle);
            buffer.write(this->id);
            buffer.write(this->proc);
        }

        void deserialize(utils::buffer_deserializer& buffer)
        {
            buffer.read(this->handle);
            buffer.read(this->id);
            buffer.read(this->proc);
        }
    };

    struct pending_apc
    {
        uint32_t flags{};
        uint64_t apc_routine{};
        uint64_t apc_argument1{};
        uint64_t apc_argument2{};
        uint64_t apc_argument3{};

        // For a WoW64 I/O completion APC: apc_argument2 is the caller's 32-bit I/O status block. The kernel writes
        // it as it dispatches the completion APC, so whatever the caller left there while the request was pending
        // is superseded; re-stamp it right before the APC runs.
        bool restamp_io_status_block{false};
        int32_t io_status{};
        uint32_t io_information{};

        void serialize(utils::buffer_serializer& buffer) const
        {
            buffer.write(this->flags);
            buffer.write(this->apc_routine);
            buffer.write(this->apc_argument1);
            buffer.write(this->apc_argument2);
            buffer.write(this->apc_argument3);
            buffer.write(this->restamp_io_status_block);
            buffer.write(this->io_status);
            buffer.write(this->io_information);
        }

        void deserialize(utils::buffer_deserializer& buffer)
        {
            buffer.read(this->flags);
            buffer.read(this->apc_routine);
            buffer.read(this->apc_argument1);
            buffer.read(this->apc_argument2);
            buffer.read(this->apc_argument3);
            buffer.read(this->restamp_io_status_block);
            buffer.read(this->io_status);
            buffer.read(this->io_information);
        }
    };

    struct pending_msg
    {
        emulator_object<msg> message;
        hwnd hwnd_filter{};
        UINT filter_min{};
        UINT filter_max{};

        pending_msg(memory_interface& memory)
            : message(memory)
        {
        }

        pending_msg(emulator_object<msg> message, hwnd hwnd_filter, UINT filter_min, UINT filter_max)
            : message(message),
              hwnd_filter(hwnd_filter),
              filter_min(filter_min),
              filter_max(filter_max)
        {
        }

        void serialize(utils::buffer_serializer& buffer) const
        {
            buffer.write(this->message);
            buffer.write(this->hwnd_filter);
            buffer.write(this->filter_min);
            buffer.write(this->filter_max);
        }

        void deserialize(utils::buffer_deserializer& buffer)
        {
            buffer.read(this->message);
            buffer.read(this->hwnd_filter);
            buffer.read(this->filter_min);
            buffer.read(this->filter_max);
        }
    };

    struct sent_window_message
    {
        uint32_t sender_thread_id{};
        uint64_t sender_callback_depth{};
        hwnd window{};
        UINT message{};
        uint64_t w_param{};
        uint64_t l_param{};
        uint64_t scratch_text{};
        BOOL ansi{};
        bool replied{};

        void serialize(utils::buffer_serializer& buffer) const
        {
            buffer.write(this->sender_thread_id);
            buffer.write(this->sender_callback_depth);
            buffer.write(this->window);
            buffer.write(this->message);
            buffer.write(this->w_param);
            buffer.write(this->l_param);
            buffer.write(this->scratch_text);
            buffer.write(this->ansi);
            buffer.write(this->replied);
        }

        void deserialize(utils::buffer_deserializer& buffer)
        {
            buffer.read(this->sender_thread_id);
            buffer.read(this->sender_callback_depth);
            buffer.read(this->window);
            buffer.read(this->message);
            buffer.read(this->w_param);
            buffer.read(this->l_param);
            buffer.read(this->scratch_text);
            buffer.read(this->ansi);
            buffer.read(this->replied);
        }
    };

    struct sent_message_wait
    {
        uint64_t callback_depth{};
        uint64_t syscall_ip{};
        uint64_t syscall_number{};
        std::optional<uint64_t> result{};

        void serialize(utils::buffer_serializer& buffer) const
        {
            buffer.write(this->callback_depth);
            buffer.write(this->syscall_ip);
            buffer.write(this->syscall_number);
            buffer.write_optional(this->result);
        }

        void deserialize(utils::buffer_deserializer& buffer)
        {
            buffer.read(this->callback_depth);
            buffer.read(this->syscall_ip);
            buffer.read(this->syscall_number);
            buffer.read_optional(this->result);
        }
    };

    enum class io_completion_wait_type : uint8_t
    {
        remove_single = 0,
        remove_multiple = 1,
    };

    // An object a thread waits for, as its store gave it for reference (handle_store::get_cached).
    struct cached_wait_object
    {
        object_reference reference{};
        ref_counted_object* object{};
        uint64_t generation{};
    };

    struct pending_io_completion_wait
    {
        object_reference io_completion{};
        io_completion_wait_type type{io_completion_wait_type::remove_single};
        emulator_pointer key_context_ptr{};
        emulator_pointer apc_context_ptr{};
        emulator_pointer io_status_block_ptr{};
        emulator_pointer completion_entries_ptr{};
        emulator_pointer entries_removed_ptr{};
        ULONG max_entries{};
        std::optional<std::chrono::steady_clock::time_point> timeout{};
        // The worker factory a NtWaitForWorkViaWorkerFactory wait takes work for; none for NtRemoveIoCompletion(Ex). Keyed by
        // object identity so the more-work check resolves it through whichever handle value the wait was entered with.
        object_reference worker_factory{};
        bool timeout_absolute{};
        // The port as last looked up (handle_store::get_cached). Not serialized; a restored wait looks it up again.
        ref_counted_object* io_completion_object{};
        uint64_t io_completion_generation{};

        void serialize(utils::buffer_serializer& buffer) const
        {
            buffer.write(static_cast<uint8_t>(this->type));
            buffer.write(this->io_completion);
            buffer.write(this->key_context_ptr);
            buffer.write(this->apc_context_ptr);
            buffer.write(this->io_status_block_ptr);
            buffer.write(this->completion_entries_ptr);
            buffer.write(this->entries_removed_ptr);
            buffer.write(this->max_entries);
            buffer.write_optional(this->timeout);
            buffer.write(this->worker_factory);
            buffer.write(this->timeout_absolute);
        }

        void deserialize(utils::buffer_deserializer& buffer)
        {
            const auto raw_type = buffer.read<uint8_t>();
            this->type = static_cast<io_completion_wait_type>(raw_type);
            buffer.read(this->io_completion);
            buffer.read(this->key_context_ptr);
            buffer.read(this->apc_context_ptr);
            buffer.read(this->io_status_block_ptr);
            buffer.read(this->completion_entries_ptr);
            buffer.read(this->entries_removed_ptr);
            buffer.read(this->max_entries);
            buffer.read_optional(this->timeout);
            buffer.read(this->worker_factory);
            buffer.read(this->timeout_absolute);
        }
    };

    struct pending_debug_event_wait
    {
        // The waited debug object by identity, so closing the handle the wait was entered with (a duplicate keeps it alive)
        // does not detach the wait.
        object_reference debug_object{};
        emulator_pointer state_change{};
        std::optional<std::chrono::steady_clock::time_point> timeout{};
        bool timeout_absolute{};

        void serialize(utils::buffer_serializer& buffer) const
        {
            buffer.write(this->debug_object);
            buffer.write(this->state_change);
            buffer.write_optional(this->timeout);
            buffer.write(this->timeout_absolute);
        }

        void deserialize(utils::buffer_deserializer& buffer)
        {
            buffer.read(this->debug_object);
            buffer.read(this->state_change);
            buffer.read_optional(this->timeout);
            buffer.read(this->timeout_absolute);
        }
    };

    enum class callback_id : uint32_t
    {
        Invalid = 0,
        NtUserGetThreadState,
        NtUserCreateWindowEx,
        NtUserDestroyWindow,
        NtUserShowWindow,
        NtUserMessageCall,
        NtUserUpdateWindow,
        NtUserEnumDisplayMonitors,
        NtUserSetWindowPos,
        NtUserGetComboBoxInfo,
        NtUserEndDeferWindowPosEx,
        NtUserGetMessage,
        NtUserPeekMessage,
        NtUserWaitMessage,
        NtUserMsgWaitForMultipleObjectsEx,
        NtUserSetWindowLong,
        NtUserSetWindowLongPtr,
        NtUserSetFocus,
        NtUserGetControlBrush,
        NtUserGetControlColor,
        NtUserFillWindow,
    };

    struct callback_frame
    {
        callback_id handler_id{};
        // The calling syscall's stack pointer, for the completion's own callbacks.
        uint64_t trap_stack_pointer{};
        uint64_t rip{};
        uint64_t rbp{};
        uint64_t rdi{};
        uint64_t rsi{};
        uint64_t rsp{};
        uint64_t r10{};
        uint64_t r11{};
        uint64_t r12{};
        uint64_t r13{};
        uint64_t r14{};
        uint64_t r15{};
        uint64_t rax{};
        uint64_t rbx{};
        uint64_t rcx{};
        uint64_t rdx{};
        uint64_t r8{};
        uint64_t r9{};
        uint16_t cs{};
        uint16_t ss{};
        uint16_t ds{};
        uint16_t es{};
        uint16_t fs{};
        uint16_t gs{};
        std::optional<arm64_context> arm64_registers{};
        std::unique_ptr<completion_state> state{};

        callback_frame();
        callback_frame(callback_id callback_id, std::unique_ptr<completion_state> completion_state);

        callback_frame(const callback_frame&) = delete;
        callback_frame& operator=(const callback_frame&) = delete;

        callback_frame(callback_frame&& obj) noexcept;
        callback_frame& operator=(callback_frame&& obj) noexcept;

        ~callback_frame();

        void save_registers(guest_cpu& cpu);
        void restore_registers(guest_cpu& cpu) const;

        void serialize(utils::buffer_serializer& buffer) const;
        void deserialize(utils::buffer_deserializer& buffer);
    };

    class emulator_process : public ref_counted_object
    {
      public:
        managed_process process{};
        uint32_t id{};
        uint32_t parent_id{};
        bool is_wow64_process{};
        uint64_t native_environment{};
        uint64_t compatibility_environment{};

      private:
        void serialize_object(utils::buffer_serializer& buffer) const override
        {
            buffer.write(this->process.value);
            buffer.write(this->id);
            buffer.write(this->parent_id);
            buffer.write(this->is_wow64_process);
            buffer.write(this->native_environment);
            buffer.write(this->compatibility_environment);
        }

        void deserialize_object(utils::buffer_deserializer& buffer) override
        {
            buffer.read(this->process.value);
            buffer.read(this->id);
            buffer.read(this->parent_id);
            buffer.read(this->is_wow64_process);
            buffer.read(this->native_environment);
            buffer.read(this->compatibility_environment);
        }
    };

    emulator_process make_emulator_process(const managed_process_identity& process);

    class managed_process_thread : public ref_counted_object
    {
      public:
        managed_process process{};
        uint32_t process_id{};
        uint32_t thread_id{};
        bool resume_observed{};

      private:
        void serialize_object(utils::buffer_serializer& buffer) const override
        {
            buffer.write(this->process.value);
            buffer.write(this->process_id);
            buffer.write(this->thread_id);
            buffer.write(this->resume_observed);
        }

        void deserialize_object(utils::buffer_deserializer& buffer) override
        {
            buffer.read(this->process.value);
            buffer.read(this->process_id);
            buffer.read(this->thread_id);
            buffer.read(this->resume_observed);
        }
    };

    enum class stack_guard_fault
    {
        outside_stack,
        grown,
        overflow,
    };

    enum class message_retrieval_mode
    {
        peek,
        remove,
        get,
    };

    // user32 caches the window a thread last worked with in Win32ClientInfo[8] (its handle) and [9] (its guest pointer).
    inline std::pair<uint64_t, uint64_t> get_cached_window(const emulator_object<TEB64>& teb)
    {
        constexpr auto offset = offsetof(TEB64, Win32ClientInfo);
        return {teb.field<std::uint64_t>(offset + 8 * sizeof(std::uint64_t)).read(),
                teb.field<std::uint64_t>(offset + 9 * sizeof(std::uint64_t)).read()};
    }

    inline void set_cached_window(const emulator_object<TEB64>& teb, const uint64_t handle, const uint64_t window)
    {
        constexpr auto offset = offsetof(TEB64, Win32ClientInfo);
        teb.field<std::uint64_t>(offset + 8 * sizeof(std::uint64_t)).write(handle);
        teb.field<std::uint64_t>(offset + 9 * sizeof(std::uint64_t)).write(window);
    }

    class emulator_thread : public ref_counted_object
    {
      public:
        emulator_thread(memory_manager& memory)
            : memory_ptr(&memory)
        {
        }

        emulator_thread(utils::buffer_deserializer& buffer)
            : emulator_thread(buffer.read<memory_manager_wrapper>().get())
        {
        }

        emulator_thread(memory_manager& memory, const process_context& context, uint64_t start_address, uint64_t argument,
                        uint64_t stack_size, uint32_t create_flags, uint32_t id, bool initial_thread, uint64_t stack_commit_size = 0);

        emulator_thread(const emulator_thread&) = delete;
        emulator_thread& operator=(const emulator_thread&) = delete;

        emulator_thread(emulator_thread&& obj) noexcept = default;
        emulator_thread& operator=(emulator_thread&& obj) noexcept = default;

        ~emulator_thread() override
        {
            this->release();
        }

        utils::moved_marker marker{};

        memory_manager* memory_ptr{};

        uint64_t stack_base{};                    // Native 64-bit stack base
        uint64_t stack_size{};                    // Native 64-bit stack size
        std::optional<uint64_t> wow64_stack_base; // WOW64 32-bit stack base
        std::optional<uint64_t> wow64_stack_size; // WOW64 32-bit stack size
        uint64_t start_address{};
        uint64_t argument{};
        uint64_t executed_instructions{0};
        uint64_t executed_blocks{0};
        bool setup_done{false};

        uint32_t id{};

        uint64_t current_ip{0};
        handle impersonation_token{};
        uint64_t previous_ip{0};

        std::u16string name{};
        ULONG io_priority{2};

        std::optional<NTSTATUS> exit_status{};
        // The waited objects by identity, resolved when the wait starts: closing or reusing the handle value a wait was
        // entered with must not detach it from its object (a duplicate keeps the object alive).
        std::vector<object_reference> await_objects{};
        // await_objects' objects as last looked up, one per reference. Not serialized; a restored thread looks them up again.
        std::vector<cached_wait_object> await_object_cache{};
        bool await_any{false};
        bool waiting_for_alert{false};
        // Set while a vCPU moves this thread's registers in or out with the kernel lock released. The thread must not
        // be scheduled, deleted or have its registers accessed until the transfer is done.
        bool registers_in_transit{false};
        // Counts syscalls on other vCPUs waiting in wait_for_thread_registers for this thread. The thread must not be
        // loaded onto a vCPU or deleted while it is non-zero.
        uint32_t pending_register_accesses{0};
        // The vCPU that loaded this thread last. The thread is still loaded while it is that vCPU's active thread.
        vcpu_context* last_vcpu{};
        // The address of a syscall that was retriggered and runs again when the thread continues, so that it is
        // reported once.
        std::optional<uint64_t> retriggered_syscall_ip{};
        bool alerted{false};
        // KTHREAD.Alerted[UserMode]: the alert NtAlertThread keeps for a thread that is not in an alertable wait
        // (KeAlertThread), which its next alertable wait, NtTestAlert or NtContinue's test-alert takes. `alerted` above is
        // NtAlertThreadByThreadId's own state.
        bool user_alerted{false};
        // KTHREAD.Queue: the port the thread's last NtRemoveIoCompletion(Ex) or NtWaitForWorkViaWorkerFactory joined
        // (KiSwitchQueue), which it stays with after the call. KeRemoveQueueEx gives such a thread what is queued before it
        // tests for an alert.
        object_reference associated_io_completion{};
        // A user APC queued while the thread was in an alertable wait ended the wait at once (KiSignalThreadForApc): the wait
        // returns STATUS_USER_APC once the APC runs, and an alert after the APC stays pending.
        bool user_apc_ended_wait{false};
        uint32_t create_flags{0};
        uint32_t suspended{0};
        std::optional<std::chrono::steady_clock::time_point> await_time{};
        // Whoever sets await_time sets this too: whether the guest gave it as an absolute system time.
        bool await_time_absolute{};
        std::optional<pending_msg> await_msg{};
        std::optional<DWORD> await_msg_mask{};
        std::optional<NTSTATUS> await_msg_ready_status{};
        std::optional<pending_io_completion_wait> await_io_completion{};
        std::optional<pending_debug_event_wait> await_debug_event{};
        std::optional<emulator_pointer> await_file_io{};
        bool await_console_io{};
        uint64_t ended_waits{};

        // When set, the thread is parked until this host-side predicate returns true. Used by the GPU
        // bridge to wait on a Vulkan semaphore cooperatively (other guest threads run while the GPU works)
        // instead of blocking the single VP. Polled by is_thread_ready and completed via
        // mark_as_ready(STATUS_SUCCESS). Idle vCPUs poll it once a millisecond unless woken, so whatever makes it
        // true calls windows_emulator::credit_idle_wakeup. A predicate cannot be saved: a snapshot keeps only
        // awaits_restored_host_condition, and the device that set it sets it again once the state is restored.
        std::function<bool()> await_host_condition{};
        bool awaits_restored_host_condition{false};

        bool apc_alertable{false};
        std::vector<pending_apc> pending_apcs{};

        std::optional<NTSTATUS> pending_status{};
        std::vector<sent_message_wait> sent_message_waits{};

        uint64_t win32k_thread_info{0};
        uint64_t win32k_thread_state{0};
        handle win32k_desktop{};
        bool win32k_thread_setup_pending{false};
        bool win32k_thread_setup_done{false};

        std::optional<emulator_allocator> gs_segment;
        std::optional<emulator_object<TEB64>> teb64;             // Native 64-bit TEB
        std::optional<emulator_object<TEB32>> teb32;             // WOW64 32-bit TEB
        std::optional<emulator_allocator> wow64_context_segment; // For WOW64 context (CONTEXT64) allocation
        std::optional<emulator_object<WOW64_CPURESERVED>>
            wow64_cpu_reserved; // Persistent WOW64 thread context for ThreadWow64Context queries
        std::optional<emulator_object<USER_CLIENTTHREADINFO>> client_thread_info;
        hwnd active_window{};
        hwnd focus_window{};

        std::vector<std::byte> last_registers{};

        bool debugger_hide{false};

        // Threads are not scheduled by processor, so this only answers SetThreadIdealProcessor.
        uint32_t ideal_processor{};

        std::vector<callback_frame> callback_stack;
        std::optional<uint64_t> callback_return_rax{};
        std::optional<user_thread_hook> cbt_hook{};
        std::optional<user_thread_hook> get_message_hook{};
        std::vector<user_low_level_hook> low_level_hooks{};

        std::optional<uint64_t> message_wait_return_register{};
        std::optional<std::chrono::steady_clock::time_point> message_wait_deadline{};
        std::vector<uint64_t> running_low_level_inputs{};

        std::map<user_timer_key, user_timer> user_timers{};
        uint64_t next_user_timer_id{1};
        std::vector<msg> message_queue;
        std::vector<sent_window_message> pending_sent_messages;
        std::vector<sent_window_message> active_sent_messages;
        DWORD current_message_time{};
        DWORD current_message_position{};
        std::array<uint32_t, 32> message_queue_status_bit_counts{};
        uint32_t message_queue_status_bits{};
        uint32_t queue_status_changed_bits{};

        void mark_as_ready(NTSTATUS status);
        // Ends the thread's alertable wait with STATUS_ALERTED, as KeAlertThread (KiSignalThread) or the alert test at the
        // start of a wait does.
        void end_wait_with_alert();

        void set_impersonation_token(const handle token)
        {
            this->impersonation_token = token;
            if (this->teb64)
            {
                this->teb64->field<ULONG>(offsetof(TEB64, IsImpersonating)).write(token != 0 ? 1 : 0);
            }
            if (this->teb32)
            {
                this->teb32->field<ULONG>(offsetof(TEB32, IsImpersonating)).write(token != 0 ? 1 : 0);
            }
        }

        void remember_message_wait(cpu_interface& emu, const windows_arch_abi& abi);
        void restart_message_wait(cpu_interface& emu, const windows_arch_abi& abi);
        bool needs_message_wait_restart(windows_emulator& win_emu);

        const sent_message_wait* current_sent_message_wait() const
        {
            if (this->sent_message_waits.empty() || this->sent_message_waits.back().callback_depth != this->callback_stack.size())
            {
                return nullptr;
            }
            return &this->sent_message_waits.back();
        }

        bool is_await_time_over(utils::clock& clock) const
        {
            constexpr auto infinite = std::chrono::steady_clock::time_point::min();
            return this->await_time.has_value() && this->await_time.value() != infinite && this->await_time.value() < clock.steady_now();
        }

        void move_absolute_deadlines(const std::chrono::steady_clock::duration system_time_ahead)
        {
            utils::follow_system_time(this->await_time, this->await_time_absolute, system_time_ahead);
            if (this->await_io_completion)
            {
                utils::follow_system_time(this->await_io_completion->timeout, this->await_io_completion->timeout_absolute,
                                          system_time_ahead);
            }

            if (this->await_debug_event)
            {
                utils::follow_system_time(this->await_debug_event->timeout, this->await_debug_event->timeout_absolute, system_time_ahead);
            }
        }

        bool has_pending_alertable_apc() const
        {
            return this->apc_alertable && !this->pending_apcs.empty();
        }

        // In an alertable wait KeAlertThread ends (KTHREAD.State Waiting, Alertable): one of the waits is_thread_ready checks,
        // still in progress, that the thread yielded for alertably and is not suspended in.
        bool is_in_alertable_wait() const
        {
            const auto waits = this->waiting_for_alert || this->await_msg_mask.has_value() || !this->await_objects.empty() ||
                               this->await_file_io || this->await_time.has_value() || this->await_host_condition ||
                               this->awaits_restored_host_condition || this->await_debug_event.has_value() ||
                               this->await_io_completion.has_value() || this->await_msg.has_value();
            return waits && this->apc_alertable && this->suspended == 0 && !this->is_terminated();
        }

        // KeInsertQueueApc of a user APC, as every way of queuing one ends in: KiSignalThreadForApc ends the thread's
        // alertable wait at once, with STATUS_USER_APC once the APC runs, so an alert after it finds the thread no longer
        // waiting (KeAlertThread).
        void queue_user_apc(const pending_apc& apc)
        {
            this->pending_apcs.push_back(apc);
            if (this->is_in_alertable_wait())
            {
                this->user_apc_ended_wait = true;
            }
        }

        void record_current_message(const msg& message)
        {
            this->current_message_time = message.time;
            const auto x = static_cast<uint16_t>(message.pt.x);
            const auto y = static_cast<uint16_t>(message.pt.y);
            this->current_message_position = static_cast<DWORD>(x) | (static_cast<DWORD>(y) << 16);
        }

        user_timer* find_user_timer(hwnd hwnd, uint64_t timer_id);
        user_timer& create_user_timer(process_context& process, hwnd hwnd, uint64_t timer_id, uint64_t timer_proc,
                                      std::chrono::milliseconds interval, std::chrono::steady_clock::time_point now,
                                      bool is_system = false);
        bool delete_user_timer(hwnd hwnd, uint64_t timer_id);
        bool synthesize_due_user_timer(windows_emulator& win_emu, hwnd hwnd_filter = 0, UINT filter_min = 0, UINT filter_max = 0);

        uint32_t get_message_queue_status(windows_emulator& win_emu);
        std::optional<msg> peek_pending_message(windows_emulator& win_emu, hwnd hwnd_filter = 0, UINT filter_min = 0, UINT filter_max = 0,
                                                message_retrieval_mode mode = message_retrieval_mode::peek);
        void post_message(windows_emulator& win_emu, msg msg, bool try_coalesce = false);
        void post_raw_input(windows_emulator& win_emu, hwnd window, uint32_t token, const raw_input_payload& payload);
        const raw_input_payload* find_raw_input(emulator_pointer token) const;
        void clear_raw_input();
        void remove_window_messages(hwnd window);
        void set_active_window(hwnd window);
        void set_focus_window(hwnd window);
        void forget_window(hwnd window);
        void mark_queue_status_changed(uint32_t bits);
        void clear_queue_status_changed(uint32_t bits);
        void publish_client_thread_info() const;

        bool is_terminated() const;

        bool is_thread_ready(windows_emulator& win_emu);
        // Whether the scheduler can pass this thread by without is_thread_ready: it waits for its alert, a delay or an I/O
        // completion port, and at now nothing has come that could end the wait (alert, APC, packet, port closed, due time).
        bool is_parked(std::chrono::steady_clock::time_point now, const process_context& process) const;

        void save(cpu_interface& emu)
        {
            this->last_registers = emu.save_registers();
        }

        void restore(cpu_interface& emu, const uint64_t gdt_base) const
        {
            this->load_registers(emu);
            this->refresh_execution_context(emu, gdt_base);
        }

        void load_registers(cpu_interface& emu) const
        {
            emu.restore_registers(this->last_registers);
        }

        void refresh_execution_context(cpu_interface& cpu, uint64_t gdt_base) const;

        stack_guard_fault handle_stack_guard_fault(uint64_t address) const;
        void grow_stack_for_frame(uint64_t address, uint64_t end) const;
        void commit_stacks_in_full() const;

        void setup_if_necessary(cpu_interface& emu, const process_context& context);

        void serialize_object(utils::buffer_serializer& buffer) const override
        {
            if (this->marker.was_moved())
            {
                throw std::runtime_error("Object was moved!");
            }

            buffer.write(this->stack_base);
            buffer.write(this->stack_size);
            buffer.write(this->start_address);
            buffer.write(this->argument);
            buffer.write(this->executed_instructions);
            buffer.write(this->executed_blocks);
            buffer.write(this->setup_done);
            buffer.write(this->id);
            buffer.write(this->current_ip);
            buffer.write(this->impersonation_token);
            buffer.write(this->previous_ip);

            buffer.write_string(this->name);
            buffer.write(this->io_priority);

            buffer.write_optional(this->exit_status);
            buffer.write_vector(this->await_objects);
            buffer.write(this->await_any);

            buffer.write(this->waiting_for_alert);
            buffer.write(this->alerted);
            buffer.write(this->user_alerted);
            buffer.write(this->associated_io_completion);
            buffer.write(this->user_apc_ended_wait);

            buffer.write(this->create_flags);
            buffer.write(this->suspended);
            buffer.write(this->ideal_processor);
            buffer.write_optional(this->await_time);
            buffer.write(this->await_time_absolute);
            buffer.write_optional(this->await_msg);
            buffer.write_optional(this->await_msg_mask);
            buffer.write_optional(this->await_msg_ready_status);
            buffer.write_optional(this->await_io_completion);
            buffer.write_optional(this->await_debug_event);
            buffer.write_optional(this->await_file_io);
            buffer.write(this->await_console_io);
            buffer.write(this->ended_waits);
            buffer.write(this->await_host_condition || this->awaits_restored_host_condition);

            buffer.write(this->apc_alertable);
            buffer.write_vector(this->pending_apcs);

            buffer.write_optional(this->pending_status);
            buffer.write_vector(this->sent_message_waits);
            buffer.write(this->win32k_thread_info);
            buffer.write(this->win32k_thread_state);
            buffer.write(this->win32k_desktop);
            buffer.write(this->win32k_thread_setup_pending);
            buffer.write(this->win32k_thread_setup_done);
            buffer.write_optional(this->gs_segment);
            buffer.write_optional(this->teb64);
            buffer.write_optional(this->wow64_stack_base);
            buffer.write_optional(this->wow64_stack_size);
            buffer.write_optional(this->teb32);
            buffer.write_optional(this->wow64_context_segment);
            buffer.write_optional(this->wow64_cpu_reserved);
            buffer.write_optional(this->client_thread_info);
            buffer.write(this->active_window);
            buffer.write(this->focus_window);

            buffer.write_vector(this->last_registers);

            buffer.write(this->debugger_hide);

            buffer.write_vector(this->callback_stack);
            buffer.write_optional(this->callback_return_rax);
            buffer.write_optional(this->cbt_hook);
            buffer.write_optional(this->get_message_hook);
            buffer.write_vector(this->low_level_hooks);
            buffer.write_optional(this->message_wait_return_register);
            buffer.write_optional(this->message_wait_deadline);
            buffer.write_vector(this->running_low_level_inputs);

            buffer.write_map(this->user_timers);
            buffer.write(this->next_user_timer_id);
            buffer.write_vector(this->message_queue);
            buffer.write_map(this->queued_raw_inputs_);
            buffer.write_optional(this->last_raw_input_);
            buffer.write_vector(this->pending_sent_messages);
            buffer.write_vector(this->active_sent_messages);
            buffer.write(this->current_message_time);
            buffer.write(this->current_message_position);
            buffer.write(this->message_queue_status_bit_counts);
            buffer.write(this->message_queue_status_bits);
            buffer.write(this->queue_status_changed_bits);
        }

        void deserialize_object(utils::buffer_deserializer& buffer) override
        {
            if (this->marker.was_moved())
            {
                throw std::runtime_error("Object was moved!");
            }

            this->release();

            buffer.read(this->stack_base);
            buffer.read(this->stack_size);
            buffer.read(this->start_address);
            buffer.read(this->argument);
            buffer.read(this->executed_instructions);
            buffer.read(this->executed_blocks);
            buffer.read(this->setup_done);
            buffer.read(this->id);
            buffer.read(this->current_ip);
            buffer.read(this->impersonation_token);
            buffer.read(this->previous_ip);

            buffer.read_string(this->name);
            buffer.read(this->io_priority);

            buffer.read_optional(this->exit_status);
            buffer.read_vector(this->await_objects);
            buffer.read(this->await_any);

            buffer.read(this->waiting_for_alert);
            buffer.read(this->alerted);
            buffer.read(this->user_alerted);
            buffer.read(this->associated_io_completion);
            buffer.read(this->user_apc_ended_wait);

            buffer.read(this->create_flags);
            buffer.read(this->suspended);
            buffer.read(this->ideal_processor);
            buffer.read_optional(this->await_time);
            buffer.read(this->await_time_absolute);
            buffer.read_optional(this->await_msg, [this] { return pending_msg{*this->memory_ptr}; });
            buffer.read_optional(this->await_msg_mask);
            buffer.read_optional(this->await_msg_ready_status);
            buffer.read_optional(this->await_io_completion, [] { return pending_io_completion_wait{}; });
            buffer.read_optional(this->await_debug_event, [] { return pending_debug_event_wait{}; });
            buffer.read_optional(this->await_file_io);
            buffer.read(this->await_console_io);
            buffer.read(this->ended_waits);
            buffer.read(this->awaits_restored_host_condition);
            this->await_host_condition = {};

            buffer.read(this->apc_alertable);
            buffer.read_vector(this->pending_apcs);

            buffer.read_optional(this->pending_status);
            buffer.read_vector(this->sent_message_waits);
            buffer.read(this->win32k_thread_info);
            buffer.read(this->win32k_thread_state);
            buffer.read(this->win32k_desktop);
            buffer.read(this->win32k_thread_setup_pending);
            buffer.read(this->win32k_thread_setup_done);
            buffer.read_optional(this->gs_segment, [this] { return emulator_allocator(*this->memory_ptr); });
            buffer.read_optional(this->teb64, [this] { return emulator_object<TEB64>(*this->memory_ptr); });
            buffer.read_optional(this->wow64_stack_base);
            buffer.read_optional(this->wow64_stack_size);
            buffer.read_optional(this->teb32, [this] { return emulator_object<TEB32>(*this->memory_ptr); });
            buffer.read_optional(this->wow64_context_segment, [this] { return emulator_allocator(*this->memory_ptr); });
            buffer.read_optional(this->wow64_cpu_reserved, [this] { return emulator_object<WOW64_CPURESERVED>(*this->memory_ptr); });
            buffer.read_optional(this->client_thread_info, [this] { return emulator_object<USER_CLIENTTHREADINFO>(*this->memory_ptr); });
            buffer.read(this->active_window);
            buffer.read(this->focus_window);

            buffer.read_vector(this->last_registers);

            buffer.read(this->debugger_hide);

            buffer.read_vector(this->callback_stack);
            buffer.read_optional(this->callback_return_rax);
            buffer.read_optional(this->cbt_hook);
            buffer.read_optional(this->get_message_hook);
            buffer.read_vector(this->low_level_hooks);
            buffer.read_optional(this->message_wait_return_register);
            buffer.read_optional(this->message_wait_deadline);
            buffer.read_vector(this->running_low_level_inputs);

            buffer.read_map(this->user_timers);
            buffer.read(this->next_user_timer_id);
            buffer.read_vector(this->message_queue);
            buffer.read_map(this->queued_raw_inputs_);
            buffer.read_optional(this->last_raw_input_);
            buffer.read_vector(this->pending_sent_messages);
            buffer.read_vector(this->active_sent_messages);
            buffer.read(this->current_message_time);
            buffer.read(this->current_message_position);
            buffer.read(this->message_queue_status_bit_counts);
            buffer.read(this->message_queue_status_bits);
            buffer.read(this->queue_status_changed_bits);
        }

        void leak_memory()
        {
            this->marker.mark_as_moved();
        }

        // A thread loaded on unloading_vcpu counts as unloaded, since that vCPU is about to unload it. The scheduler's
        // deletion and the handle deleter share this, so that the scheduler never erases a thread the deleter keeps.
        bool can_be_deleted(const vcpu_context* unloading_vcpu = nullptr) const;
        static bool deleter(emulator_thread& t);

      private:
        struct retrieved_raw_input
        {
            emulator_pointer token{};
            raw_input_payload payload{};
        };

        std::map<emulator_pointer, raw_input_payload> queued_raw_inputs_{};
        std::optional<retrieved_raw_input> last_raw_input_{};

        bool can_coalesce_message(const msg& msg) const;
        std::optional<msg> select_pending_message(windows_emulator& win_emu, hwnd hwnd_filter, UINT filter_min, UINT filter_max,
                                                  bool remove);
        std::optional<msg> peek_queued_message(const process_context& process, hwnd hwnd_filter, UINT filter_min, UINT filter_max,
                                               bool remove);

        void setup_registers(cpu_interface& emu, const process_context& context) const;
        void setup_wow64_registers(x86_64_cpu& emu, const process_context& context) const;

        void release()
        {
            if (this->marker.was_moved())
            {
                return;
            }

            if (this->stack_base)
            {
                if (!this->memory_ptr)
                {
                    throw std::runtime_error("Emulator was never assigned!");
                }

                this->memory_ptr->release_memory(this->stack_base, static_cast<size_t>(this->stack_size));
                this->stack_base = 0;
            }

            if (this->gs_segment)
            {
                this->gs_segment->release(*this->memory_ptr);
                this->gs_segment = {};
            }
        }
    };

} // namespace sogen
