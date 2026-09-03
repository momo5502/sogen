#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <memory_interface.hpp>

namespace sogen
{
    constexpr size_t MACOS_TRACE_MAX_STRING = 4096;
    constexpr size_t MACOS_TRACE_MAX_BUFFER = 4096;

    struct macos_guest_text
    {
        std::string text{};
        bool truncated{};
    };

    // Byte at a time on purpose: a chunked read that straddles a page boundary fails wholesale, and a
    // trace has to show the readable prefix of a string whose tail runs off the end of a mapping.
    std::optional<macos_guest_text> read_bounded_guest_string(const memory_interface& memory, uint64_t address, size_t max_length);
    std::optional<std::vector<uint8_t>> read_bounded_guest_bytes(const memory_interface& memory, uint64_t address, size_t length);

    std::string escape_trace_text(std::string_view text);
    std::string quote_trace_text(const macos_guest_text& value);
    bool is_printable_trace_run(std::span<const uint8_t> bytes);
    std::string format_byte_preview(std::span<const uint8_t> bytes, size_t offset, size_t length, size_t total_length);
    std::string format_hex(uint64_t value);
}
