#include "std_include.hpp"
#include "macos_process_context.hpp"

#include <address_utils.hpp>

#include <cinttypes>
#include <cstdio>

namespace sogen
{
    namespace
    {
        constexpr uint64_t MACOS_MAIN_STACK_BOTTOM = MACOS_MAIN_STACK_TOP - MACOS_MAIN_STACK_SIZE;
        constexpr uint64_t MACOS_STACK_POINTER_ALIGNMENT = 16;

        // Fixed rather than drawn from an entropy source: two runs of the same sample have to lay out
        // identical stacks for an analysis tool to be reproducible, and nothing in Stage 3 authenticates
        // the guard.
        constexpr uint64_t MACOS_STACK_GUARD_VALUE = 0x8F3C21A700B45D69ULL;

        // Deterministic by construction. A real kernel draws these from the entropy pool; sogen is a
        // deterministic replay tool, and a run that cannot be reproduced byte for byte is worth less
        // than one whose cookies are predictable.
        uint64_t derive_cookie(const std::string_view seed, const uint64_t salt)
        {
            uint64_t value = 0xCBF29CE484222325ull ^ salt;
            for (const auto character : seed)
            {
                value ^= static_cast<uint8_t>(character);
                value *= 0x100000001B3ull;
            }

            return value | 1;
        }

        std::string derive_hash(const std::string_view seed, const uint64_t salt)
        {
            std::string hash{};
            hash.reserve(40);

            auto value = derive_cookie(seed, salt);
            while (hash.size() < 40)
            {
                std::array<char, 32> buffer{};
                std::snprintf(buffer.data(), buffer.size(), "%016" PRIx64, value);
                hash.append(buffer.data());
                value = derive_cookie(hash, salt);
            }

            hash.resize(40);
            return hash;
        }

        std::string format_stack_guard(const uint64_t value)
        {
            constexpr std::string_view digits = "0123456789abcdef";

            std::array<char, 17> text{};
            for (size_t i = 0; i < 16; ++i)
            {
                text[15 - i] = digits[(value >> (i * 4)) & 0xF];
            }

            return "stack_guard=0x" + std::string{text.data()};
        }

        bool accumulate_string_bytes(const std::vector<std::string>& block, uint64_t& total)
        {
            for (const auto& value : block)
            {
                const auto entry = static_cast<uint64_t>(value.size()) + 1;
                if (entry > MACOS_MAIN_STACK_SIZE || total > MACOS_MAIN_STACK_SIZE - entry)
                {
                    return false;
                }

                total += entry;
            }

            return true;
        }

        uint64_t write_guest_string(macos_memory_manager& memory, uint64_t& cursor, const std::string& value)
        {
            const auto address = cursor;
            memory.write_memory(cursor, value.c_str(), value.size() + 1);
            cursor += value.size() + 1;
            return address;
        }

        void write_guest_pointer(macos_memory_manager& memory, uint64_t& cursor, const uint64_t value)
        {
            memory.write_memory(cursor, &value, sizeof(value));
            cursor += sizeof(value);
        }

        void write_guest_vector(macos_memory_manager& memory, uint64_t& cursor, const std::vector<uint64_t>& addresses)
        {
            for (const auto address : addresses)
            {
                write_guest_pointer(memory, cursor, address);
            }

            write_guest_pointer(memory, cursor, 0);
        }
    }

    uint64_t macos_process_context::create_thread(const uint64_t thread_stack_base, const uint64_t thread_stack_size,
                                                  const uint64_t entry_point)
    {
        const auto thread_id = this->next_thread_id++;

        macos_thread thread{};
        thread.thread_id = thread_id;
        thread.stack_base = thread_stack_base;
        thread.stack_size = thread_stack_size;
        thread.saved_regs.pc = entry_point;
        thread.saved_regs.sp = thread_stack_base + thread_stack_size;

        this->threads.emplace(thread_id, thread);
        return thread_id;
    }

    uint64_t macos_process_context::wake_receivers_of(const uint32_t port_name)
    {
        for (auto& [thread_id, thread] : this->threads)
        {
            if (!thread.terminated && thread.blocked_on_port == port_name)
            {
                thread.blocked_on_port = 0;
                return thread_id;
            }
        }

        return 0;
    }

    uint64_t macos_process_context::wake_ulock_waiter_of(const uint64_t address)
    {
        for (auto& [thread_id, thread] : this->threads)
        {
            if (!thread.terminated && thread.blocked_on_ulock == address)
            {
                thread.blocked_on_ulock = 0;
                return thread_id;
            }
        }

        return 0;
    }

    size_t macos_process_context::wake_all_semwait_waiters_of(const uint32_t semaphore_name)
    {
        size_t woken = 0;

        for (auto& [thread_id, thread] : this->threads)
        {
            if (thread.terminated || thread.blocked_on_sem != semaphore_name)
            {
                continue;
            }

            thread.blocked_on_sem = 0;
            thread.semwait_deadline = 0;

            if (this->active_thread == nullptr || thread_id != this->active_thread->thread_id)
            {
                thread.semwait_woken = true;
            }

            ++woken;
        }

        return woken;
    }

    uint64_t macos_process_context::wake_semwait_waiter_of(const uint32_t semaphore_name)
    {
        for (auto& [thread_id, thread] : this->threads)
        {
            if (!thread.terminated && thread.blocked_on_sem == semaphore_name)
            {
                thread.blocked_on_sem = 0;
                thread.semwait_deadline = 0;

                // The marker tells the woken re-run not to signal the mutex semaphore a second time.
                // Self-pairing (cond_sem == mutex_sem: the running thread signalled the semaphore it
                // just parked on) is resolved inline by the handler, so it never gets marked.
                if (this->active_thread == nullptr || thread_id != this->active_thread->thread_id)
                {
                    thread.semwait_woken = true;
                }

                return thread_id;
            }
        }

        return 0;
    }

    uint64_t macos_process_context::wake_psynch_mutex_waiter_of(const uint64_t mutex, const uint32_t updatebits)
    {
        macos_thread* longest_queued = nullptr;

        for (auto& [thread_id, thread] : this->threads)
        {
            if (thread.terminated || thread.blocked_on_psynch_mutex != mutex)
            {
                continue;
            }

            if (longest_queued == nullptr || thread.psynch_wait_ticket < longest_queued->psynch_wait_ticket)
            {
                longest_queued = &thread;
            }
        }

        if (longest_queued == nullptr)
        {
            return 0;
        }

        longest_queued->blocked_on_psynch_mutex = 0;
        longest_queued->psynch_mutex_updatebits = updatebits;
        return longest_queued->thread_id;
    }

    size_t macos_process_context::wake_psynch_cv_waiters_of(const uint64_t cv, const size_t limit)
    {
        std::vector<macos_thread*> queued{};

        for (auto& [thread_id, thread] : this->threads)
        {
            if (!thread.terminated && thread.blocked_on_psynch_cv == cv)
            {
                queued.push_back(&thread);
            }
        }

        std::ranges::sort(queued, {}, &macos_thread::psynch_wait_ticket);

        const auto woken = std::min(limit, queued.size());
        for (size_t i = 0; i < woken; ++i)
        {
            queued[i]->blocked_on_psynch_cv = 0;
            queued[i]->psynch_deadline = 0;
            queued[i]->psynch_cv_woken = true;
        }

        return woken;
    }

    bool macos_process_context::setup_for_dyld(arm64_64_emulator& emu, macos_memory_manager& memory, const uint64_t dyld_entry_point,
                                               const uint64_t executable_mach_header, const std::vector<std::string>& argv_values,
                                               const std::vector<std::string>& envp_values, std::string process_executable_path)
    {
        this->stack_base = MACOS_MAIN_STACK_BOTTOM;
        this->stack_size = MACOS_MAIN_STACK_SIZE;

        if (!memory.get_region_info(this->stack_base).has_value() &&
            !memory.allocate_memory(this->stack_base, MACOS_MAIN_STACK_SIZE, memory_permission::read_write))
        {
            return false;
        }

        this->executable_path = std::move(process_executable_path);
        this->argv = argv_values;
        this->envp = envp_values;

        this->apple_strings.executable_path = this->executable_path;
        this->apple_strings.stack_guard = derive_cookie(this->executable_path, 0x5A5A5A5A5A5A5A5Aull);
        this->apple_strings.malloc_entropy = {derive_cookie(this->executable_path, 1), derive_cookie(this->executable_path, 2)};
        this->apple_strings.ptr_munge = derive_cookie(this->executable_path, 3);
        this->apple_strings.main_stack_top = MACOS_MAIN_STACK_TOP;
        this->apple_strings.main_stack_size = MACOS_MAIN_STACK_SIZE;
        this->apple_strings.main_stack_alloc_base = this->stack_base;
        this->apple_strings.main_stack_alloc_size = MACOS_MAIN_STACK_SIZE;
        this->apple_strings.executable_cdhash = derive_hash(this->executable_path, 4);
        this->apple_strings.executable_boothash = derive_hash(this->executable_path, 5);

        this->apple = this->apple_strings.render();

        const auto layout = build_dyld_kernel_args(memory, MACOS_MAIN_STACK_TOP, this->stack_base, executable_mach_header, this->argv,
                                                   this->envp, this->apple);
        if (!layout.valid)
        {
            return false;
        }

        this->kernel_args_pointer = layout.stack_pointer;

        const auto thread_id = this->create_thread(this->stack_base, this->stack_size, dyld_entry_point);
        this->active_thread = &this->threads.at(thread_id);

        // dyld4::start reads prevDyldMH, dyldSharedCache and startTime from x1..x3 and branches on x3
        // being zero to source its own start time, so a stale register sends it down the restart path.
        for (size_t i = 0; i < MACOS_GENERAL_REGISTER_COUNT; ++i)
        {
            emu.reg(macos_thread_detail::general_register(i), uint64_t{0});
        }

        emu.reg(arm64_register::x29, uint64_t{0});
        emu.reg(arm64_register::x30, uint64_t{0});
        emu.reg(arm64_register::nzcv, uint64_t{0});
        emu.reg(arm64_register::sp, layout.stack_pointer);
        emu.reg(arm64_register::pc, dyld_entry_point);

        // Two pieces of libSystem read straight off the thread pointer, so it has to name real memory
        // rather than a token. cerror, which every failing syscall goes through, reads the errno slot at
        // TPIDRRO_EL0 + 8; libpthread takes its own struct to begin 0xE0 *below* the pointer and writes
        // there. A page mapped around the pointer satisfies both, and its zeroes are what a thread with
        // no TSD yet looks like.
        if (!memory.get_region_info(MACOS_MAIN_THREAD_STATE_BASE).has_value() &&
            !memory.allocate_memory(MACOS_MAIN_THREAD_STATE_BASE, MACOS_MAIN_THREAD_STATE_SIZE, memory_permission::read_write))
        {
            return false;
        }

        this->active_thread->thread_self = MACOS_MAIN_THREAD_STATE_BASE + MACOS_PTHREAD_STRUCT_TO_TSD_OFFSET;
        emu.set_thread_pointer(this->active_thread->thread_self);
        emu.reg(arm64_register::tpidr_el0, uint64_t{0});

        this->active_thread->save(emu);

        return true;
    }

    void macos_process_context::setup(arm64_64_emulator& emu, macos_memory_manager& memory, const uint64_t entry_point,
                                      const std::vector<std::string>& argv_values, const std::vector<std::string>& envp_values,
                                      std::string process_executable_path)
    {
        this->argv = argv_values;
        this->envp = envp_values;
        this->executable_path = std::move(process_executable_path);

        // The macho-loader-facts document could only recover executable_path= and stack_guard=. The rest
        // of the apple[] key set is unverified and belongs to Stage 5.
        this->apple = {"executable_path=" + this->executable_path, format_stack_guard(MACOS_STACK_GUARD_VALUE)};

        uint64_t string_bytes = 0;
        if (!accumulate_string_bytes(this->argv, string_bytes) || !accumulate_string_bytes(this->envp, string_bytes) ||
            !accumulate_string_bytes(this->apple, string_bytes))
        {
            return;
        }

        const auto pointer_slots = static_cast<uint64_t>(this->argv.size()) + this->envp.size() + this->apple.size() + 4;
        if (pointer_slots > MACOS_MAIN_STACK_SIZE / sizeof(uint64_t))
        {
            return;
        }

        const auto pointer_bytes = pointer_slots * sizeof(uint64_t);
        if (pointer_bytes > MACOS_MAIN_STACK_SIZE - string_bytes ||
            MACOS_STACK_POINTER_ALIGNMENT > MACOS_MAIN_STACK_SIZE - string_bytes - pointer_bytes)
        {
            return;
        }

        if (!memory.allocate_memory(MACOS_MAIN_STACK_BOTTOM, MACOS_MAIN_STACK_SIZE, memory_permission::read_write))
        {
            return;
        }

        this->stack_base = MACOS_MAIN_STACK_BOTTOM;
        this->stack_size = MACOS_MAIN_STACK_SIZE;

        auto string_cursor = MACOS_MAIN_STACK_TOP - string_bytes;

        const auto write_block = [&](const std::vector<std::string>& block) {
            std::vector<uint64_t> addresses{};
            addresses.reserve(block.size());

            for (const auto& value : block)
            {
                addresses.push_back(write_guest_string(memory, string_cursor, value));
            }

            return addresses;
        };

        const auto argv_addresses = write_block(this->argv);
        const auto envp_addresses = write_block(this->envp);
        const auto apple_addresses = write_block(this->apple);

        const auto sp = align_down(MACOS_MAIN_STACK_TOP - string_bytes - pointer_bytes, MACOS_STACK_POINTER_ALIGNMENT);

        auto vector_cursor = sp;
        write_guest_pointer(memory, vector_cursor, static_cast<uint64_t>(argv_addresses.size()));
        write_guest_vector(memory, vector_cursor, argv_addresses);
        write_guest_vector(memory, vector_cursor, envp_addresses);
        write_guest_vector(memory, vector_cursor, apple_addresses);

        for (size_t i = 0; i < MACOS_GENERAL_REGISTER_COUNT; ++i)
        {
            emu.reg(macos_thread_detail::general_register(i), uint64_t{0});
        }

        emu.reg(arm64_register::x29, uint64_t{0});
        emu.reg(arm64_register::x30, uint64_t{0});
        emu.reg(arm64_register::nzcv, uint64_t{0});
        emu.reg(arm64_register::sp, sp);
        emu.reg(arm64_register::pc, entry_point);

        const auto thread_id = this->create_thread(MACOS_MAIN_STACK_BOTTOM, MACOS_MAIN_STACK_SIZE, entry_point);
        const auto entry = this->threads.find(thread_id);
        if (entry == this->threads.end())
        {
            return;
        }

        this->active_thread = &entry->second;

        // Stage 4 replaces the token with the address of the real mach thread-self port. XNU publishes
        // thread identity through the read-only TPIDRRO_EL0, which is what set_thread_pointer writes,
        // and leaves TPIDR_EL0 to the process.
        this->active_thread->thread_self = MACOS_MAIN_STACK_BOTTOM;
        emu.set_thread_pointer(this->active_thread->thread_self);
        emu.reg(arm64_register::tpidr_el0, uint64_t{0});

        this->active_thread->save(emu);
    }

    void macos_process_context::serialize(utils::buffer_serializer& buffer) const
    {
        this->fds.serialize(buffer);
        buffer.write_map(this->directory_entries);
        buffer.write_map(this->directory_offsets);
        buffer.write_map(this->fd_guards);
        buffer.write_vector(this->file_locks);

        buffer.write(this->pid);
        buffer.write(this->ppid);
        buffer.write(this->uid);
        buffer.write(this->gid);
        buffer.write(this->euid);
        buffer.write(this->egid);
        buffer.write(this->audit_session_id);
        buffer.write(this->pid_version);
        buffer.write(this->signal_mask);
        buffer.write(this->shared_region_base);
        buffer.write_optional(this->exit_status);
        buffer.write(this->current_working_directory);
        buffer.write(this->executable_path);
        buffer.write_map(this->threads);
        buffer.write<uint64_t>(this->active_thread ? this->active_thread->thread_id : 0);
        buffer.write(this->next_thread_id);
        buffer.write_map(this->psynch_mutex_preposts);
        buffer.write_map(this->psynch_cv_preposts);
        buffer.write(this->next_psynch_ticket);
        buffer.write_vector(this->argv);
        buffer.write_vector(this->envp);
        buffer.write_vector(this->apple);
        buffer.write(this->stack_base);
        buffer.write(this->stack_size);
        buffer.write(this->pthread_thread_start);
        buffer.write(this->pthread_wqthread);
        buffer.write(this->apple_strings.executable_path);
        buffer.write(this->apple_strings.stack_guard);
        buffer.write(this->apple_strings.ptr_munge);
        buffer.write(this->apple_strings.malloc_entropy[0]);
        buffer.write(this->apple_strings.malloc_entropy[1]);
        buffer.write(this->apple_strings.th_port);
        buffer.write(this->kernel_args_pointer);
    }

    void macos_process_context::deserialize(utils::buffer_deserializer& buffer)
    {
        // The reopen happens inside deserialize, so the temporary has to carry Darwin's O_APPEND too:
        // a default-constructed one decodes with Linux's 02000, which is O_TRUNC on Darwin, and the
        // move below would then leave this->fds permanently downgraded.
        guest_fd_table new_fds{macos_open::MACOS_O_APPEND};
        new_fds.deserialize(buffer);

        auto new_directory_entries = buffer.read_map<std::map<int, std::vector<macos_directory_entry>>>();
        auto new_directory_offsets = buffer.read_map<std::map<int, size_t>>();
        auto new_fd_guards = buffer.read_map<std::map<int, macos_fd_guard>>();
        auto new_file_locks = buffer.read_vector<macos_file_lock>();

        const auto new_pid = buffer.read<uint32_t>();
        const auto new_ppid = buffer.read<uint32_t>();
        const auto new_uid = buffer.read<uint32_t>();
        const auto new_gid = buffer.read<uint32_t>();
        const auto new_euid = buffer.read<uint32_t>();
        const auto new_egid = buffer.read<uint32_t>();
        const auto new_audit_session_id = buffer.read<uint32_t>();
        const auto new_pid_version = buffer.read<uint32_t>();

        const auto new_signal_mask = buffer.read<uint32_t>();
        const auto new_shared_region_base = buffer.read<uint64_t>();

        std::optional<int> new_exit_status{};
        buffer.read_optional(new_exit_status);

        auto new_current_working_directory = buffer.read<std::string>();
        auto new_executable_path = buffer.read<std::string>();
        auto new_threads = buffer.read_map<std::map<uint64_t, macos_thread>>();
        const auto active_id = buffer.read<uint64_t>();
        const auto new_next_thread_id = buffer.read<uint64_t>();
        auto new_psynch_mutex_preposts = buffer.read_map<std::map<uint64_t, uint32_t>>();
        auto new_psynch_cv_preposts = buffer.read_map<std::map<uint64_t, uint32_t>>();
        const auto new_next_psynch_ticket = buffer.read<uint64_t>();
        auto new_argv = buffer.read_vector<std::string>();
        auto new_envp = buffer.read_vector<std::string>();
        auto new_apple = buffer.read_vector<std::string>();
        const auto new_stack_base = buffer.read<uint64_t>();
        const auto new_stack_size = buffer.read<uint64_t>();
        const auto new_pthread_thread_start = buffer.read<uint64_t>();
        const auto new_pthread_wqthread = buffer.read<uint64_t>();
        auto new_apple_executable_path = buffer.read<std::string>();
        const auto new_stack_guard = buffer.read<uint64_t>();
        const auto new_ptr_munge = buffer.read<uint64_t>();
        const auto new_malloc_entropy_low = buffer.read<uint64_t>();
        const auto new_malloc_entropy_high = buffer.read<uint64_t>();
        const auto new_th_port = buffer.read<uint32_t>();
        const auto new_kernel_args_pointer = buffer.read<uint64_t>();

        if (active_id != 0 && !new_threads.contains(active_id))
        {
            throw std::runtime_error("macOS process snapshot names a missing active thread " + std::to_string(active_id));
        }

        for (const auto& [fd, offset] : new_directory_offsets)
        {
            const auto entries = new_directory_entries.find(fd);
            if (entries == new_directory_entries.end() || offset > entries->second.size())
            {
                throw std::runtime_error("macOS process snapshot has an out of range directory offset for fd " + std::to_string(fd));
            }
        }

        this->fds = std::move(new_fds);
        this->directory_entries = std::move(new_directory_entries);
        this->directory_offsets = std::move(new_directory_offsets);
        this->fd_guards = std::move(new_fd_guards);
        this->file_locks = std::move(new_file_locks);
        this->pid = new_pid;
        this->ppid = new_ppid;
        this->uid = new_uid;
        this->gid = new_gid;
        this->euid = new_euid;
        this->egid = new_egid;
        this->audit_session_id = new_audit_session_id;
        this->pid_version = new_pid_version;
        this->signal_mask = new_signal_mask;
        this->shared_region_base = new_shared_region_base;
        this->exit_status = new_exit_status;
        this->current_working_directory = std::move(new_current_working_directory);
        this->executable_path = std::move(new_executable_path);
        this->threads = std::move(new_threads);
        this->next_thread_id = new_next_thread_id;
        this->psynch_mutex_preposts = std::move(new_psynch_mutex_preposts);
        this->psynch_cv_preposts = std::move(new_psynch_cv_preposts);
        this->next_psynch_ticket = new_next_psynch_ticket;
        this->argv = std::move(new_argv);
        this->envp = std::move(new_envp);
        this->apple = std::move(new_apple);
        this->stack_base = new_stack_base;
        this->stack_size = new_stack_size;
        this->pthread_thread_start = new_pthread_thread_start;
        this->pthread_wqthread = new_pthread_wqthread;
        this->apple_strings.executable_path = std::move(new_apple_executable_path);
        this->apple_strings.stack_guard = new_stack_guard;
        this->apple_strings.ptr_munge = new_ptr_munge;
        this->apple_strings.malloc_entropy = {new_malloc_entropy_low, new_malloc_entropy_high};
        this->apple_strings.th_port = new_th_port;
        this->kernel_args_pointer = new_kernel_args_pointer;

        this->active_thread = active_id != 0 ? &this->threads.at(active_id) : nullptr;
    }
}
