#pragma once

#include "handles.hpp"
#include "emulator_utils.hpp"
#include "memory_manager.hpp"

#include <functional>

#include <utils/moved_marker.hpp>

namespace sogen
{

    struct completion_state;
    struct process_context;

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
        std::optional<std::chrono::steady_clock::time_point> due_time{};
        std::chrono::steady_clock::duration interval{};

        void serialize(utils::buffer_serializer& buffer) const
        {
            buffer.write(this->name);
            buffer.write(this->hwnd);
            buffer.write(this->timer_id);
            buffer.write(this->timer_proc);
            buffer.write_optional(this->due_time);
            buffer.write(this->interval);
        }

        void deserialize(utils::buffer_deserializer& buffer)
        {
            buffer.read(this->name);
            buffer.read(this->hwnd);
            buffer.read(this->timer_id);
            buffer.read(this->timer_proc);
            buffer.read_optional(this->due_time);
            buffer.read(this->interval);
        }
    };

    struct pending_apc
    {
        uint32_t flags{};
        uint64_t apc_routine{};
        uint64_t apc_argument1{};
        uint64_t apc_argument2{};
        uint64_t apc_argument3{};

        // For a WoW64 I/O completion APC: the WoW64 layer's 64-bit I/O status block (apc_argument2) is a
        // reused per-thread scratch buffer. A syscall issued between this APC being queued and dispatched
        // overwrites it, so the completion routine would read a stale byte count. Re-stamp the 32-bit status
        // block right before the APC runs to guarantee the correct result reaches the guest.
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

    enum class io_completion_wait_type : uint8_t
    {
        remove_single = 0,
        remove_multiple = 1,
    };

    struct pending_io_completion_wait
    {
        handle io_completion_handle{};
        io_completion_wait_type type{io_completion_wait_type::remove_single};
        emulator_pointer key_context_ptr{};
        emulator_pointer apc_context_ptr{};
        emulator_pointer io_status_block_ptr{};
        emulator_pointer completion_entries_ptr{};
        emulator_pointer entries_removed_ptr{};
        ULONG max_entries{};
        std::optional<std::chrono::steady_clock::time_point> timeout{};

        void serialize(utils::buffer_serializer& buffer) const
        {
            buffer.write(static_cast<uint8_t>(this->type));
            buffer.write(this->io_completion_handle);
            buffer.write(this->key_context_ptr);
            buffer.write(this->apc_context_ptr);
            buffer.write(this->io_status_block_ptr);
            buffer.write(this->completion_entries_ptr);
            buffer.write(this->entries_removed_ptr);
            buffer.write(this->max_entries);
            buffer.write_optional(this->timeout);
        }

        void deserialize(utils::buffer_deserializer& buffer)
        {
            const auto raw_type = buffer.read<uint8_t>();
            this->type = static_cast<io_completion_wait_type>(raw_type);
            buffer.read(this->io_completion_handle);
            buffer.read(this->key_context_ptr);
            buffer.read(this->apc_context_ptr);
            buffer.read(this->io_status_block_ptr);
            buffer.read(this->completion_entries_ptr);
            buffer.read(this->entries_removed_ptr);
            buffer.read(this->max_entries);
            buffer.read_optional(this->timeout);
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
        NtUserEnumDisplayMonitors,
    };

    struct callback_frame
    {
        callback_id handler_id{};
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
        std::unique_ptr<completion_state> state{};

        callback_frame();
        callback_frame(callback_id callback_id, std::unique_ptr<completion_state> completion_state);

        callback_frame(const callback_frame&) = delete;
        callback_frame& operator=(const callback_frame&) = delete;

        callback_frame(callback_frame&& obj) noexcept;
        callback_frame& operator=(callback_frame&& obj) noexcept;

        ~callback_frame();

        void save_registers(x86_64_emulator& emu);
        void restore_registers(x86_64_emulator& emu) const;

        void serialize(utils::buffer_serializer& buffer) const;
        void deserialize(utils::buffer_deserializer& buffer);
    };

    class emulator_process : public ref_counted_object
    {
        void serialize_object(utils::buffer_serializer&) const override
        {
        }

        void deserialize_object(utils::buffer_deserializer&) override
        {
        }
    };

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
                        uint64_t stack_size, uint32_t create_flags, uint32_t id, bool initial_thread);

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
        bool setup_done{false};

        uint32_t id{};

        uint64_t current_ip{0};
        uint64_t previous_ip{0};

        std::u16string name{};

        std::optional<NTSTATUS> exit_status{};
        std::vector<handle> await_objects{};
        bool await_any{false};
        bool waiting_for_alert{false};
        bool alerted{false};
        uint32_t create_flags{0};
        uint32_t suspended{0};
        std::optional<std::chrono::steady_clock::time_point> await_time{};
        std::optional<pending_msg> await_msg{};
        std::optional<DWORD> await_msg_mask{};
        std::optional<pending_io_completion_wait> await_io_completion{};

        // When set, the thread is parked until this host-side predicate returns true. Used by the GPU
        // bridge to wait on a Vulkan semaphore cooperatively (other guest threads run while the GPU works)
        // instead of blocking the single VP. Polled by is_thread_ready and completed via
        // mark_as_ready(STATUS_SUCCESS). Not serialized: the only user (GPU bridge) is not snapshotable.
        std::function<bool()> await_host_condition{};

        bool apc_alertable{false};
        std::vector<pending_apc> pending_apcs{};

        std::optional<NTSTATUS> pending_status{};

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

        std::vector<std::byte> last_registers{};

        bool debugger_hide{false};

        std::vector<callback_frame> callback_stack;
        std::optional<uint64_t> callback_return_rax{};

        std::map<user_timer_key, user_timer> user_timers{};
        uint64_t next_user_timer_id{1};
        std::vector<msg> message_queue;
        DWORD current_message_time{};
        std::array<uint32_t, 32> message_queue_status_bit_counts{};
        uint32_t message_queue_status_bits{};
        uint32_t queue_status_changed_bits{};

        void mark_as_ready(NTSTATUS status);

        bool is_await_time_over(utils::clock& clock) const
        {
            constexpr auto infinite = std::chrono::steady_clock::time_point::min();
            return this->await_time.has_value() && this->await_time.value() != infinite && this->await_time.value() < clock.steady_now();
        }

        bool has_pending_alertable_apc() const
        {
            return this->apc_alertable && !this->pending_apcs.empty();
        }

        user_timer* find_user_timer(hwnd hwnd, uint64_t timer_id);
        user_timer& create_user_timer(process_context& process, hwnd hwnd, uint64_t timer_id, uint64_t timer_proc,
                                      std::chrono::milliseconds interval, std::chrono::steady_clock::time_point now);
        bool delete_user_timer(hwnd hwnd, uint64_t timer_id);
        bool synthesize_due_user_timer(const windows_emulator& win_emu, hwnd hwnd_filter = 0, UINT filter_min = 0, UINT filter_max = 0);

        uint32_t get_message_queue_status(const windows_emulator& win_emu);
        std::optional<msg> peek_pending_message(const windows_emulator& win_emu, hwnd hwnd_filter = 0, UINT filter_min = 0,
                                                UINT filter_max = 0, bool remove = false);
        void post_message(const windows_emulator& win_emu, msg msg, bool try_coalesce = false);

        bool is_terminated() const;

        bool is_thread_ready(windows_emulator& win_emu);

        void save(x86_64_emulator& emu)
        {
            this->last_registers = emu.save_registers();
        }

        void restore(x86_64_emulator& emu) const
        {
            emu.restore_registers(this->last_registers);
            this->refresh_execution_context(emu);
        }

        void setup_if_necessary(x86_64_emulator& emu, const process_context& context)
        {
            if (!this->setup_done)
            {
                this->setup_done = true;
                this->setup_registers(emu, context);
            }

            if (this->pending_status.has_value())
            {
                const auto status = *this->pending_status;
                this->pending_status = {};

                emu.reg<uint64_t>(x86_register::rax, static_cast<uint64_t>(status));
            }
        }

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
            buffer.write(this->setup_done);
            buffer.write(this->id);
            buffer.write(this->current_ip);
            buffer.write(this->previous_ip);

            buffer.write_string(this->name);

            buffer.write_optional(this->exit_status);
            buffer.write_vector(this->await_objects);
            buffer.write(this->await_any);

            buffer.write(this->waiting_for_alert);
            buffer.write(this->alerted);

            buffer.write(this->create_flags);
            buffer.write(this->suspended);
            buffer.write_optional(this->await_time);
            buffer.write_optional(this->await_msg);
            buffer.write_optional(this->await_msg_mask);
            buffer.write_optional(this->await_io_completion);

            buffer.write(this->apc_alertable);
            buffer.write_vector(this->pending_apcs);

            buffer.write_optional(this->pending_status);
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

            buffer.write_vector(this->last_registers);

            buffer.write(this->debugger_hide);

            buffer.write_vector(this->callback_stack);
            buffer.write_optional(this->callback_return_rax);

            buffer.write_map(this->user_timers);
            buffer.write(this->next_user_timer_id);
            buffer.write_vector(this->message_queue);
            buffer.write(this->current_message_time);
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
            buffer.read(this->setup_done);
            buffer.read(this->id);
            buffer.read(this->current_ip);
            buffer.read(this->previous_ip);

            buffer.read_string(this->name);

            buffer.read_optional(this->exit_status);
            buffer.read_vector(this->await_objects);
            buffer.read(this->await_any);

            buffer.read(this->waiting_for_alert);
            buffer.read(this->alerted);

            buffer.read(this->create_flags);
            buffer.read(this->suspended);
            buffer.read_optional(this->await_time);
            buffer.read_optional(this->await_msg, [this] { return pending_msg{*this->memory_ptr}; });
            buffer.read_optional(this->await_msg_mask);
            buffer.read_optional(this->await_io_completion, [] { return pending_io_completion_wait{}; });

            buffer.read(this->apc_alertable);
            buffer.read_vector(this->pending_apcs);

            buffer.read_optional(this->pending_status);
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

            buffer.read_vector(this->last_registers);

            buffer.read(this->debugger_hide);

            buffer.read_vector(this->callback_stack);
            buffer.read_optional(this->callback_return_rax);

            buffer.read_map(this->user_timers);
            buffer.read(this->next_user_timer_id);
            buffer.read_vector(this->message_queue);
            buffer.read(this->current_message_time);
            buffer.read(this->message_queue_status_bit_counts);
            buffer.read(this->message_queue_status_bits);
            buffer.read(this->queue_status_changed_bits);
        }

        void leak_memory()
        {
            this->marker.mark_as_moved();
        }

        static bool deleter(emulator_thread& t)
        {
            return ref_counted_object::deleter(t) && t.is_terminated();
        }

      private:
        bool can_coalesce_message(const msg& msg) const;

        void setup_registers(x86_64_emulator& emu, const process_context& context) const;
        void refresh_execution_context(x86_64_emulator& emu) const;

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
