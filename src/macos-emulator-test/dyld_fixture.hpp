#pragma once

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <platform/macho.hpp>

namespace macos_test
{
    struct macho_image_spec
    {
        uint32_t file_type{sogen::macho::MH_EXECUTE};
        uint64_t text_vmaddr{0x100000000ull};
        uint64_t page_zero_size{0x100000000ull};
        uint64_t entry_offset{0x200};
        std::string dylinker_path{};
        std::vector<uint32_t> code{};
        uint32_t cpu_subtype{sogen::macho::CPU_SUBTYPE_ARM64_ALL};

        // Real dyld refuses an executable that carries only LC_UNIXTHREAD -- "main executable is missing
        // LC_MAIN" -- so any image meant to be launched through it has to ask for the modern command.
        bool uses_lc_main{false};
    };

    inline std::vector<uint8_t> build_macho_image(const macho_image_spec& spec)
    {
        constexpr size_t page = 0x4000;

        const auto dylinker_command_size =
            spec.dylinker_path.empty()
                ? 0u
                : static_cast<uint32_t>((sizeof(sogen::macho::dylinker_command) + spec.dylinker_path.size() + 1 + 7) & ~7ull);

        const uint32_t segment_count = spec.page_zero_size ? 2 : 1;
        const uint32_t entry_command_size = spec.uses_lc_main ? static_cast<uint32_t>(sizeof(sogen::macho::entry_point_command)) : 288u;
        const uint32_t commands_size =
            segment_count * static_cast<uint32_t>(sizeof(sogen::macho::segment_command_64)) + entry_command_size + dylinker_command_size;

        std::vector<uint8_t> image(page, 0);

        sogen::macho::mach_header_64 header{};
        header.magic = sogen::macho::MH_MAGIC_64;
        header.cputype = sogen::macho::CPU_TYPE_ARM64;
        header.cpusubtype = spec.cpu_subtype;
        header.filetype = spec.file_type;
        header.ncmds = segment_count + 1 + (dylinker_command_size ? 1 : 0);
        header.sizeofcmds = commands_size;
        header.flags = sogen::macho::MH_NOUNDEFS;
        std::memcpy(image.data(), &header, sizeof(header));

        size_t cursor = sizeof(header);

        auto emit_segment = [&](const char* name, const uint64_t vmaddr, const uint64_t vmsize, const uint64_t fileoff,
                                const uint64_t filesize, const uint32_t prot) {
            sogen::macho::segment_command_64 segment{};
            segment.cmd = sogen::macho::LC_SEGMENT_64;
            segment.cmdsize = sizeof(segment);
            std::strncpy(segment.segname.data(), name, segment.segname.size());
            segment.vmaddr = vmaddr;
            segment.vmsize = vmsize;
            segment.fileoff = fileoff;
            segment.filesize = filesize;
            segment.maxprot = prot;
            segment.initprot = prot;
            std::memcpy(image.data() + cursor, &segment, sizeof(segment));
            cursor += sizeof(segment);
        };

        if (spec.page_zero_size)
        {
            emit_segment("__PAGEZERO", 0, spec.page_zero_size, 0, 0, 0);
        }

        emit_segment("__TEXT", spec.text_vmaddr, page, 0, page, sogen::macho::VM_PROT_READ | sogen::macho::VM_PROT_EXECUTE);

        if (spec.uses_lc_main)
        {
            sogen::macho::entry_point_command entry{};
            entry.cmd = sogen::macho::LC_MAIN;
            entry.cmdsize = sizeof(entry);
            entry.entryoff = spec.entry_offset;
            std::memcpy(image.data() + cursor, &entry, sizeof(entry));
            cursor += entry.cmdsize;
        }
        else
        {
            sogen::macho::thread_command thread{};
            thread.cmd = sogen::macho::LC_UNIXTHREAD;
            thread.cmdsize = 288;
            thread.flavor = sogen::macho::ARM_THREAD_STATE64;
            thread.count = sogen::macho::ARM_THREAD_STATE64_COUNT;
            std::memcpy(image.data() + cursor, &thread, sizeof(thread));

            sogen::macho::arm_thread_state64_t state{};
            state.pc = spec.text_vmaddr + spec.entry_offset;
            std::memcpy(image.data() + cursor + sizeof(thread), &state, sizeof(state));
            cursor += thread.cmdsize;
        }

        if (dylinker_command_size)
        {
            sogen::macho::dylinker_command dylinker{};
            dylinker.cmd = sogen::macho::LC_LOAD_DYLINKER;
            dylinker.cmdsize = dylinker_command_size;
            dylinker.name = sizeof(dylinker);
            std::memcpy(image.data() + cursor, &dylinker, sizeof(dylinker));
            std::memcpy(image.data() + cursor + sizeof(dylinker), spec.dylinker_path.c_str(), spec.dylinker_path.size() + 1);
            cursor += dylinker_command_size;
        }

        std::memcpy(image.data() + spec.entry_offset, spec.code.data(), spec.code.size() * sizeof(uint32_t));

        return image;
    }

    inline void write_image(const std::filesystem::path& path, const std::vector<uint8_t>& image)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream stream{path, std::ios::binary | std::ios::trunc};
        stream.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
    }
}
