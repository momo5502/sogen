#include "std_include.hpp"
#include "system_parameters.hpp"

#include "registry/registry_notification.hpp"
#include "syscall_utils.hpp"
#include "user_profile.hpp"

namespace sogen
{
    namespace
    {
        constexpr uint32_t default_filter_keys_flags = 0x52;
        constexpr uint32_t default_wait_ms = 1000;
        constexpr uint32_t default_repeat_ms = 500;
        constexpr uint32_t default_delay_ms = 1000;
        constexpr uint32_t default_sound_sentry_flags = 2;
        constexpr uint32_t default_sticky_keys_flags = 0x1D2;
        constexpr uint32_t default_mouse_keys_flags = 0x12;
        constexpr uint32_t default_mouse_keys_max_speed = 40;
        constexpr uint32_t default_mouse_keys_time_to_max_speed = 3000;
        constexpr uint32_t default_toggle_keys_flags = 0x12;
        constexpr uint32_t default_access_timeout_flags = 2;
        constexpr uint32_t default_access_timeout_ms = 300000;
        constexpr uint32_t default_high_contrast_flags = 0x52;
        constexpr size_t high_contrast_scheme_capacity = 128;
        constexpr DWORD min_mouse_keys_speed = 10;
        constexpr DWORD max_mouse_keys_speed = 360;
        constexpr DWORD min_mouse_keys_time_to_max_speed = 1000;
        constexpr DWORD max_mouse_keys_time_to_max_speed = 5000;
        constexpr size_t sm_showsounds = 70;
        constexpr DWORD srvif_keyboard_preference = 0x80;
        constexpr uint8_t key_toggled = 0x01;

        namespace accessibility_flag
        {
            constexpr DWORD available = 0x2;
            constexpr DWORD filter_keys_valid = 0x7F;
            constexpr DWORD toggle_keys_valid = 0x3F;
            constexpr DWORD sticky_keys_valid = 0x1FF;
            constexpr DWORD sticky_keys_state = 0xFFFF0000;
            constexpr DWORD mouse_keys_on = 0x1;
            constexpr DWORD mouse_keys_replace_numbers = 0x80;
            constexpr DWORD mouse_keys_valid = 0xFF;
            constexpr DWORD mouse_keys_reported = 0x4CFFFFFF;
            constexpr DWORD mouse_keys_left_button_selected = 0x10000000;
            constexpr DWORD mouse_keys_mouse_mode = 0x80000000;
            constexpr DWORD access_timeout_valid = 0x3;
            constexpr DWORD sound_sentry_valid = 0x7;
            constexpr DWORD high_contrast_no_theme_change = 0x1000;
        }

        bool in_range(const uint32_t value, const uint32_t minimum, const uint32_t maximum)
        {
            return value >= minimum && value <= maximum;
        }

        // win32k's gbMKMouseMode, as xxxUpdatePerUserAccessPackSettings, SPI_SETMOUSEKEYS and xxxMKToggleMouseKeys set it.
        void update_mouse_mode(accessibility_settings& settings, const bool num_lock_on)
        {
            const bool replaces_numbers = (settings.mouse_keys.dwFlags & accessibility_flag::mouse_keys_replace_numbers) != 0;
            settings.mouse_mode = replaces_numbers != num_lock_on;
        }

        bool num_lock_on(const std::array<uint8_t, 256>& key_state)
        {
            return (key_state[VK_NUMLOCK] & key_toggled) != 0;
        }

        // win32kbase's gPhysModifierState has one bit for each of these keys, in this order.
        constexpr std::array<uint8_t, 8> physical_modifier_keys{VK_LSHIFT, VK_RSHIFT, VK_LCONTROL, VK_RCONTROL,
                                                                VK_LMENU,  VK_RMENU,  VK_LWIN,     VK_RWIN};

        constexpr uint8_t physical_modifier_bit(const uint8_t key)
        {
            const auto index = static_cast<size_t>(std::ranges::find(physical_modifier_keys, key) - physical_modifier_keys.begin());
            return index == physical_modifier_keys.size() ? 0 : static_cast<uint8_t>(1u << index);
        }

        constexpr auto mouse_keys_hotkey_modifiers =
            static_cast<uint8_t>(physical_modifier_bit(VK_LSHIFT) | physical_modifier_bit(VK_LMENU));

        void publish_show_sounds(USER_SERVERINFO& server_info, const bool show_sounds)
        {
            server_info.systemMetrics[sm_showsounds] = show_sounds ? 1 : 0;
        }
    }

    // win32kbase's xxxUpdatePerUserAccessPackSettings, as a user logs on.
    void accessibility_settings::load(const user_profile& profile, const std::array<uint8_t, 256>& key_state)
    {
        using enum profile_section;

        this->filter_keys.dwFlags = profile.read_int(keyboard_response, "Flags", default_filter_keys_flags);
        this->filter_keys.iWaitMSec = profile.read_int(keyboard_response, "DelayBeforeAcceptance", default_wait_ms);
        this->filter_keys.iRepeatMSec = profile.read_int(keyboard_response, "AutoRepeatRate", default_repeat_ms);
        this->filter_keys.iDelayMSec = profile.read_int(keyboard_response, "AutoRepeatDelay", default_delay_ms);
        this->filter_keys.iBounceMSec = profile.read_int(keyboard_response, "BounceTime", 0);

        this->sound_sentry.dwFlags = profile.read_int(sound_sentry, "Flags", default_sound_sentry_flags);
        this->sound_sentry.iFSTextEffect = profile.read_int(sound_sentry, "FSTextEffect", 0);
        this->sound_sentry.iWindowsEffect = profile.read_int(sound_sentry, "WindowsEffect", 0);

        this->show_sounds = profile.read_int(show_sounds, "On", 0) != 0;

        this->sticky_keys.dwFlags = profile.read_int(sticky_keys, "Flags", default_sticky_keys_flags);

        this->mouse_keys.dwFlags = profile.read_int(mouse_keys, "Flags", default_mouse_keys_flags);
        this->mouse_keys.iMaxSpeed = profile.read_int(mouse_keys, "MaximumSpeed", default_mouse_keys_max_speed);
        this->mouse_keys.iTimeToMaxSpeed = profile.read_int(mouse_keys, "TimeToMaximumSpeed", default_mouse_keys_time_to_max_speed);
        if (!in_range(this->mouse_keys.iMaxSpeed, min_mouse_keys_speed, max_mouse_keys_speed))
        {
            this->mouse_keys.iMaxSpeed = default_mouse_keys_max_speed;
        }
        if (!in_range(this->mouse_keys.iTimeToMaxSpeed, min_mouse_keys_time_to_max_speed, max_mouse_keys_time_to_max_speed))
        {
            this->mouse_keys.iTimeToMaxSpeed = default_mouse_keys_time_to_max_speed;
        }
        update_mouse_mode(*this, num_lock_on(key_state));

        this->toggle_keys.dwFlags = profile.read_int(toggle_keys, "Flags", default_toggle_keys_flags);

        this->access_timeout.dwFlags = profile.read_int(time_out, "Flags", default_access_timeout_flags);
        this->access_timeout.iTimeOutMSec = profile.read_int(time_out, "TimeToWait", default_access_timeout_ms);

        this->high_contrast_flags = profile.read_int(high_contrast, "Flags", default_high_contrast_flags);
        if (auto scheme = profile.read_string(high_contrast, "High Contrast Scheme", high_contrast_scheme_capacity))
        {
            this->high_contrast_scheme = std::move(*scheme);
        }

        this->audio_description.cbSize = sizeof(EMU_AUDIODESCRIPTION);
        this->audio_description.Enabled = static_cast<BOOL>(profile.read_int(audio_description, "On", 0));
        this->audio_description.Locale = profile.read_int(audio_description, "Locale", 0);

        this->keyboard_preference = profile.read_int(keyboard_preference, "On", 0) != 0;
        this->screen_reader = profile.read_int(blind_access, "On", 0) != 0;
    }

    void accessibility_settings::publish(USER_SERVERINFO& server_info) const
    {
        publish_show_sounds(server_info, this->show_sounds);
        if (this->keyboard_preference)
        {
            server_info.dwSRVIFlags |= srvif_keyboard_preference;
        }
    }

    // win32kbase's ProcessKeyboardInputWorker and its MouseKeys filter, for the NumLock key that xxxMKToggleMouseKeys handles.
    void accessibility_settings::filter_key(const uint8_t key, const bool down, const std::array<uint8_t, 256>& key_state)
    {
        const auto modifier = physical_modifier_bit(key);
        this->physical_modifiers = static_cast<uint8_t>(down ? this->physical_modifiers | modifier : this->physical_modifiers & ~modifier);

        if (key != VK_NUMLOCK || (this->mouse_keys.dwFlags & accessibility_flag::mouse_keys_on) == 0)
        {
            return;
        }

        const bool repeated = this->mouse_keys_num_lock_down;
        this->mouse_keys_num_lock_down = down;
        if (down && !repeated && this->physical_modifiers != mouse_keys_hotkey_modifiers)
        {
            // The key state changes only after the low-level hooks, so it still holds NumLock's toggle from before this press.
            update_mouse_mode(*this, !num_lock_on(key_state));
        }
    }

    void accessibility_settings::serialize(utils::buffer_serializer& buffer) const
    {
        buffer.write(this->filter_keys);
        buffer.write(this->toggle_keys);
        buffer.write(this->sticky_keys);
        buffer.write(this->mouse_keys);
        buffer.write(this->access_timeout);
        buffer.write(this->sound_sentry);
        buffer.write(this->audio_description);
        buffer.write(this->high_contrast_flags);
        buffer.write_string(this->high_contrast_scheme);
        buffer.write(this->show_sounds);
        buffer.write(this->keyboard_preference);
        buffer.write(this->screen_reader);
        buffer.write(this->mouse_mode);
        buffer.write(this->mouse_keys_num_lock_down);
        buffer.write(this->physical_modifiers);
    }

    void accessibility_settings::deserialize(utils::buffer_deserializer& buffer)
    {
        buffer.read(this->filter_keys);
        buffer.read(this->toggle_keys);
        buffer.read(this->sticky_keys);
        buffer.read(this->mouse_keys);
        buffer.read(this->access_timeout);
        buffer.read(this->sound_sentry);
        buffer.read(this->audio_description);
        buffer.read(this->high_contrast_flags);
        buffer.read_string(this->high_contrast_scheme);
        buffer.read(this->show_sounds);
        buffer.read(this->keyboard_preference);
        buffer.read(this->screen_reader);
        buffer.read(this->mouse_mode);
        buffer.read(this->mouse_keys_num_lock_down);
        buffer.read(this->physical_modifiers);
    }

    namespace system_parameters
    {
        namespace
        {
            namespace spi_action
            {
                constexpr uint32_t getfilterkeys = 0x0032;
                constexpr uint32_t setfilterkeys = 0x0033;
                constexpr uint32_t gettogglekeys = 0x0034;
                constexpr uint32_t settogglekeys = 0x0035;
                constexpr uint32_t getmousekeys = 0x0036;
                constexpr uint32_t setmousekeys = 0x0037;
                constexpr uint32_t getshowsounds = 0x0038;
                constexpr uint32_t setshowsounds = 0x0039;
                constexpr uint32_t getstickykeys = 0x003A;
                constexpr uint32_t setstickykeys = 0x003B;
                constexpr uint32_t getaccesstimeout = 0x003C;
                constexpr uint32_t setaccesstimeout = 0x003D;
                constexpr uint32_t getsoundsentry = 0x0040;
                constexpr uint32_t setsoundsentry = 0x0041;
                constexpr uint32_t gethighcontrast = 0x0042;
                constexpr uint32_t sethighcontrast = 0x0043;
                constexpr uint32_t getkeyboardpref = 0x0044;
                constexpr uint32_t setkeyboardpref = 0x0045;
                constexpr uint32_t getscreenreader = 0x0046;
                constexpr uint32_t setscreenreader = 0x0047;
                constexpr uint32_t getaudiodescription = 0x0074;
                constexpr uint32_t setaudiodescription = 0x0075;
            }

            constexpr DWORD error_noaccess = 998;
            constexpr uint32_t output_alignment = 4;
            constexpr uint32_t input_alignment = 4;
            constexpr DWORD max_filter_keys_ms = 20000;
            constexpr DWORD max_access_timeout_ms = 3600000;
            constexpr DWORD max_sound_sentry_windows_effect = 3;
            constexpr uint32_t high_contrast_scheme_bytes = 0x100;
        }

        BOOL fail_with(const syscall_context& c, const DWORD error)
        {
            set_guest_last_error(c, error);
            return FALSE;
        }

        // win32kbase probes these outputs at 4 bytes (1 for a WOW64 caller), also the 8-byte HKL.
        bool is_misaligned_output(const syscall_context& c, const emulator_pointer value)
        {
            return !c.proc.is_wow64_process && value % output_alignment != 0;
        }

        BOOL write_output(const syscall_context& c, const emulator_pointer value, const void* data, const size_t size)
        {
            if (!is_writable_range(c.win_emu, value, size) || !c.win_emu.memory.try_write_memory(value, data, size))
            {
                return fail_with(c, error_noaccess);
            }

            return TRUE;
        }

        std::optional<uint32_t> probe_output_struct_size(const syscall_context& c, const emulator_pointer value, const size_t size)
        {
            uint32_t struct_size{};
            if (!is_writable_range(c.win_emu, value, size) || !c.win_emu.memory.try_read_memory(value, &struct_size, sizeof(struct_size)))
            {
                return std::nullopt;
            }

            return struct_size;
        }

        // Windows copies its zeroed output buffer back even though the call fails.
        bool write_zeroed_output(const syscall_context& c, const emulator_pointer value, const size_t size, const uint32_t struct_size)
        {
            std::vector<std::byte> zeroed(size);
            std::memcpy(zeroed.data(), &struct_size, sizeof(struct_size));
            return c.win_emu.memory.try_write_memory(value, zeroed.data(), zeroed.size());
        }

        namespace
        {
            bool takes_struct_size(const uint32_t param, const size_t size)
            {
                return param == 0 || param == size;
            }

            DWORD keep_availability(const DWORD requested, const DWORD current)
            {
                return (requested & ~accessibility_flag::available) | (current & accessibility_flag::available);
            }

            template <typename T>
            BOOL get_accessibility_struct(const syscall_context& c, T reported, const uint32_t param, const emulator_pointer value)
            {
                if (is_misaligned_output(c, value))
                {
                    return fail_with(c, error_noaccess);
                }

                const auto struct_size = probe_output_struct_size(c, value, sizeof(T));
                if (!struct_size)
                {
                    return fail_with(c, error_noaccess);
                }

                if (*struct_size == sizeof(T) && takes_struct_size(param, sizeof(T)))
                {
                    reported.cbSize = sizeof(T);
                    return write_output(c, value, &reported, sizeof(T));
                }

                return write_zeroed_output(c, value, sizeof(T), *struct_size) ? FALSE : fail_with(c, error_noaccess);
            }

            BOOL get_sticky_keys(const syscall_context& c, const uint32_t param, const emulator_pointer value)
            {
                auto sticky_keys = c.proc.accessibility.sticky_keys;
                sticky_keys.dwFlags &= ~accessibility_flag::sticky_keys_state;
                return get_accessibility_struct(c, sticky_keys, param, value);
            }

            BOOL get_mouse_keys(const syscall_context& c, const uint32_t param, const emulator_pointer value)
            {
                const auto& settings = c.proc.accessibility;
                auto mouse_keys = settings.mouse_keys;
                mouse_keys.dwFlags =
                    (mouse_keys.dwFlags & accessibility_flag::mouse_keys_reported) | accessibility_flag::mouse_keys_left_button_selected;
                if (settings.mouse_mode)
                {
                    mouse_keys.dwFlags |= accessibility_flag::mouse_keys_mouse_mode;
                }

                return get_accessibility_struct(c, mouse_keys, param, value);
            }

            BOOL get_high_contrast(const syscall_context& c, const emulator_pointer value)
            {
                EMU_HIGHCONTRAST high_contrast{};
                if (is_misaligned_output(c, value) || !is_writable_range(c.win_emu, value, sizeof(high_contrast)) ||
                    !c.win_emu.memory.try_read_memory(value, &high_contrast, sizeof(high_contrast)))
                {
                    return fail_with(c, error_noaccess);
                }

                const auto scheme_buffer = high_contrast.lpszDefaultScheme;
                if (scheme_buffer % sizeof(char16_t) != 0 || !is_writable_range(c.win_emu, scheme_buffer, high_contrast_scheme_bytes))
                {
                    return fail_with(c, error_noaccess);
                }

                const auto& settings = c.proc.accessibility;
                std::array<char16_t, high_contrast_scheme_bytes / sizeof(char16_t)> scheme{};
                std::ranges::copy(settings.high_contrast_scheme, scheme.begin());
                c.win_emu.memory.write_memory(scheme_buffer, scheme.data(), sizeof(scheme));

                high_contrast.dwFlags = settings.high_contrast_flags;
                return write_output(c, value, &high_contrast, sizeof(high_contrast));
            }

            template <typename T>
            std::optional<T> read_input_struct(const syscall_context& c, const emulator_pointer value)
            {
                T input{};
                if (value % input_alignment != 0 || !c.win_emu.memory.try_read_memory(value, &input, sizeof(input)))
                {
                    set_guest_last_error(c, error_noaccess);
                    return std::nullopt;
                }

                return input;
            }

            bool write_profile_value(const syscall_context& c, const profile_section section, const std::string_view name,
                                     const std::vector<std::byte>& data)
            {
                const auto key = user_profile{c.win_emu.registry}.writable_key(section);
                if (!key)
                {
                    return false;
                }

                registry_notification::set_value(c, *key, std::string{name}, REG_SZ, data);
                return true;
            }

            bool write_profile_int(const syscall_context& c, const profile_section section, const std::string_view name, const DWORD value)
            {
                return write_profile_value(c, section, name, user_profile::int_data(static_cast<int32_t>(value)));
            }

            bool write_profile_switch(const syscall_context& c, const profile_section section, const bool on)
            {
                return write_profile_value(c, section, "On", user_profile::string_data(on ? u"1" : u"0"));
            }

            bool all_written(const std::initializer_list<bool> results)
            {
                return std::ranges::all_of(results, std::identity{});
            }

            bool write_filter_keys(const syscall_context& c, const EMU_FILTERKEYS& keys)
            {
                using enum profile_section;
                return all_written({write_profile_int(c, keyboard_response, "Flags", keys.dwFlags),
                                    write_profile_int(c, keyboard_response, "DelayBeforeAcceptance", keys.iWaitMSec),
                                    write_profile_int(c, keyboard_response, "AutoRepeatDelay", keys.iDelayMSec),
                                    write_profile_int(c, keyboard_response, "AutoRepeatRate", keys.iRepeatMSec),
                                    write_profile_int(c, keyboard_response, "BounceTime", keys.iBounceMSec)});
            }

            bool write_mouse_keys(const syscall_context& c, const EMU_MOUSEKEYS& keys)
            {
                using enum profile_section;
                return all_written({write_profile_int(c, mouse_keys, "Flags", keys.dwFlags),
                                    write_profile_int(c, mouse_keys, "MaximumSpeed", keys.iMaxSpeed),
                                    write_profile_int(c, mouse_keys, "TimeToMaximumSpeed", keys.iTimeToMaxSpeed)});
            }

            bool write_access_timeout(const syscall_context& c, const EMU_ACCESSTIMEOUT& timeout)
            {
                using enum profile_section;
                return all_written({write_profile_int(c, time_out, "Flags", timeout.dwFlags),
                                    write_profile_int(c, time_out, "TimeToWait", timeout.iTimeOutMSec)});
            }

            // win32kfull's SetSoundSentry writes TextEffect, though logon reads FSTextEffect.
            bool write_sound_sentry(const syscall_context& c, const EMU_SOUNDSENTRY& sentry)
            {
                using enum profile_section;
                return all_written({write_profile_int(c, sound_sentry, "Flags", sentry.dwFlags),
                                    write_profile_int(c, sound_sentry, "TextEffect", sentry.iFSTextEffect),
                                    write_profile_int(c, sound_sentry, "WindowsEffect", sentry.iWindowsEffect)});
            }

            bool write_audio_description(const syscall_context& c, const EMU_AUDIODESCRIPTION& description)
            {
                using enum profile_section;
                return all_written({write_profile_switch(c, audio_description, description.Enabled != FALSE),
                                    write_profile_int(c, audio_description, "Locale", description.Locale)});
            }

            template <typename T>
            BOOL store_setting(const syscall_context& c, T& current, const T& requested, const bool update_ini,
                               bool (*write)(const syscall_context&, const T&))
            {
                if (update_ini && !write(c, requested))
                {
                    write(c, current);
                    return FALSE;
                }

                current = requested;
                current.cbSize = sizeof(T);
                return TRUE;
            }

            BOOL set_filter_keys(const syscall_context& c, const uint32_t param, const emulator_pointer value, const bool update_ini)
            {
                auto keys = read_input_struct<EMU_FILTERKEYS>(c, value);
                if (!keys)
                {
                    return FALSE;
                }

                auto& current = c.proc.accessibility.filter_keys;
                if (!takes_struct_size(param, sizeof(*keys)) || keys->cbSize != sizeof(*keys) ||
                    (keys->iWaitMSec != 0 && keys->iBounceMSec != 0) || (keys->dwFlags & ~accessibility_flag::filter_keys_valid) != 0)
                {
                    return FALSE;
                }

                keys->dwFlags = keep_availability(keys->dwFlags, current.dwFlags);
                if (std::max({keys->iWaitMSec, keys->iDelayMSec, keys->iRepeatMSec, keys->iBounceMSec}) > max_filter_keys_ms)
                {
                    return FALSE;
                }

                return store_setting(c, current, *keys, update_ini, write_filter_keys);
            }

            template <typename T>
            BOOL set_key_flags(const syscall_context& c, const uint32_t param, T keys, const bool update_ini, T& current,
                               const profile_section section, const DWORD valid_flags)
            {
                if (!takes_struct_size(param, sizeof(T)) || keys.cbSize != sizeof(T) || (keys.dwFlags & ~valid_flags) != 0)
                {
                    return FALSE;
                }

                keys.dwFlags = keep_availability(keys.dwFlags, current.dwFlags);
                if (update_ini && !write_profile_int(c, section, "Flags", keys.dwFlags))
                {
                    return FALSE;
                }

                current = keys;
                return TRUE;
            }

            BOOL set_toggle_keys(const syscall_context& c, const uint32_t param, const emulator_pointer value, const bool update_ini)
            {
                const auto keys = read_input_struct<EMU_TOGGLEKEYS>(c, value);
                if (!keys)
                {
                    return FALSE;
                }

                return set_key_flags(c, param, *keys, update_ini, c.proc.accessibility.toggle_keys, profile_section::toggle_keys,
                                     accessibility_flag::toggle_keys_valid);
            }

            BOOL set_sticky_keys(const syscall_context& c, const uint32_t param, const emulator_pointer value, const bool update_ini)
            {
                auto keys = read_input_struct<EMU_STICKYKEYS>(c, value);
                if (!keys)
                {
                    return FALSE;
                }

                keys->dwFlags &= ~accessibility_flag::sticky_keys_state;
                return set_key_flags(c, param, *keys, update_ini, c.proc.accessibility.sticky_keys, profile_section::sticky_keys,
                                     accessibility_flag::sticky_keys_valid);
            }

            BOOL set_mouse_keys(const syscall_context& c, const uint32_t param, const emulator_pointer value, const bool update_ini)
            {
                auto keys = read_input_struct<EMU_MOUSEKEYS>(c, value);
                if (!keys)
                {
                    return FALSE;
                }

                auto& settings = c.proc.accessibility;
                if (!takes_struct_size(param, sizeof(*keys)) || keys->cbSize != sizeof(*keys))
                {
                    return FALSE;
                }

                keys->dwFlags &= accessibility_flag::mouse_keys_reported;
                if ((keys->dwFlags & ~accessibility_flag::mouse_keys_valid) != 0)
                {
                    return FALSE;
                }

                keys->dwFlags = keep_availability(keys->dwFlags, settings.mouse_keys.dwFlags);
                if (!in_range(keys->iMaxSpeed, min_mouse_keys_speed, max_mouse_keys_speed) ||
                    !in_range(keys->iTimeToMaxSpeed, min_mouse_keys_time_to_max_speed, max_mouse_keys_time_to_max_speed))
                {
                    return FALSE;
                }

                if (!store_setting(c, settings.mouse_keys, *keys, update_ini, write_mouse_keys))
                {
                    return FALSE;
                }

                if ((settings.mouse_keys.dwFlags & accessibility_flag::mouse_keys_on) != 0)
                {
                    update_mouse_mode(settings, num_lock_on(c.proc.key_state));
                }

                return TRUE;
            }

            BOOL set_access_timeout(const syscall_context& c, const uint32_t param, const emulator_pointer value, const bool update_ini)
            {
                const auto timeout = read_input_struct<EMU_ACCESSTIMEOUT>(c, value);
                if (!timeout)
                {
                    return FALSE;
                }

                if (!takes_struct_size(param, sizeof(*timeout)) || timeout->cbSize != sizeof(*timeout) ||
                    (timeout->dwFlags & ~accessibility_flag::access_timeout_valid) != 0 || timeout->iTimeOutMSec > max_access_timeout_ms)
                {
                    return FALSE;
                }

                return store_setting(c, c.proc.accessibility.access_timeout, *timeout, update_ini, write_access_timeout);
            }

            BOOL set_sound_sentry(const syscall_context& c, const uint32_t param, const emulator_pointer value, const bool update_ini)
            {
                auto sentry = read_input_struct<EMU_SOUNDSENTRY>(c, value);
                if (!sentry)
                {
                    return FALSE;
                }

                auto& current = c.proc.accessibility.sound_sentry;
                if (!takes_struct_size(param, sizeof(*sentry)) || sentry->cbSize != sizeof(*sentry) ||
                    (sentry->dwFlags & ~accessibility_flag::sound_sentry_valid) != 0 ||
                    sentry->iWindowsEffect > max_sound_sentry_windows_effect || sentry->iFSTextEffect != 0 || sentry->iFSGrafEffect != 0)
                {
                    return FALSE;
                }

                sentry->dwFlags = keep_availability(sentry->dwFlags, current.dwFlags);
                return store_setting(c, current, *sentry, update_ini, write_sound_sentry);
            }

            BOOL set_audio_description(const syscall_context& c, const uint32_t param, const emulator_pointer value, const bool update_ini)
            {
                const auto description = read_input_struct<EMU_AUDIODESCRIPTION>(c, value);
                if (!description)
                {
                    return FALSE;
                }

                if (!takes_struct_size(param, sizeof(*description)) || description->cbSize != sizeof(*description))
                {
                    return FALSE;
                }

                return store_setting(c, c.proc.accessibility.audio_description, *description, update_ini, write_audio_description);
            }

            // win32k's tagINTERNALSETHIGHCONTRAST: user32 passes HIGHCONTRAST's scheme as a UNICODE_STRING.
            struct internal_set_high_contrast
            {
                UINT cbSize;
                DWORD dwFlags;
                UNICODE_STRING<EmulatorTraits<Emu64>> usDefaultScheme;
            };

            static_assert(sizeof(internal_set_high_contrast) == 24);

            BOOL set_high_contrast(const syscall_context& c, const emulator_pointer value, const bool update_ini)
            {
                internal_set_high_contrast request{};
                if (!c.win_emu.memory.try_read_memory(value, &request, sizeof(request)))
                {
                    return fail_with(c, error_noaccess);
                }

                const auto& string = request.usDefaultScheme;
                const auto buffer = string.Buffer;
                const auto probe_end = buffer + string.Length + sizeof(char16_t);
                if (buffer % sizeof(char16_t) != 0 || probe_end > c.win_emu.highest_user_address() || probe_end <= buffer ||
                    string.Length > string.MaximumLength || string.Length % sizeof(char16_t) != 0)
                {
                    return fail_with(c, error_noaccess);
                }

                const auto has_scheme = string.Length != 0 && buffer != 0;
                if (string.Length >= high_contrast_scheme_bytes)
                {
                    return update_ini ? FALSE : TRUE;
                }

                std::u16string scheme(string.Length / sizeof(char16_t), u'\0');
                if (has_scheme && !c.win_emu.memory.try_read_memory(buffer, scheme.data(), string.Length))
                {
                    return fail_with(c, error_noaccess);
                }
                scheme.resize(std::min(scheme.find(u'\0'), scheme.size()));

                if (update_ini)
                {
                    const auto flags_written = write_profile_int(c, profile_section::high_contrast, "Flags", request.dwFlags);
                    if (has_scheme)
                    {
                        write_profile_value(c, profile_section::high_contrast, "High Contrast Scheme", user_profile::string_data(scheme));
                    }

                    if (!flags_written)
                    {
                        return FALSE;
                    }
                }

                auto& settings = c.proc.accessibility;
                if (has_scheme)
                {
                    settings.high_contrast_scheme = std::move(scheme);
                }
                settings.high_contrast_flags = request.dwFlags & ~accessibility_flag::high_contrast_no_theme_change;
                return TRUE;
            }

            BOOL set_accessibility_switch(const syscall_context& c, const uint32_t action, const uint32_t param, const bool update_ini)
            {
                auto& settings = c.proc.accessibility;
                switch (action)
                {
                case spi_action::setshowsounds:
                    if (update_ini && !write_profile_int(c, profile_section::show_sounds, "On", param == 1 ? 1 : 0))
                    {
                        return FALSE;
                    }
                    settings.show_sounds = param == 1;
                    c.proc.user_handles.get_server_info().access(
                        [&](USER_SERVERINFO& server_info) { publish_show_sounds(server_info, settings.show_sounds); });
                    return TRUE;
                case spi_action::setkeyboardpref:
                    if (update_ini && !write_profile_switch(c, profile_section::keyboard_preference, param != 0))
                    {
                        return FALSE;
                    }
                    settings.keyboard_preference = param != 0;
                    return TRUE;
                default:
                    if (update_ini && !write_profile_switch(c, profile_section::blind_access, param != 0))
                    {
                        return FALSE;
                    }
                    settings.screen_reader = param != 0;
                    return TRUE;
                }
            }

            bool accessibility_switch(const syscall_context& c, const uint32_t action)
            {
                const auto& settings = c.proc.accessibility;
                switch (action)
                {
                case spi_action::getshowsounds:
                    return settings.show_sounds;
                case spi_action::getkeyboardpref:
                    return settings.keyboard_preference;
                default:
                    return settings.screen_reader;
                }
            }

            BOOL get_accessibility_switch(const syscall_context& c, const uint32_t action, const emulator_pointer value)
            {
                const DWORD on = accessibility_switch(c, action) ? TRUE : FALSE;
                return write_output(c, value, &on, sizeof(on));
            }
        }

        std::optional<BOOL> handle_accessibility_action(const syscall_context& c, const uint32_t action, const uint32_t param,
                                                        const emulator_pointer value, const bool update_ini)
        {
            const auto& settings = c.proc.accessibility;
            switch (action)
            {
            case spi_action::getfilterkeys:
                return get_accessibility_struct(c, settings.filter_keys, param, value);
            case spi_action::setfilterkeys:
                return set_filter_keys(c, param, value, update_ini);
            case spi_action::gettogglekeys:
                return get_accessibility_struct(c, settings.toggle_keys, param, value);
            case spi_action::settogglekeys:
                return set_toggle_keys(c, param, value, update_ini);
            case spi_action::getmousekeys:
                return get_mouse_keys(c, param, value);
            case spi_action::setmousekeys:
                return set_mouse_keys(c, param, value, update_ini);
            case spi_action::getstickykeys:
                return get_sticky_keys(c, param, value);
            case spi_action::setstickykeys:
                return set_sticky_keys(c, param, value, update_ini);
            case spi_action::getaccesstimeout:
                return get_accessibility_struct(c, settings.access_timeout, param, value);
            case spi_action::setaccesstimeout:
                return set_access_timeout(c, param, value, update_ini);
            case spi_action::getsoundsentry:
                return get_accessibility_struct(c, settings.sound_sentry, param, value);
            case spi_action::setsoundsentry:
                return set_sound_sentry(c, param, value, update_ini);
            case spi_action::gethighcontrast:
                return get_high_contrast(c, value);
            case spi_action::sethighcontrast:
                return set_high_contrast(c, value, update_ini);
            case spi_action::getaudiodescription:
                return get_accessibility_struct(c, settings.audio_description, param, value);
            case spi_action::setaudiodescription:
                return set_audio_description(c, param, value, update_ini);
            case spi_action::getshowsounds:
            case spi_action::getkeyboardpref:
            case spi_action::getscreenreader:
                return get_accessibility_switch(c, action, value);
            case spi_action::setshowsounds:
            case spi_action::setkeyboardpref:
            case spi_action::setscreenreader:
                return set_accessibility_switch(c, action, param, update_ini);
            default:
                return std::nullopt;
            }
        }
    }
}
