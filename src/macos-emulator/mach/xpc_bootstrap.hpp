#pragma once

#include "mach_msg.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sogen
{
    class macos_emulator;
}

namespace sogen::xpc
{
    constexpr uint32_t MAGIC = 0x40585043u;
    constexpr uint32_t VERSION = 5;

    namespace type
    {
        constexpr uint32_t null = 0x00001000u;
        constexpr uint32_t boolean = 0x00002000u;
        constexpr uint32_t int64 = 0x00003000u;
        constexpr uint32_t uint64 = 0x00004000u;
        constexpr uint32_t data = 0x00008000u;
        constexpr uint32_t string = 0x00009000u;
        // 16 raw bytes, no length prefix. Measured 2026-08-28 from the "instance" entry of libxpc's
        // 0x400000cf lookup, whose "name" entry follows it.
        constexpr uint32_t uuid = 0x0000A000u;
        // The placeholder a lookup reply carries for the port right; the right itself travels as a mach
        // descriptor. Measured 2026-08-27 from launchd's reply to a service lookup.
        constexpr uint32_t endpoint = 0x0000D000u;
        constexpr uint32_t dictionary = 0x0000F000u;
    }

    struct value;
    using dictionary = std::vector<std::pair<std::string, value>>;

    struct value
    {
        uint32_t type_tag{type::null};
        uint64_t number{};
        std::string text{};
        std::vector<uint8_t> bytes{};
        dictionary entries{};
        bool incomplete{};
    };

    value make_uint64(uint64_t v);
    value make_string(std::string v);
    value make_endpoint(uint32_t v);
    value make_data(std::vector<uint8_t> v);
    value make_dictionary(dictionary entries);

    std::vector<uint8_t> serialize(const value& root);
    // A tag with no measured wire shape stops the walk rather than mis-reading what follows it; the tag
    // is reported through unsupported_tag so the caller can name it.
    std::optional<value> parse(std::span<const uint8_t> bytes, uint32_t* unsupported_tag = nullptr);
    const value* find(const value& dictionary_value, std::string_view key);

    class bootstrap_responder
    {
      public:
        // The full reply message, header included, or nullopt when the routine is not one of the two
        // launchd routines sogen answers.
        static std::optional<std::vector<uint8_t>> respond(macos_emulator& emu, const mach::msg_call& call, std::span<const uint8_t> body);

        static bool is_xpc_routine(int32_t routine);
    };
}
