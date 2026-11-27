#pragma once

#include <serialization.hpp>
#include <platform/user.hpp>
#include <platform/window.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace sogen
{
    struct syscall_context;
    class user_profile;

    struct accessibility_settings
    {
        EMU_FILTERKEYS filter_keys{};
        EMU_TOGGLEKEYS toggle_keys{};
        EMU_STICKYKEYS sticky_keys{};
        EMU_MOUSEKEYS mouse_keys{};
        EMU_ACCESSTIMEOUT access_timeout{};
        EMU_SOUNDSENTRY sound_sentry{};
        EMU_AUDIODESCRIPTION audio_description{};
        DWORD high_contrast_flags{};
        std::u16string high_contrast_scheme{};
        bool show_sounds{};
        bool keyboard_preference{};
        bool screen_reader{};
        bool mouse_mode{};
        bool mouse_keys_num_lock_down{};
        uint8_t physical_modifiers{};

        void load(const user_profile& profile, const std::array<uint8_t, 256>& key_state);
        void publish(USER_SERVERINFO& server_info) const;
        void filter_key(uint8_t key, bool down, const std::array<uint8_t, 256>& key_state);

        void serialize(utils::buffer_serializer& buffer) const;
        void deserialize(utils::buffer_deserializer& buffer);
    };

    namespace system_parameters
    {
        BOOL fail_with(const syscall_context& c, DWORD error);
        bool is_misaligned_output(const syscall_context& c, uint64_t value);
        BOOL write_output(const syscall_context& c, uint64_t value, const void* data, size_t size);
        std::optional<uint32_t> probe_output_struct_size(const syscall_context& c, uint64_t value, size_t size);
        bool write_zeroed_output(const syscall_context& c, uint64_t value, size_t size, uint32_t struct_size);

        std::optional<BOOL> handle_accessibility_action(const syscall_context& c, uint32_t action, uint32_t param, uint64_t value,
                                                        bool update_ini);
    }
}
