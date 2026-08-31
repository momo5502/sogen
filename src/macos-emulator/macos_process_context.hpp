#pragma once

#include "std_include.hpp"

#include "dyld_process.hpp"
#include "macos_kqueue.hpp"
#include "macos_memory_manager.hpp"
#include "macos_thread.hpp"

#include <arch_emulator.hpp>
#include <guest/guest_fd_table.hpp>
#include <serialization.hpp>

#include <vector>

namespace sogen
{

    struct macos_directory_entry
    {
        uint64_t inode{};
        uint8_t type{};
        std::string name{};

        void serialize(utils::buffer_serializer& buffer) const
        {
            buffer.write(this->inode);
            buffer.write(this->type);
            buffer.write_string(this->name);
        }

        void deserialize(utils::buffer_deserializer& buffer)
        {
            buffer.read(this->inode);
            buffer.read(this->type);
            buffer.read_string(this->name);
        }
    };

    // What guarded_open_np pinned to a descriptor. It lives here rather than in guest_fd because that
    // structure is shared with the Linux and Windows personalities, neither of which has anything a
    // guard id would mean.
    struct macos_fd_guard
    {
        uint64_t id{};
        uint32_t flags{};

        void serialize(utils::buffer_serializer& buffer) const
        {
            buffer.write(this->id);
            buffer.write(this->flags);
        }

        void deserialize(utils::buffer_deserializer& buffer)
        {
            buffer.read(this->id);
            buffer.read(this->flags);
        }
    };

    // One byte range a descriptor holds a lock on. Locks are advisory: they mean nothing except to
    // another opener of the same file that also asks. sogen runs a single process, so the only openers
    // that can ever contend are two descriptors of this one -- which is exactly the case a program that
    // opens one SQLite database twice relies on. Ownership is per descriptor rather than per process,
    // which is the F_OFD_SETLK rule; the plain F_SETLK family is given the same rule instead of the
    // POSIX one that drops every lock when any descriptor for the file closes.
    struct macos_file_lock
    {
        std::string path{};
        int32_t fd{};
        uint64_t start{};

        // Zero reaches the end of the file however far it later grows, exactly as struct flock means it.
        uint64_t length{};
        bool exclusive{};

        void serialize(utils::buffer_serializer& buffer) const
        {
            buffer.write_string(this->path);
            buffer.write(this->fd);
            buffer.write(this->start);
            buffer.write(this->length);
            buffer.write(this->exclusive);
        }

        void deserialize(utils::buffer_deserializer& buffer)
        {
            buffer.read_string(this->path);
            buffer.read(this->fd);
            buffer.read(this->start);
            buffer.read(this->length);
            buffer.read(this->exclusive);
        }
    };

    struct macos_process_context
    {
        guest_fd_table fds{macos_open::MACOS_O_APPEND};
        macos_kqueue_table kqueues{fds};

        // getdirentries64 has to report a stable snapshot across the calls that drain one directory, so
        // the host listing is read once per descriptor and replayed from here.
        std::map<int, std::vector<macos_directory_entry>> directory_entries{};
        std::map<int, size_t> directory_offsets{};

        // Only the descriptors guarded_open_np and guarded_kqueue_np produced. An entry outliving its
        // descriptor would hand the next open a guard it never asked for, so every path that retires a
        // descriptor goes through forget_descriptor_state.
        std::map<int, macos_fd_guard> fd_guards{};

        std::vector<macos_file_lock> file_locks{};

        // Anything but 1. dyld reads the pid and, seeing launchd's, runs libignition to boot the system:
        // it mounts preboot, reports failures to /dev/console, and closes the descriptors it believes it
        // inherited, which leaves the guest with no stdout. The value itself carries no meaning beyond
        // being an ordinary user-process pid.
        uint32_t pid{4242};
        uint32_t ppid{1};

        // The first regular macOS account and the staff group, not the 1000/1000 a Linux personality
        // would use.
        uint32_t uid{501};
        uint32_t gid{20};
        uint32_t euid{501};
        uint32_t egid{20};

        // The last two words of audit_token_t. Both have to be non-zero: libxpc reads the token in
        // bootstrap_look_up3 and traps outright when the pid or the pid version is clear, on the
        // reasoning that the kernel never hands out either. The version distinguishes a live pid from a
        // recycled one, so any non-zero value is a truthful answer for a process that has not been
        // replaced.
        uint32_t audit_session_id{100004};
        uint32_t pid_version{1};

        uint32_t signal_mask{};

        // Recorded, not delivered. A guest installs handlers long before anything could raise a signal,
        // and refusing the call stops start-up dead; remembering them keeps sigaction's own contract --
        // the next call gets back what the last one set -- without claiming signals are delivered.
        struct macos_signal_action
        {
            uint64_t handler{};
            uint64_t trampoline{};
            uint32_t mask{};
            uint32_t flags{};
        };

        std::map<uint32_t, macos_signal_action> signal_actions{};
        uint64_t shared_region_base{};
        std::optional<int> exit_status{};
        std::string current_working_directory{"/"};
        std::string executable_path{};

        std::map<uint64_t, macos_thread> threads{};
        macos_thread* active_thread{};
        uint64_t next_thread_id{1};

        // The only part of xnu's per-address psynch state that outlives a call: a hand-off or a wake that
        // arrives before its waiter registered (xnu's kw_pre_rwwc). The waiter that registers afterwards
        // consumes it instead of parking on an event that has already happened. The mutex map holds the
        // sequence word the successor is owed; the condition variable map holds a count.
        std::map<uint64_t, uint32_t> psynch_mutex_preposts{};
        std::map<uint64_t, uint32_t> psynch_cv_preposts{};
        uint64_t next_psynch_ticket{1};

        std::vector<std::string> argv{};
        std::vector<std::string> envp{};
        std::vector<std::string> apple{};

        uint64_t stack_base{};
        uint64_t stack_size{};

        // Handed over once by bsdthread_register. A new pthread does not begin at the start routine the
        // caller passed: it begins here, and this trampoline calls the routine after it has established
        // the thread's own state.
        uint64_t pthread_thread_start{};
        uint64_t pthread_wqthread{};

        macos_apple_strings apple_strings{};
        uint64_t kernel_args_pointer{};

        uint64_t create_thread(uint64_t thread_stack_base, uint64_t thread_stack_size, uint64_t entry_point);

        // Returns the woken thread's id, 0 when nobody was parked on the port.
        uint64_t wake_receivers_of(uint32_t port_name);

        // Every thread parked on the semaphore, for semaphore_signal_all. Returns how many were woken.
        size_t wake_all_semwait_waiters_of(uint32_t semaphore_name);

        // The ulock_wake counterpart: clears one waiter on the futex word and returns its id.
        uint64_t wake_ulock_waiter_of(uint64_t address);

        // The semaphore_signal counterpart: clears one __semwait_signal waiter on the semaphore and
        // returns its id. The count stays raised; the woken thread consumes it when its svc re-runs,
        // and the semwait_woken marker keeps that re-run from signalling the mutex semaphore again.
        uint64_t wake_semwait_waiter_of(uint32_t semaphore_name);

        // psynch_mutexdrop's counterpart: hands the mutex to the longest-queued psynch_mutexwait together
        // with the sequence word libpthread expects back. Returns the woken thread's id, 0 when nobody
        // was parked on the mutex.
        uint64_t wake_psynch_mutex_waiter_of(uint64_t mutex, uint32_t updatebits);

        // psynch_cvsignal and psynch_cvbroad's counterpart: wakes up to `limit` of the longest-queued
        // psynch_cvwait waiters and returns how many. Each woken thread re-runs its own svc, and the
        // psynch_cv_woken marker keeps that re-run from dropping the caller's mutex a second time.
        size_t wake_psynch_cv_waiters_of(uint64_t cv, size_t limit);

        void forget_descriptor_state(const int fd)
        {
            this->directory_entries.erase(fd);
            this->directory_offsets.erase(fd);
            this->fd_guards.erase(fd);
            std::erase_if(this->file_locks, [fd](const macos_file_lock& held) { return held.fd == fd; });
        }

        const macos_fd_guard* guard_of(const int fd) const
        {
            const auto entry = this->fd_guards.find(fd);
            return entry == this->fd_guards.end() ? nullptr : &entry->second;
        }

        bool setup_for_dyld(arm64_64_emulator& emu, macos_memory_manager& memory, uint64_t dyld_entry_point,
                            uint64_t executable_mach_header, const std::vector<std::string>& argv_values,
                            const std::vector<std::string>& envp_values, std::string process_executable_path);

        void setup(arm64_64_emulator& emu, macos_memory_manager& memory, uint64_t entry_point, const std::vector<std::string>& argv_values,
                   const std::vector<std::string>& envp_values, std::string process_executable_path);

        void serialize(utils::buffer_serializer& buffer) const;
        void deserialize(utils::buffer_deserializer& buffer);
    };

}
