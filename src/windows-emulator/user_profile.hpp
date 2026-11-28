#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sogen
{
    class registry_manager;
    struct registry_key;

    enum class profile_section
    {
        sticky_keys,
        keyboard_response,
        mouse_keys,
        toggle_keys,
        time_out,
        sound_sentry,
        show_sounds,
        keyboard_preference,
        blind_access,
        high_contrast,
        audio_description,
    };

    class user_profile
    {
      public:
        explicit user_profile(registry_manager& registry);

        std::optional<std::u16string> read_string(profile_section section, std::string_view name, size_t capacity = 40) const;
        uint32_t read_int(profile_section section, std::string_view name, uint32_t default_value) const;

        std::optional<registry_key> writable_key(profile_section section) const;

        static std::vector<std::byte> string_data(std::u16string_view value);
        static std::vector<std::byte> int_data(int32_t value);

      private:
        registry_manager* registry_;
        std::filesystem::path user_root_;

        std::filesystem::path path(profile_section section) const;
    };

    uint32_t parse_profile_int(std::u16string_view text);
}
