#pragma once

#include "../std_include.hpp"
#include "../macos_memory_manager.hpp"

#include <platform/macho.hpp>

namespace sogen
{
    // dyld has no preferred base of its own (__TEXT vmaddr 0, no __PAGEZERO). This is where a real
    // private-mode launch on build 25G76 placed it, so traces line up with the host.
    constexpr uint64_t MACOS_DYLD_DEFAULT_BASE = 0x0000000105270000ULL;

    inline memory_permission macho_prot_to_permission(const uint32_t prot)
    {
        auto permission = memory_permission::none;

        if (prot & macho::VM_PROT_READ)
        {
            permission = permission | memory_permission::read;
        }

        if (prot & macho::VM_PROT_WRITE)
        {
            permission = permission | memory_permission::write;
        }

        if (prot & macho::VM_PROT_EXECUTE)
        {
            permission = permission | memory_permission::exec;
        }

        return permission;
    }

    struct macos_mapped_segment
    {
        std::string name{};
        uint64_t start{};
        size_t length{};
        uint64_t file_offset{};
        size_t file_length{};
        memory_permission initial_permissions{};
        memory_permission max_permissions{};
        uint32_t flags{};
    };

    struct macos_mapped_section
    {
        std::string name{};
        std::string segment_name{};
        uint64_t start{};
        size_t length{};
    };

    struct macos_mapped_module
    {
        std::string name{};
        std::filesystem::path path{};

        uint64_t slice_offset{};
        uint64_t image_base{};
        uint64_t preferred_base{};

        // image_base is where the mach header lands, which is the first FILE-BACKED segment's vmaddr and
        // therefore not necessarily the lowest one. The mapped extent is [image_start, image_start +
        // size_of_image); measuring containment from image_base instead would run past the last segment.
        uint64_t image_start{};
        uint64_t size_of_image{};
        uint64_t entry_point{};

        uint32_t cpu_type{};
        uint32_t cpu_subtype{};
        uint32_t file_type{};
        uint32_t flags{};

        uint64_t page_zero_size{};
        uint64_t stack_size{};

        std::optional<uint64_t> main_entry_offset{};
        std::optional<uint64_t> thread_entry{};

        std::string dylinker_path{};
        std::array<uint8_t, 16> uuid{};

        uint32_t platform{};
        uint32_t min_os{};
        uint32_t sdk{};

        std::vector<macos_mapped_segment> segments{};
        std::vector<macos_mapped_section> sections{};
        std::vector<std::string> dependent_libraries{};

        bool contains(const uint64_t address) const
        {
            return (address - this->image_start) < this->size_of_image;
        }

        bool is_arm64e() const
        {
            return (this->cpu_subtype & ~macho::CPU_SUBTYPE_MASK) == macho::CPU_SUBTYPE_ARM64E;
        }
    };

    uint64_t select_macho_slice(std::span<const std::byte> data, const std::filesystem::path& path);

    macos_mapped_module read_macho_module_metadata(std::span<const std::byte> data, const std::filesystem::path& path,
                                                   uint64_t slice_offset, uint64_t image_base);

    macos_mapped_module map_macho_from_data(macos_memory_manager& memory, std::span<const std::byte> data,
                                            const std::filesystem::path& path, uint64_t forced_base = 0);
}
