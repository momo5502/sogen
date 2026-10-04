#pragma once

#include <memory>

#include <unicorn_arm64_emulator.hpp>

#include <macos_emulator.hpp>
#include <mach/mach_types.hpp>
#include <ranges>

namespace macos_test
{
    inline std::unique_ptr<sogen::arm64_mappable_emulator> make_backend()
    {
        return sogen::unicorn::create_arm64_emulator();
    }

    inline std::unique_ptr<sogen::macos_emulator> make_emulator()
    {
        return std::make_unique<sogen::macos_emulator>(macos_test::make_backend(), std::filesystem::path{});
    }

    inline void write_guest_code(sogen::macos_emulator& emu, const uint64_t base, const std::vector<uint32_t>& words)
    {
        if (!emu.memory.get_region_info(base).has_value())
        {
            emu.memory.allocate_memory(base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::all);
        }

        emu.memory.write_memory(base, words.data(), words.size() * sizeof(uint32_t));
        emu.emu().reg(sogen::arm64_register::pc, base);
    }

    inline std::vector<uint32_t> mach_trap_words(const uint32_t movz_word)
    {
        return {movz_word, 0xD4001001};
    }

    constexpr uint32_t movz_x(const uint32_t reg, const uint16_t imm, const uint32_t shift)
    {
        return 0xD2800000u | ((shift / 16) << 21) | (static_cast<uint32_t>(imm) << 5) | reg;
    }

    constexpr uint32_t movk_x(const uint32_t reg, const uint16_t imm, const uint32_t shift)
    {
        return 0xF2800000u | ((shift / 16) << 21) | (static_cast<uint32_t>(imm) << 5) | reg;
    }

    inline void load_x(std::vector<uint32_t>& words, const uint32_t reg, const uint64_t value)
    {
        words.push_back(movz_x(reg, static_cast<uint16_t>(value & 0xFFFFu), 0));
        for (uint32_t shift = 16; shift < 64; shift += 16)
        {
            const auto part = static_cast<uint16_t>((value >> shift) & 0xFFFFu);
            if (part != 0)
            {
                words.push_back(movk_x(reg, part, shift));
            }
        }
    }

    struct mach_msg2_args
    {
        uint64_t buffer{};
        uint64_t options{1 | 2};
        uint32_t bits{};
        uint32_t send_size{};
        uint32_t remote_port{};
        uint32_t local_port{};
        uint32_t voucher_port{};
        uint32_t id{};
        uint32_t descriptor_count{};
        uint32_t rcv_name{};
        uint32_t rcv_size{};
        uint32_t priority{};
        uint64_t timeout{};
    };

    constexpr uint64_t pack_pair(const uint32_t low, const uint32_t high)
    {
        return static_cast<uint64_t>(low) | (static_cast<uint64_t>(high) << 32);
    }

    inline std::vector<uint32_t> mach_msg2_words(const mach_msg2_args& args)
    {
        std::vector<uint32_t> words{};
        load_x(words, 0, args.buffer);
        load_x(words, 1, args.options);
        load_x(words, 2, pack_pair(args.bits, args.send_size));
        load_x(words, 3, pack_pair(args.remote_port, args.local_port));
        load_x(words, 4, pack_pair(args.voucher_port, args.id));
        load_x(words, 5, pack_pair(args.descriptor_count, args.rcv_name));
        load_x(words, 6, pack_pair(args.rcv_size, args.priority));
        load_x(words, 7, args.timeout);
        words.push_back(0x928005D0); // mov x16, #-47
        words.push_back(0xD4001001); // svc #0x80
        return words;
    }

    constexpr uint64_t MIG_CODE_BASE = 0x100000000ULL;
    constexpr uint64_t MIG_MSG_BASE = 0x340000000ULL;

    inline std::vector<uint8_t> send_mig_call(sogen::macos_emulator& emu, const uint32_t remote_port, const uint32_t id,
                                              const std::vector<uint8_t>& body, const uint32_t rcv_size, const bool complex = false)
    {
        emu.memory.allocate_memory(MIG_MSG_BASE, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        const auto send_size = static_cast<uint32_t>(sogen::mach::MSG_HEADER_SIZE + body.size());
        std::vector<uint8_t> message(send_size, 0);
        const auto reply_port = emu.mach.make_special_reply_port(1);
        sogen::mach::write_msg_header(
            message, {.bits = (complex ? sogen::mach::BITS_COMPLEX : 0u) |
                              sogen::mach::make_bits(sogen::mach::disposition::copy_send, sogen::mach::disposition::make_send_once),
                      .size = send_size,
                      .remote_port = remote_port,
                      .local_port = reply_port,
                      .voucher_port = 0,
                      .id = static_cast<int32_t>(id)});
        std::ranges::copy(body, message.begin() + sogen::mach::MSG_HEADER_SIZE);
        emu.memory.write_memory(MIG_MSG_BASE, message.data(), message.size());

        macos_test::mach_msg2_args args{};
        args.buffer = MIG_MSG_BASE;
        args.options = sogen::mach::msg_option::send_msg | sogen::mach::msg_option::rcv_msg;
        args.bits = sogen::mach::read_u32(message, 0);
        args.send_size = send_size;
        args.remote_port = remote_port;
        args.local_port = reply_port;
        args.id = id;
        args.descriptor_count = complex ? 1u : 0u;
        args.rcv_name = reply_port;
        args.rcv_size = rcv_size;

        // Unicorn caches translated blocks, so a second program written over the first at one address
        // silently re-executes the first. Every call gets its own page.
        static uint64_t next_code_page = MIG_CODE_BASE;
        const auto page = next_code_page;
        next_code_page += sogen::MACOS_PAGE_SIZE;
        emu.memory.allocate_memory(page, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::all);

        const auto words = macos_test::mach_msg2_words(args);
        macos_test::write_guest_code(emu, page, words);
        emu.start(words.size());

        std::vector<uint8_t> reply(rcv_size, 0);
        emu.memory.read_memory(MIG_MSG_BASE, reply.data(), reply.size());
        return reply;
    }

    inline std::vector<uint8_t> ndr_body(const std::vector<uint32_t>& words)
    {
        std::vector<uint8_t> body(sogen::mach::NDR_RECORD_SIZE + words.size() * 4, 0);
        std::ranges::copy(sogen::mach::NDR_RECORD, body.begin());
        for (size_t i = 0; i < words.size(); ++i)
        {
            sogen::mach::write_u32(body, sogen::mach::NDR_RECORD_SIZE + i * 4, words.at(i));
        }
        return body;
    }

    inline std::vector<uint32_t> mmap_file_sequence(const int guest_fd, const size_t length)
    {
        return {
            movz_x(0, 0, 0),                                                // addr hint
            movz_x(1, static_cast<uint16_t>(length & 0xFFFFu), 0),          // length, low half
            movk_x(1, static_cast<uint16_t>((length >> 16) & 0xFFFFu), 16), // length, high half
            movz_x(2, 1, 0),                                                // PROT_READ
            movz_x(3, 2, 0),                                                // MAP_PRIVATE
            movz_x(4, static_cast<uint16_t>(guest_fd), 0),
            movz_x(5, 0, 0),    // file offset
            movz_x(16, 197, 0), // mmap
            0xD4001001u,        // svc #0x80
            0xD4200000u,        // brk #0
        };
    }

    inline std::vector<uint32_t> syscall_sequence(const int64_t number, const std::vector<uint64_t>& arguments)
    {
        std::vector<uint32_t> words{};

        for (uint32_t reg = 0; reg < arguments.size(); ++reg)
        {
            const auto value = arguments[reg];
            words.push_back(movz_x(reg, static_cast<uint16_t>(value & 0xFFFFu), 0));

            for (uint32_t shift = 16; shift < 64; shift += 16)
            {
                const auto part = static_cast<uint16_t>((value >> shift) & 0xFFFFu);
                if (part != 0)
                {
                    words.push_back(movk_x(reg, part, shift));
                }
            }
        }

        if (number >= 0)
        {
            words.push_back(movz_x(16, static_cast<uint16_t>(number), 0));
        }
        else
        {
            // movn x16, #(-number - 1) — the encoding clang emits for a negative Mach trap index.
            words.push_back(0x92800010u | (static_cast<uint32_t>(-number - 1) << 5));
        }

        words.push_back(0xD4001001u);
        words.push_back(0xD4200000u);

        return words;
    }
}
