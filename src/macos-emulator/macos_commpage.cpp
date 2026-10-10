#include "macos_commpage.hpp"

#include <array>
#include <cstring>
#include <stdexcept>
#include <string_view>

namespace sogen
{
    namespace
    {
        constexpr size_t COMMPAGE_SIZE = 0x1000;

        // Offsets from XNU's osfmk/i386/cpu_capabilities.h (_COMM_PAGE_*).
        constexpr size_t SIGNATURE_OFFSET = 0x000;
        constexpr size_t CPU_CAPABILITIES64_OFFSET = 0x010;
        constexpr size_t VERSION_OFFSET = 0x01E;
        constexpr size_t CPU_CAPABILITIES_OFFSET = 0x020;
        constexpr size_t CACHE_LINESIZE_OFFSET = 0x026;
        constexpr size_t ACTIVE_CPUS_OFFSET = 0x034;
        constexpr size_t PHYSICAL_CPUS_OFFSET = 0x035;
        constexpr size_t LOGICAL_CPUS_OFFSET = 0x036;
        constexpr size_t CPU_CLUSTERS_OFFSET = 0x037;
        constexpr size_t MEMORY_SIZE_OFFSET = 0x038;
        constexpr size_t CPUFAMILY_OFFSET = 0x040;
        constexpr size_t KERNEL_PAGE_SHIFT_OFFSET = 0x04D;
        constexpr size_t USER_PAGE_SHIFT_64_OFFSET = 0x04E;
        constexpr size_t NT_SCALE_OFFSET = 0x058;
        constexpr size_t NT_SHIFT_OFFSET = 0x05C;
        constexpr size_t NT_GENERATION_OFFSET = 0x068;

        constexpr std::string_view SIGNATURE = "commpage 64-bit";
        constexpr uint16_t VERSION = 14;

        // MMX, SSE, SSE2, SSE3, Cache64, FastThreadLocalStorage, SSSE3, 64Bit, UP and one CPU. SSE4.x, AES and
        // AVX stay hidden, matching the CPU feature set Sogen exposes to Windows guests (see kusd_mmio.cpp).
        constexpr uint64_t CPU_CAPABILITIES = 0x183AF;
        constexpr uint16_t CACHE_LINESIZE = 64;

        // CPUFAMILY_INTEL_PENRYN. libsystem_platform picks its memmove/memset variants from the CPU family alone
        // and falls back to the AVX (Haswell) variants for an unknown family.
        constexpr uint32_t CPUFAMILY = 0x78EA4FBC;

        constexpr uint8_t PAGE_SHIFT = 12;

        // mach_absolute_time() converts the TSC with ((tsc << shift) * scale) >> 32, so these values make one
        // tick one nanosecond. It spins forever while the generation is zero.
        constexpr uint32_t NT_SCALE = 0x80000000;
        constexpr uint32_t NT_SHIFT = 1;
        constexpr uint32_t NT_GENERATION = 1;

        template <typename T>
        void write_field(std::array<std::byte, COMMPAGE_SIZE>& page, const size_t offset, const T& value)
        {
            std::memcpy(page.data() + offset, &value, sizeof(value));
        }

        std::array<std::byte, COMMPAGE_SIZE> build_commpage()
        {
            std::array<std::byte, COMMPAGE_SIZE> page{};

            std::memcpy(page.data() + SIGNATURE_OFFSET, SIGNATURE.data(), SIGNATURE.size());
            write_field(page, CPU_CAPABILITIES64_OFFSET, CPU_CAPABILITIES);
            write_field(page, VERSION_OFFSET, VERSION);
            write_field(page, CPU_CAPABILITIES_OFFSET, static_cast<uint32_t>(CPU_CAPABILITIES));
            write_field(page, CACHE_LINESIZE_OFFSET, CACHE_LINESIZE);
            write_field(page, ACTIVE_CPUS_OFFSET, MACOS_CPU_COUNT);
            write_field(page, PHYSICAL_CPUS_OFFSET, MACOS_CPU_COUNT);
            write_field(page, LOGICAL_CPUS_OFFSET, MACOS_CPU_COUNT);
            write_field(page, CPU_CLUSTERS_OFFSET, MACOS_CPU_COUNT);
            write_field(page, MEMORY_SIZE_OFFSET, MACOS_MEMORY_SIZE);
            write_field(page, CPUFAMILY_OFFSET, CPUFAMILY);
            write_field(page, KERNEL_PAGE_SHIFT_OFFSET, PAGE_SHIFT);
            write_field(page, USER_PAGE_SHIFT_64_OFFSET, PAGE_SHIFT);
            write_field(page, NT_SCALE_OFFSET, NT_SCALE);
            write_field(page, NT_SHIFT_OFFSET, NT_SHIFT);
            write_field(page, NT_GENERATION_OFFSET, NT_GENERATION);

            return page;
        }
    }

    void map_macos_commpage(posix_memory_manager& memory)
    {
        if (!memory.allocate_memory(MACOS_COMMPAGE_ADDRESS, COMMPAGE_SIZE, memory_permission::read_write))
        {
            throw std::runtime_error("Failed to map the commpage");
        }

        const auto page = build_commpage();
        memory.write_memory(MACOS_COMMPAGE_ADDRESS, page.data(), page.size());
        memory.protect_memory(MACOS_COMMPAGE_ADDRESS, COMMPAGE_SIZE, memory_permission::read);
    }
}
