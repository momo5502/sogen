#pragma once

#include "std_include.hpp"
#include "linux_memory_manager.hpp"

// Synthetic vDSO (Virtual Dynamic Shared Object) for the Linux emulator.
//
// The Linux kernel normally maps a small ELF shared object into every process's
// address space. This object contains fast-path implementations of a few
// syscalls (clock_gettime, gettimeofday, time, getcpu). Programs — especially
// glibc — discover the vDSO via the AT_SYSINFO_EHDR auxiliary vector entry and
// resolve symbols from it using standard ELF dynamic symbol lookup.
//
// Our synthetic vDSO contains stub functions that simply execute the real
// syscall instruction, which gets intercepted by the emulator's syscall hook
// and handled normally. This means programs that call through the vDSO will
// work identically to programs that call syscall directly.
//
// The vDSO is a valid minimal ELF64 shared object (ET_DYN) with:
//   - ELF header + program headers (PT_LOAD, PT_DYNAMIC)
//   - .text section with syscall stubs
//   - .dynsym + .dynstr + .hash for symbol resolution
//   - .dynamic section with required entries
//
// Exported symbols:
//   __vdso_clock_gettime
//   __vdso_gettimeofday
//   __vdso_time
//   __vdso_getcpu
//   clock_gettime        (alias)
//   gettimeofday         (alias)
//   time                 (alias)
//   getcpu               (alias)

class linux_vdso
{
  public:
    linux_vdso() = default;

    // Build and map the synthetic vDSO into emulated memory.
    // Returns the base address (AT_SYSINFO_EHDR value) or 0 on failure.
    uint64_t setup(linux_memory_manager& memory);

    // Get the base address of the mapped vDSO.
    uint64_t get_base() const
    {
        return this->base_address_;
    }

    // Get the total size of the vDSO image in bytes.
    size_t get_size() const
    {
        return this->image_size_;
    }

  private:
    uint64_t base_address_{};
    size_t image_size_{};
};
