#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace macos_test
{
    // ADR is PC-relative with the 21-bit immediate split across immlo (bits 30:29) and immhi (bits 23:5).
    constexpr uint32_t adr_x1(const int32_t delta)
    {
        const auto imm = static_cast<uint32_t>(delta) & 0x1FFFFFU;
        return 0x10000001U | ((imm & 0x3U) << 29) | ((imm >> 2) << 5);
    }

    static_assert(adr_x1(0) == 0x10000001U);
    static_assert(adr_x1(16) == 0x10000081U);
    static_assert(adr_x1(20) == 0x100000A1U);
    static_assert(adr_x1(124) == 0x100003E1U);
    static_assert(adr_x1(-8) == 0x10FFFFC1U);

    constexpr uint32_t movz_x(const uint32_t rd, const uint16_t value)
    {
        return 0xD2800000U | (static_cast<uint32_t>(value) << 5) | (rd & 0x1FU);
    }

    static_assert(movz_x(0, 0) == 0xD2800000U);
    static_assert(movz_x(0, 1) == 0xD2800020U);
    static_assert(movz_x(0, 7) == 0xD28000E0U);
    static_assert(movz_x(2, 10) == 0xD2800142U);
    static_assert(movz_x(2, 14) == 0xD28001C2U);
    static_assert(movz_x(16, 1) == 0xD2800030U);
    static_assert(movz_x(16, 4) == 0xD2800090U);

    constexpr uint32_t svc_80 = 0xD4001001U;
    constexpr uint32_t brk_0 = 0xD4200000U;
    constexpr uint32_t ldr_x0_sp = 0xF94003E0U;
    constexpr uint32_t ldr_x1_sp_8 = 0xF94007E1U;

    constexpr uint32_t macos_sys_exit = 1;
    constexpr uint32_t macos_sys_write = 4;

    constexpr uint64_t hello_text_vmaddr = 0x100000000ULL;
    constexpr uint64_t hello_text_size = 0x4000ULL;
    constexpr uint64_t hello_code_offset = 0x200ULL;
    constexpr uint64_t hello_message_offset = 0x280ULL;
    constexpr std::string_view hello_message = "Hello, sogen!\n";

    constexpr size_t macho_header_size = 32;
    constexpr size_t macho_segment_command_size = 72;
    constexpr size_t macho_unixthread_command_size = 288;
    constexpr size_t macho_unixthread_pc_offset = 272;
    constexpr size_t macho_entry_pc_offset = macho_header_size + 2 * macho_segment_command_size + macho_unixthread_pc_offset;

    class byte_writer
    {
      public:
        explicit byte_writer(std::vector<std::byte>& data)
            : data_(&data)
        {
        }

        void at(const size_t offset, const void* source, const size_t size) const
        {
            std::memcpy(this->data_->data() + offset, source, size);
        }

        template <typename T>
        void value_at(const size_t offset, const T& value) const
        {
            static_assert(std::is_trivially_copyable_v<T>);
            this->at(offset, &value, sizeof(value));
        }

      private:
        std::vector<std::byte>* data_{};
    };

    inline std::vector<std::byte> build_macho_image(const std::span<const uint32_t> code)
    {
        std::vector<std::byte> image(static_cast<size_t>(hello_text_size), std::byte{0});
        const byte_writer writer{image};

        size_t offset = 0;
        writer.value_at<uint32_t>(offset + 0, 0xFEEDFACFU); // magic
        writer.value_at<uint32_t>(offset + 4, 0x0100000CU); // CPU_TYPE_ARM64
        writer.value_at<uint32_t>(offset + 8, 0U);          // CPU_SUBTYPE_ARM64_ALL
        writer.value_at<uint32_t>(offset + 12, 2U);         // MH_EXECUTE
        writer.value_at<uint32_t>(offset + 16, 3U);         // ncmds
        writer.value_at<uint32_t>(offset + 20,
                                  static_cast<uint32_t>(2 * macho_segment_command_size + macho_unixthread_command_size)); // sizeofcmds
        writer.value_at<uint32_t>(offset + 24, 1U);                                                                       // MH_NOUNDEFS
        writer.value_at<uint32_t>(offset + 28, 0U);                                                                       // reserved
        offset += macho_header_size;

        const auto write_segment = [&](const std::string_view name, const uint64_t vmaddr, const uint64_t vmsize, const uint64_t fileoff,
                                       const uint64_t filesize, const uint32_t maxprot, const uint32_t initprot) {
            writer.value_at<uint32_t>(offset + 0, 0x19U); // LC_SEGMENT_64
            writer.value_at<uint32_t>(offset + 4, static_cast<uint32_t>(macho_segment_command_size));
            std::array<char, 16> segment_name{};
            std::memcpy(segment_name.data(), name.data(), name.size());
            writer.at(offset + 8, segment_name.data(), segment_name.size());
            writer.value_at<uint64_t>(offset + 24, vmaddr);
            writer.value_at<uint64_t>(offset + 32, vmsize);
            writer.value_at<uint64_t>(offset + 40, fileoff);
            writer.value_at<uint64_t>(offset + 48, filesize);
            writer.value_at<uint32_t>(offset + 56, maxprot);
            writer.value_at<uint32_t>(offset + 60, initprot);
            writer.value_at<uint32_t>(offset + 64, 0U); // nsects
            writer.value_at<uint32_t>(offset + 68, 0U); // flags
            offset += macho_segment_command_size;
        };

        write_segment("__PAGEZERO", 0, hello_text_vmaddr, 0, 0, 0, 0);
        write_segment("__TEXT", hello_text_vmaddr, hello_text_size, 0, hello_text_size, 5, 5);

        writer.value_at<uint32_t>(offset + 0, 0x5U); // LC_UNIXTHREAD
        writer.value_at<uint32_t>(offset + 4, static_cast<uint32_t>(macho_unixthread_command_size));
        writer.value_at<uint32_t>(offset + 8, 6U);   // ARM_THREAD_STATE64
        writer.value_at<uint32_t>(offset + 12, 68U); // ARM_THREAD_STATE64_COUNT
        writer.value_at<uint64_t>(offset + macho_unixthread_pc_offset, hello_text_vmaddr + hello_code_offset);
        offset += macho_unixthread_command_size;

        writer.at(static_cast<size_t>(hello_code_offset), code.data(), code.size_bytes());
        writer.at(static_cast<size_t>(hello_message_offset), hello_message.data(), hello_message.size());

        return image;
    }

    // The trailing brk is a tripwire rather than padding: if exit fails to stop the emulator, execution
    // falls into it and the catch-all reports unhandled_cpu_exception instead of running off into zeroes.
    inline std::vector<std::byte> build_hello_world_macho(const uint16_t exit_status = 0)
    {
        const std::array<uint32_t, 9> code{
            movz_x(0, 1),                                                                 // mov  x0, #1
            adr_x1(static_cast<int32_t>(hello_message_offset - (hello_code_offset + 4))), // adr  x1, message
            movz_x(2, static_cast<uint16_t>(hello_message.size())),                       // mov  x2, #14
            movz_x(16, static_cast<uint16_t>(macos_sys_write)),                           // mov  x16, #4
            svc_80,                                                                       // svc  #0x80
            movz_x(0, exit_status),                                                       // mov  x0, #status
            movz_x(16, static_cast<uint16_t>(macos_sys_exit)),                            // mov  x16, #1
            svc_80,                                                                       // svc  #0x80
            brk_0,                                                                        // brk  #0
        };

        return build_macho_image(code);
    }

    // Writes argv[0] and exits with argc, so both observables come from the Darwin initial stack: argc at
    // sp and the argv vector immediately above it.
    inline std::vector<std::byte> build_stack_echo_macho(const uint16_t argv0_length)
    {
        const std::array<uint32_t, 9> code{
            ldr_x1_sp_8,                                        // ldr  x1, [sp, #8]
            movz_x(0, 1),                                       // mov  x0, #1
            movz_x(2, argv0_length),                            // mov  x2, #len
            movz_x(16, static_cast<uint16_t>(macos_sys_write)), // mov  x16, #4
            svc_80,                                             // svc  #0x80
            ldr_x0_sp,                                          // ldr  x0, [sp]
            movz_x(16, static_cast<uint16_t>(macos_sys_exit)),  // mov  x16, #1
            svc_80,                                             // svc  #0x80
            brk_0,                                              // brk  #0
        };

        return build_macho_image(code);
    }
}
