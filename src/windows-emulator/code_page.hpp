#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sogen
{
    class windows_emulator;
    class registry_manager;

    namespace utils
    {
        class buffer_serializer;
        class buffer_deserializer;
    }

    class code_page
    {
      public:
        code_page() = default;
        explicit code_page(std::vector<std::byte> data);

        uint16_t id() const;
        uint16_t maximum_character_size() const;
        std::span<const std::byte> data() const;

        std::u16string decode(std::string_view text) const;
        std::string encode(std::u16string_view text, size_t maximum_bytes = std::numeric_limits<size_t>::max()) const;
        size_t encoded_size(std::u16string_view text) const;

        void serialize(utils::buffer_serializer& buffer) const;
        void deserialize(utils::buffer_deserializer& buffer);

      private:
        std::vector<std::byte> data_{};
        uint16_t id_{1252};
        uint16_t maximum_character_size_{1};
        size_t multibyte_offset_{};
        size_t unicode_offset_{};
        size_t dbcs_offset_{};

        uint16_t encode_character(char16_t character) const;
    };

    struct ansi_oem_tables
    {
        std::array<uint8_t, 256> ansi_to_oem{};
        std::array<uint8_t, 256> oem_to_ansi{};
    };

    std::optional<ansi_oem_tables> build_ansi_oem_tables(std::span<const std::byte> ansi, std::span<const std::byte> oem);
    uint64_t map_nls_data(windows_emulator& win_emu, std::span<const std::byte> data);
    void configure_process_code_pages(registry_manager& registry, uint32_t ansi_id, uint32_t oem_id);
    void initialize_process_code_pages(windows_emulator& win_emu);
    void initialize_user_code_page_tables(windows_emulator& win_emu);
}
