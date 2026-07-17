#include "std_include.hpp"
#include "linux_process_context.hpp"
#include "linux_emulator_utils.hpp"

#include <address_utils.hpp>
#include <platform/elf.hpp>
#include <serialization_helper.hpp>

namespace sogen
{

    using namespace elf; // NOLINT(google-build-using-namespace)

    namespace utils
    {
        static void serialize(buffer_serializer& buffer, const linux_cached_dir_entry& entry)
        {
            buffer.write(entry.ino);
            buffer.write(entry.d_type);
            buffer.write(entry.name);
        }

        static void deserialize(buffer_deserializer& buffer, linux_cached_dir_entry& entry)
        {
            buffer.read(entry.ino);
            buffer.read(entry.d_type);
            buffer.read(entry.name);
        }

        static void serialize(buffer_serializer& buffer, const linux_epoll_instance& instance)
        {
            buffer.write_vector(instance.entries);
        }

        static void deserialize(buffer_deserializer& buffer, linux_epoll_instance& instance)
        {
            buffer.read_vector(instance.entries);
        }
    }

    namespace
    {
        constexpr uint64_t STACK_SIZE = 0x800000; // 8MB
        constexpr uint64_t STACK_TOP = 0x7ffffffde000;

        // Write a null-terminated string into emulator memory. Returns the address where it was written.
        uint64_t write_string_to_memory(linux_memory_manager& memory, uint64_t& cursor, const std::string_view str)
        {
            const auto addr = cursor;
            memory.write_memory(cursor, str.data(), str.size());
            cursor += str.size();

            // Write null terminator
            const uint8_t zero = 0;
            memory.write_memory(cursor, &zero, 1);
            cursor += 1;

            return addr;
        }

    }

    void linux_process_context::setup(x86_64_emulator& emu, linux_memory_manager& memory, const linux_mapped_module& exe,
                                      const std::vector<std::string>& argv_values, const std::vector<std::string>& envp_values,
                                      std::string process_executable_path, const uint64_t interpreter_base, const uint64_t initial_rip,
                                      const uint64_t vdso_base)
    {
        // Store argv/envp for procfs emulation
        this->argv = argv_values;
        this->envp = envp_values;
        this->executable_path = std::move(process_executable_path);

        // ---- Allocate the stack ----
        const auto stack_base = STACK_TOP;
        const auto stack_alloc_base = stack_base - STACK_SIZE;

        if (!memory.allocate_memory(stack_alloc_base, STACK_SIZE, memory_permission::read_write))
        {
            throw std::runtime_error("Failed to allocate stack");
        }

        // ---- Set brk ----
        // brk_base = page-aligned end of last PT_LOAD segment
        {
            uint64_t highest_end = 0;
            for (const auto& sec : exe.sections)
            {
                const auto sec_end = sec.start + sec.length;
                highest_end = std::max(highest_end, sec_end);
            }

            if (highest_end == 0)
            {
                highest_end = exe.image_base + exe.size_of_image;
            }

            this->brk_base = page_align_up(highest_end);
            this->brk_current = this->brk_base;
        }

        // ---- Build initial stack ----
        // The Linux initial stack layout (from high to low addresses):
        //   [string data: argv strings, envp strings, platform string, AT_RANDOM bytes]
        //   [padding to 16-byte alignment]
        //   AT_NULL (two zeros)
        //   auxiliary vector entries (pairs of uint64_t)
        //   NULL (envp terminator)
        //   envp[n-1] ... envp[0]  (pointers)
        //   NULL (argv terminator)
        //   argv[n-1] ... argv[0]  (pointers)
        //   argc (uint64_t)
        //   ← RSP points here

        // Start from the top of the stack and work downward for string data
        uint64_t string_cursor = stack_base - 256; // Leave some room at the very top

        // Write platform string "x86_64"
        const auto platform_addr = write_string_to_memory(memory, string_cursor, "x86_64");

        // Write argv strings
        std::vector<uint64_t> argv_addrs{};
        argv_addrs.reserve(argv_values.size());
        for (const auto& arg : argv_values)
        {
            argv_addrs.push_back(write_string_to_memory(memory, string_cursor, arg));
        }

        // Write envp strings
        std::vector<uint64_t> envp_addrs{};
        envp_addrs.reserve(envp_values.size());
        for (const auto& env : envp_values)
        {
            envp_addrs.push_back(write_string_to_memory(memory, string_cursor, env));
        }

        const auto random_addr = string_cursor;
        {
            std::array<uint8_t, 16> random_bytes{};
            uint32_t byte_index = 0;
            for (auto& byte : random_bytes)
            {
                byte = static_cast<uint8_t>((byte_index * 73U + 0xABU) & 0xFFU);
                ++byte_index;
            }
            memory.write_memory(string_cursor, random_bytes.data(), random_bytes.size());
            string_cursor += random_bytes.size();
        }

        // Now build the stack from the bottom up (we push items onto a stack pointer
        // starting below the string data area)
        // Align string_cursor to 16 bytes
        string_cursor = align_up(string_cursor, 16);

        // We'll build the stack frame in a separate area below the strings
        // Start sp well below the string data
        uint64_t sp = stack_base - 0x1000; // Give plenty of room

        // Align sp to 16 bytes
        sp = align_down(sp, 16);

        // We need to build the stack layout in memory from bottom to top:
        // But in stack convention we push from high address downward.
        // So we'll calculate the total size needed, then write it.

        // Calculate the number of entries we need:
        const auto argc = argv_values.size();
        // Total entries: argc(1) + argv_ptrs(argc) + null(1) + envp_ptrs(envp.size()) + null(1) + auxv entries
        // Each auxv entry is 2 uint64_t values

        this->auxv.clear();
        this->auxv.push_back({.type = AT_PHDR, .value = exe.phdr_vaddr});
        this->auxv.push_back({.type = AT_PHENT, .value = exe.phdr_entry_size});
        this->auxv.push_back({.type = AT_PHNUM, .value = exe.phdr_count});
        this->auxv.push_back({.type = AT_PAGESZ, .value = 4096});
        this->auxv.push_back({.type = AT_BASE, .value = interpreter_base}); // Interpreter load address (0 if static)
        this->auxv.push_back({.type = AT_ENTRY, .value = exe.entry_point});
        this->auxv.push_back({.type = AT_UID, .value = this->uid});
        this->auxv.push_back({.type = AT_EUID, .value = this->euid});
        this->auxv.push_back({.type = AT_GID, .value = this->gid});
        this->auxv.push_back({.type = AT_EGID, .value = this->egid});
        this->auxv.push_back({.type = AT_SECURE, .value = 0});
        this->auxv.push_back({.type = AT_RANDOM, .value = random_addr});
        this->auxv.push_back({.type = AT_PLATFORM, .value = platform_addr});
        this->auxv.push_back({.type = AT_CLKTCK, .value = 100});
        if (vdso_base != 0)
        {
            this->auxv.push_back({.type = AT_SYSINFO_EHDR, .value = vdso_base});
        }
        this->auxv.push_back({.type = AT_NULL, .value = 0});

        // Total stack frame size (in uint64_t units):
        // 1 (argc) + argc (argv ptrs) + 1 (null) + envp.size() (envp ptrs) + 1 (null) + auxv.size()*2
        const auto total_u64s = 1 + argc + 1 + envp_values.size() + 1 + this->auxv.size() * 2;
        const auto frame_size = total_u64s * sizeof(uint64_t);

        // Position sp so the frame fits
        sp = align_down(sp - frame_size, 16);

        // Write the frame
        uint64_t write_ptr = sp;

        // argc
        {
            const auto val = static_cast<uint64_t>(argc);
            memory.write_memory(write_ptr, &val, sizeof(val));
            write_ptr += sizeof(uint64_t);
        }

        // argv pointers
        for (const auto& addr : argv_addrs)
        {
            memory.write_memory(write_ptr, &addr, sizeof(addr));
            write_ptr += sizeof(uint64_t);
        }

        // argv NULL terminator
        {
            const uint64_t zero = 0;
            memory.write_memory(write_ptr, &zero, sizeof(zero));
            write_ptr += sizeof(uint64_t);
        }

        // envp pointers
        for (const auto& addr : envp_addrs)
        {
            memory.write_memory(write_ptr, &addr, sizeof(addr));
            write_ptr += sizeof(uint64_t);
        }

        // envp NULL terminator
        {
            const uint64_t zero = 0;
            memory.write_memory(write_ptr, &zero, sizeof(zero));
            write_ptr += sizeof(uint64_t);
        }

        // auxiliary vector
        for (const auto& entry : this->auxv)
        {
            memory.write_memory(write_ptr, &entry.type, sizeof(entry.type));
            write_ptr += sizeof(uint64_t);
            memory.write_memory(write_ptr, &entry.value, sizeof(entry.value));
            write_ptr += sizeof(uint64_t);
        }

        // ---- Set up initial TLS area ----
        // x86-64 TLS variant II layout (used by glibc and musl):
        //
        //   Low address                                    High address
        //   [   TLS data block (tls_mem_size)   ] [ TCB (Thread Control Block) ]
        //                                          ^
        //                                          |
        //                                        FS_BASE points here
        //
        // Thread-local variables are accessed at NEGATIVE offsets from FS_BASE.
        // The TCB's first 8 bytes at %fs:0 contain a self-pointer (the "tp" value).
        // glibc's TCB also expects: offset 0 = self-pointer, offset 8 = dtv pointer.
        //
        // For static binaries with PT_TLS, we copy .tdata and zero .tbss into the
        // TLS data block. For dynamic binaries, the dynamic linker (ld-linux/ld-musl)
        // will set up its own TLS via arch_prctl(ARCH_SET_FS), but we still need a
        // valid initial FS_BASE so that any early %fs: accesses don't fault.

        constexpr uint64_t INITIAL_TLS_REGION = 0x7ffff7ff8000; // Base of the TLS allocation region
        constexpr size_t MIN_TLS_REGION_SIZE = 0x6000;          // 24KB minimum (6 pages)
        constexpr size_t TCB_SIZE = 0xA00; // 2560 bytes for TCB + struct pthread (glibc's struct pthread is ~0x900 bytes)

        // Determine TLS data size from the executable's PT_TLS segment
        const auto tls_mem_size = exe.tls_mem_size;
        const auto tls_image_size = exe.tls_image_size;
        const auto tls_alignment = exe.tls_alignment > 0 ? exe.tls_alignment : 8;
        const auto tls_image_addr = exe.tls_image_addr; // Address of .tdata in emulated memory (already mapped)

        // Calculate the total region size needed:
        // We need: [alignment padding] [TLS block (tls_mem_size, aligned)] [TCB (TCB_SIZE)]
        // All page-aligned for the allocation.
        const auto aligned_tls_size = align_up(tls_mem_size, tls_alignment);
        const auto total_tls_needed = aligned_tls_size + TCB_SIZE;
        const auto tls_region_size = page_align_up(std::max(total_tls_needed, static_cast<uint64_t>(MIN_TLS_REGION_SIZE)));

        // Position the allocation so that the TCB falls at a known address.
        // We allocate from INITIAL_TLS_REGION and compute the TCB position within it.
        const auto tls_alloc_base = INITIAL_TLS_REGION;

        uint64_t initial_fs_base = 0;

        if (memory.allocate_memory(tls_alloc_base, static_cast<size_t>(tls_region_size), memory_permission::read_write))
        {
            // Place the TCB at the end of the allocated region (aligned to 16 bytes).
            // The TLS data block sits immediately before the TCB.
            const auto tcb_addr = align_down(tls_alloc_base + tls_region_size - TCB_SIZE, 16);
            const auto tls_block_start = tcb_addr - aligned_tls_size;

            // Copy .tdata (initialized thread-local data) from the mapped ELF image
            if (tls_image_size > 0 && tls_image_addr != 0)
            {
                // The .tdata is already in emulated memory at tls_image_addr (from PT_LOAD mapping).
                // Copy it to the TLS block. In variant II, .tdata goes at the END of the TLS block
                // (closest to the TCB), and .tbss fills the space before it.
                // Actually, the layout is: [.tbss zero-fill] [.tdata] [TCB]
                // with .tdata at the highest addresses of the TLS block.
                // But the standard glibc layout is simpler:
                //   TLS block at [tls_block_start .. tls_block_start + tls_mem_size)
                //   where [0 .. tls_image_size) = .tdata copy, [tls_image_size .. tls_mem_size) = .tbss (zeroed)
                // Thread-local vars are accessed as %fs:-(tls_mem_size - var_offset)
                std::vector<uint8_t> tls_data(static_cast<size_t>(tls_image_size));
                memory.read_memory(tls_image_addr, tls_data.data(), tls_data.size());
                memory.write_memory(tls_block_start, tls_data.data(), tls_data.size());
            }

            // .tbss portion (tls_image_size .. tls_mem_size) is already zero from fresh allocation

            // Write self-referencing pointer at TCB offset 0 (%fs:0 = tcb pointer)
            memory.write_memory(tcb_addr, &tcb_addr, sizeof(uint64_t));

            // Write a NULL DTV pointer at TCB offset 8 (%fs:8 = dtv)
            // glibc checks this; a NULL or valid pointer prevents crashes
            const uint64_t null_dtv = 0;
            memory.write_memory(tcb_addr + 8, &null_dtv, sizeof(uint64_t));

            // Write self-pointer at TCB offset 16 (%fs:0x10 = struct pthread * / thread descriptor)
            // In glibc, the TCB is embedded within struct pthread, so the thread descriptor
            // pointer should point to the TCB itself. glibc's __syscall_cancel and other
            // routines access fields via %fs:0x10 + offset (e.g., 0x308 for cancel state).
            // By pointing %fs:0x10 to the TCB (which has a large zero-filled area after it),
            // these accesses will read zeros instead of faulting.
            memory.write_memory(tcb_addr + 16, &tcb_addr, sizeof(uint64_t));

            // Set FS_BASE to the TCB
            emu.set_segment_base(x86_register::fs, tcb_addr);
            initial_fs_base = tcb_addr;
        }
        else
        {
            // Fallback: if allocation failed, set a basic FS_BASE anyway
            initial_fs_base = INITIAL_TLS_REGION;
        }

        // ---- Create the initial thread ----
        // If an interpreter is loaded, execution starts at the interpreter's entry point.
        // The executable's entry point is communicated via AT_ENTRY in the auxiliary vector.
        const auto start_rip = (initial_rip != 0) ? initial_rip : exe.entry_point;

        const auto tid = this->create_thread(stack_alloc_base, STACK_SIZE, start_rip, initial_fs_base);

        auto it = this->threads.find(tid);
        assert(it != this->threads.end());
        this->active_thread = &it->second;

        // ---- Set CPU state ----
        emu.reg(x86_register::rsp, sp);
        emu.reg(x86_register::rip, start_rip);
        emu.reg(x86_register::rflags, uint64_t{0x202}); // IF flag set

        // Clear general-purpose registers
        emu.reg(x86_register::rax, uint64_t{0});
        emu.reg(x86_register::rbx, uint64_t{0});
        emu.reg(x86_register::rcx, uint64_t{0});
        emu.reg(x86_register::rdx, uint64_t{0});
        emu.reg(x86_register::rsi, uint64_t{0});
        emu.reg(x86_register::rdi, uint64_t{0});
        emu.reg(x86_register::rbp, uint64_t{0});
        emu.reg(x86_register::r8, uint64_t{0});
        emu.reg(x86_register::r9, uint64_t{0});
        emu.reg(x86_register::r10, uint64_t{0});
        emu.reg(x86_register::r11, uint64_t{0});
        emu.reg(x86_register::r12, uint64_t{0});
        emu.reg(x86_register::r13, uint64_t{0});
        emu.reg(x86_register::r14, uint64_t{0});
        emu.reg(x86_register::r15, uint64_t{0});

        // Update the thread's saved registers to reflect initial state
        this->active_thread->saved_regs.rsp = sp;
        this->active_thread->saved_regs.rip = start_rip;
        this->active_thread->saved_regs.rflags = 0x202;
        this->active_thread->saved_regs.fs_base = initial_fs_base;
        this->active_thread->fs_base = initial_fs_base;
        this->active_thread->stack_base = stack_alloc_base;
        this->active_thread->stack_size = STACK_SIZE;
    }

    void linux_process_context::serialize(utils::buffer_serializer& buffer) const
    {
        this->fds.serialize(buffer);
        buffer.write_map(this->directory_entries);
        buffer.write_map(this->directory_offsets);

        buffer.write<uint64_t>(this->epoll_instances.size());
        for (const auto& [fd, instance] : this->epoll_instances)
        {
            buffer.write(fd);
            if (!instance)
            {
                throw std::runtime_error("Linux process snapshot found an invalid epoll instance");
            }
            buffer.write(*instance);
        }

        buffer.write(this->brk_base);
        buffer.write(this->brk_current);
        buffer.write_map(this->threads);
        buffer.write<uint32_t>(this->active_thread ? this->active_thread->tid : 0);
        buffer.write(this->pid);
        buffer.write(this->ppid);
        buffer.write(this->uid);
        buffer.write(this->gid);
        buffer.write(this->euid);
        buffer.write(this->egid);
        buffer.write_optional(this->exit_status);
        buffer.write(this->current_working_directory);
        buffer.write(this->executable_path);
        buffer.write(this->next_tid);
        buffer.write_vector(this->argv);
        buffer.write_vector(this->envp);
        buffer.write_vector(this->auxv);
    }

    void linux_process_context::deserialize(utils::buffer_deserializer& buffer)
    {
        linux_fd_table new_fds{};
        new_fds.deserialize(buffer);
        auto new_directory_entries = buffer.read_map<std::map<int, std::vector<linux_cached_dir_entry>>>();
        auto new_directory_offsets = buffer.read_map<std::map<int, size_t>>();

        std::map<int, std::shared_ptr<linux_epoll_instance>> new_epoll_instances{};
        const auto epoll_count = buffer.read<uint64_t>();
        for (uint64_t i = 0; i < epoll_count; ++i)
        {
            const auto fd = buffer.read<int>();
            auto instance = std::make_shared<linux_epoll_instance>();
            buffer.read(*instance);
            new_epoll_instances.emplace(fd, std::move(instance));
        }

        auto new_brk_base = buffer.read<uint64_t>();
        auto new_brk_current = buffer.read<uint64_t>();
        auto new_threads = buffer.read_map<std::map<uint32_t, linux_thread>>();
        const auto active_tid = buffer.read<uint32_t>();
        auto new_pid = buffer.read<uint32_t>();
        auto new_ppid = buffer.read<uint32_t>();
        auto new_uid = buffer.read<uint32_t>();
        auto new_gid = buffer.read<uint32_t>();
        auto new_euid = buffer.read<uint32_t>();
        auto new_egid = buffer.read<uint32_t>();
        std::optional<int> new_exit_status{};
        buffer.read_optional(new_exit_status);
        auto new_current_working_directory = buffer.read<std::string>();
        auto new_executable_path = buffer.read<std::string>();
        auto new_next_tid = buffer.read<uint32_t>();
        auto new_argv = buffer.read_vector<std::string>();
        auto new_envp = buffer.read_vector<std::string>();
        auto new_auxv = buffer.read_vector<linux_auxv_entry>();

        this->fds = std::move(new_fds);
        this->directory_entries = std::move(new_directory_entries);
        this->directory_offsets = std::move(new_directory_offsets);
        this->epoll_instances = std::move(new_epoll_instances);
        this->brk_base = new_brk_base;
        this->brk_current = new_brk_current;
        this->threads = std::move(new_threads);
        this->active_thread = nullptr;
        if (active_tid != 0)
        {
            this->active_thread = &this->threads.at(active_tid);
        }
        this->pid = new_pid;
        this->ppid = new_ppid;
        this->uid = new_uid;
        this->gid = new_gid;
        this->euid = new_euid;
        this->egid = new_egid;
        this->exit_status = new_exit_status;
        this->current_working_directory = std::move(new_current_working_directory);
        this->executable_path = std::move(new_executable_path);
        this->next_tid = new_next_tid;
        this->argv = std::move(new_argv);
        this->envp = std::move(new_envp);
        this->auxv = std::move(new_auxv);
    }

} // namespace sogen
