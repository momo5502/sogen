#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace sogen::sspi
{
    constexpr size_t tls_record_header_size = 5;

    struct tls_record_framing
    {
        size_t complete_size{};
        size_t missing_size{};
    };

    tls_record_framing frame_tls_records(std::span<const uint8_t> bytes);

    class tls_record_counter
    {
      public:
        bool observe(std::span<const uint8_t> record);
        uint64_t value() const;

      private:
        uint64_t value_{};
        bool active_{};
    };

    struct provider_context_input
    {
        uint32_t protocol{};
        uint32_t cipher_suite{};
        std::array<uint8_t, 16> inbound_raw_key{};
        std::array<uint8_t, 4> inbound_fixed_iv{};
        uint64_t inbound_sequence{};
        std::array<uint8_t, 16> outbound_raw_key{};
        std::array<uint8_t, 4> outbound_fixed_iv{};
        uint64_t outbound_sequence{};
        uint64_t serialized_context_flags{};
    };

    std::optional<std::vector<uint8_t>> build_provider_context(const provider_context_input& input);
}
