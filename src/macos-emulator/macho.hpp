#pragma once

#include <array>
#include <cstdint>

// Layouts from XNU's <mach-o/fat.h>, <mach-o/loader.h> and <mach/i386/thread_status.h>.
namespace sogen::macho
{
    constexpr uint32_t FAT_MAGIC = 0xCAFEBABE;
    constexpr uint32_t FAT_MAGIC_64 = 0xCAFEBABF;
    constexpr uint64_t FAT_HEADER_SIZE = 8;
    constexpr uint64_t FAT_ARCH_SIZE = 20;
    constexpr uint64_t FAT_ARCH_64_SIZE = 32;

    constexpr uint32_t MH_MAGIC_64 = 0xFEEDFACF;
    constexpr uint32_t MH_EXECUTE = 0x2;
    constexpr uint32_t MH_DYLINKER = 0x7;
    constexpr uint32_t CPU_TYPE_X86_64 = 0x01000007;

    constexpr uint32_t LC_SEGMENT_64 = 0x19;
    constexpr uint32_t LC_UNIXTHREAD = 0x5;
    constexpr uint32_t LC_LOAD_DYLINKER = 0xE;
    constexpr uint32_t LC_MAIN = 0x80000028;

    constexpr uint32_t VM_PROT_READ = 0x1;
    constexpr uint32_t VM_PROT_WRITE = 0x2;
    constexpr uint32_t VM_PROT_EXECUTE = 0x4;

    constexpr uint32_t X86_THREAD_STATE64 = 4;

    struct mach_header_64
    {
        uint32_t magic;
        uint32_t cputype;
        uint32_t cpusubtype;
        uint32_t filetype;
        uint32_t ncmds;
        uint32_t sizeofcmds;
        uint32_t flags;
        uint32_t reserved;
    };

    struct load_command
    {
        uint32_t cmd;
        uint32_t cmdsize;
    };

    struct segment_command_64
    {
        uint32_t cmd;
        uint32_t cmdsize;
        std::array<char, 16> segname;
        uint64_t vmaddr;
        uint64_t vmsize;
        uint64_t fileoff;
        uint64_t filesize;
        uint32_t maxprot;
        uint32_t initprot;
        uint32_t nsects;
        uint32_t flags;
    };

    struct x86_thread_state64
    {
        uint64_t rax;
        uint64_t rbx;
        uint64_t rcx;
        uint64_t rdx;
        uint64_t rdi;
        uint64_t rsi;
        uint64_t rbp;
        uint64_t rsp;
        uint64_t r8;
        uint64_t r9;
        uint64_t r10;
        uint64_t r11;
        uint64_t r12;
        uint64_t r13;
        uint64_t r14;
        uint64_t r15;
        uint64_t rip;
        uint64_t rflags;
        uint64_t cs;
        uint64_t fs;
        uint64_t gs;
    };

    struct dylinker_command
    {
        uint32_t cmd;
        uint32_t cmdsize;
        uint32_t name_offset;
    };

    struct thread_command
    {
        uint32_t cmd;
        uint32_t cmdsize;
        uint32_t flavor;
        uint32_t count;
    };

    static_assert(sizeof(mach_header_64) == 32);
    static_assert(sizeof(segment_command_64) == 72);
    static_assert(sizeof(x86_thread_state64) == 168);
}
