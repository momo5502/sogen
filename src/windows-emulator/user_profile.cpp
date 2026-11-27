#include "std_include.hpp"
#include "user_profile.hpp"

#include "registry/registry_manager.hpp"
#include "registry/registry_utils.hpp"

namespace sogen
{
    namespace
    {
        std::u16string_view locate(const profile_section section)
        {
            switch (section)
            {
            case profile_section::sticky_keys:
                return u"Control Panel\\Accessibility\\StickyKeys";
            case profile_section::keyboard_response:
                return u"Control Panel\\Accessibility\\Keyboard Response";
            case profile_section::mouse_keys:
                return u"Control Panel\\Accessibility\\MouseKeys";
            case profile_section::toggle_keys:
                return u"Control Panel\\Accessibility\\ToggleKeys";
            case profile_section::time_out:
                return u"Control Panel\\Accessibility\\TimeOut";
            case profile_section::sound_sentry:
                return u"Control Panel\\Accessibility\\SoundSentry";
            case profile_section::show_sounds:
                return u"Control Panel\\Accessibility\\ShowSounds";
            case profile_section::keyboard_preference:
                return u"Control Panel\\Accessibility\\Keyboard Preference";
            case profile_section::blind_access:
                return u"Control Panel\\Accessibility\\Blind Access";
            case profile_section::high_contrast:
                return u"Control Panel\\Accessibility\\HighContrast";
            case profile_section::audio_description:
                return u"Control Panel\\Accessibility\\AudioDescription";
            }

            throw std::runtime_error("Unknown profile section");
        }
    }

    user_profile::user_profile(registry_manager& registry)
        : registry_(&registry),
          user_root_(std::filesystem::path{u"\\Registry\\User"} / registry_utils::get_user_sid_string(registry))
    {
    }

    std::filesystem::path user_profile::path(const profile_section section) const
    {
        return this->user_root_ / locate(section);
    }

    // FastGetProfileStringW reads the data as UTF-16 whatever its type, keeps at most capacity characters and puts the
    // terminator on the last one it keeps.
    std::optional<std::u16string> user_profile::read_string(const profile_section section, const std::string_view name,
                                                            const size_t capacity) const
    {
        const auto key = this->registry_->get_key(utils::path_key{this->path(section)});
        const auto value = key ? this->registry_->get_value(*key, name) : std::nullopt;
        if (!value)
        {
            return std::nullopt;
        }

        const auto kept = std::min(value->data.size() / sizeof(char16_t), capacity);
        if (kept == 0)
        {
            return std::nullopt;
        }

        std::u16string text(kept - 1, u'\0');
        std::memcpy(text.data(), value->data.data(), text.size() * sizeof(char16_t));
        text.resize(std::min(text.find(u'\0'), text.size()));
        return text;
    }

    uint32_t user_profile::read_int(const profile_section section, const std::string_view name, const uint32_t default_value) const
    {
        const auto text = this->read_string(section, name);
        return text ? parse_profile_int(*text) : default_value;
    }

    std::optional<registry_key> user_profile::writable_key(const profile_section section) const
    {
        return this->registry_->get_key(utils::path_key{this->path(section)});
    }

    std::vector<std::byte> user_profile::string_data(const std::u16string_view value)
    {
        std::vector<std::byte> data((value.size() + 1) * sizeof(char16_t));
        std::memcpy(data.data(), value.data(), value.size() * sizeof(char16_t));
        return data;
    }

    std::vector<std::byte> user_profile::int_data(const int32_t value)
    {
        const auto text = std::to_string(value);
        return string_data(std::u16string{text.begin(), text.end()});
    }

    // RtlUnicodeStringToInteger in base 10, as win32k reads its REG_SZ numbers.
    uint32_t parse_profile_int(const std::u16string_view text)
    {
        size_t position = 0;
        while (position < text.size() && text[position] <= u' ')
        {
            ++position;
        }

        bool negative = false;
        if (position < text.size() && (text[position] == u'+' || text[position] == u'-'))
        {
            negative = text[position] == u'-';
            ++position;
        }

        uint32_t value = 0;
        for (; position < text.size() && text[position] >= u'0' && text[position] <= u'9'; ++position)
        {
            value = value * 10 + static_cast<uint32_t>(text[position] - u'0');
        }

        return negative ? 0u - value : value;
    }
}
