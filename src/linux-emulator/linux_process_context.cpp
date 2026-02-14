#include "std_include.hpp"
#include "linux_process_context.hpp"
#include "linux_emulator_utils.hpp"

#include <address_utils.hpp>
#include <platform/elf.hpp>

using namespace elf;

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
                                  const uint64_t interpreter_base, const uint64_t initial_rip, const uint64_t vdso_base)
{
    // Store argv/envp for procfs emulation
    this->argv = argv_values;
    this->envp = envp_values;

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
            if (sec_end > highest_end)
            {
                highest_end = sec_end;
            }
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
    for (const auto& arg : argv_values)
    {
        argv_addrs.push_back(write_string_to_memory(memory, string_cursor, arg));
    }

    // Write envp strings
    std::vector<uint64_t> envp_addrs{};
    for (const auto& env : envp_values)
    {
        envp_addrs.push_back(write_string_to_memory(memory, string_cursor, env));
    }

    // Write 16 random bytes for AT_RANDOM
    const auto random_addr = string_cursor;
    {
        uint8_t random_bytes[16]{};
        // Fill with deterministic pseudo-random data
        for (int i = 0; i < 16; ++i)
        {
            random_bytes[i] = static_cast<uint8_t>((i * 73 + 0xAB) & 0xFF);
        }
        memory.write_memory(string_cursor, random_bytes, 16);
        string_cursor += 16;
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

    struct auxv_entry
    {
        uint64_t type;
        uint64_t value;
    };

    std::vector<auxv_entry> auxv{};
    auxv.push_back({AT_PHDR, exe.phdr_vaddr});
    auxv.push_back({AT_PHENT, exe.phdr_entry_size});
    auxv.push_back({AT_PHNUM, exe.phdr_count});
    auxv.push_back({AT_PAGESZ, 4096});
    auxv.push_back({AT_BASE, interpreter_base}); // Interpreter load address (0 if static)
    auxv.push_back({AT_ENTRY, exe.entry_point});
    auxv.push_back({AT_UID, this->uid});
    auxv.push_back({AT_EUID, this->euid});
    auxv.push_back({AT_GID, this->gid});
    auxv.push_back({AT_EGID, this->egid});
    auxv.push_back({AT_SECURE, 0});
    auxv.push_back({AT_RANDOM, random_addr});
    auxv.push_back({AT_PLATFORM, platform_addr});
    auxv.push_back({AT_CLKTCK, 100});
    if (vdso_base != 0)
    {
        auxv.push_back({AT_SYSINFO_EHDR, vdso_base});
    }
    auxv.push_back({AT_NULL, 0});

    // Total stack frame size (in uint64_t units):
    // 1 (argc) + argc (argv ptrs) + 1 (null) + envp.size() (envp ptrs) + 1 (null) + auxv.size()*2
    const auto total_u64s = 1 + argc + 1 + envp_values.size() + 1 + auxv.size() * 2;
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
    for (const auto& entry : auxv)
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
    constexpr size_t TCB_SIZE = 0xA00;                      // 2560 bytes for TCB + struct pthread (glibc's struct pthread is ~0x900 bytes)

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
