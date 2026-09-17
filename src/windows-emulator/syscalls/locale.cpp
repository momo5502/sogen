#include "../std_include.hpp"
#include "../emulator_utils.hpp"
#include "../syscall_utils.hpp"
#include "../registry/registry_utils.hpp"

#include <charconv>

#include <utils/io.hpp>
#include <utils/string.hpp>

namespace sogen
{

    namespace syscalls
    {
        namespace
        {
            std::optional<uint32_t> parse_hex_identifier(std::u16string_view value)
            {
                while (!value.empty() && (value.front() == u' ' || value.front() == u'\t'))
                {
                    value.remove_prefix(1);
                }
                while (!value.empty() && (value.back() == u' ' || value.back() == u'\t'))
                {
                    value.remove_suffix(1);
                }

                if (value.starts_with(u"0x") || value.starts_with(u"0X"))
                {
                    value.remove_prefix(2);
                }

                if (value.empty())
                {
                    return std::nullopt;
                }

                const auto text = u16_to_u8(value);
                uint32_t result{};
                const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result, 16);
                if (error != std::errc{} || end != text.data() + text.size())
                {
                    return std::nullopt;
                }

                return result;
            }

            std::optional<uint32_t> read_hex_identifier(registry_manager& registry, const std::filesystem::path& key_path,
                                                        const std::string_view value_name)
            {
                const auto value = registry_utils::read_registry_string(registry, key_path, value_name);
                return value ? parse_hex_identifier(*value) : std::nullopt;
            }

            LCID get_system_locale_id(registry_manager& registry)
            {
                return read_hex_identifier(registry, R"(\Registry\Machine\System\CurrentControlSet\Control\Nls\Language)", "Default")
                    .value_or(0x407);
            }

            LCID get_user_locale_id(registry_manager& registry)
            {
                return read_hex_identifier(registry, R"(\Registry\User\Control Panel\International)", "Locale")
                    .value_or(get_system_locale_id(registry));
            }

            LANGID get_install_language_id(registry_manager& registry)
            {
                return static_cast<LANGID>(
                    read_hex_identifier(registry, R"(\Registry\Machine\System\CurrentControlSet\Control\Nls\Language)", "InstallLanguage")
                        .value_or(0x407));
            }

            std::vector<uint32_t> get_keyboard_layouts(registry_manager& registry)
            {
                std::vector<uint32_t> result{};
                const auto key = registry.get_key(utils::path_key{R"(\Registry\User\Keyboard Layout\Preload)"});
                if (key)
                {
                    for (size_t index = 1; index <= 64; ++index)
                    {
                        const auto value = registry_utils::read_registry_string(registry, *key, std::to_string(index));
                        if (!value)
                        {
                            break;
                        }

                        if (value->size() != 8)
                        {
                            continue;
                        }

                        if (const auto identifier = parse_hex_identifier(*value))
                        {
                            result.push_back(*identifier);
                        }
                    }
                }

                if (result.empty())
                {
                    result.push_back(0x00000407);
                }

                return result;
            }

            uint64_t keyboard_layout_to_handle(const uint32_t identifier)
            {
                const auto language = static_cast<uint16_t>(identifier & 0xFFFF);

                // Standard KLIDs have a zero high word. Windows represents those HKLs by
                // repeating the language identifier in both words (for example,
                // 00000416 -> 04160416). Preserve non-standard identifiers as-is until
                // per-layout device handles are emulated.
                if ((identifier & 0xFFFF0000) == 0)
                {
                    return (static_cast<uint64_t>(language) << 16) | language;
                }

                return identifier;
            }

            std::u16string keyboard_layout_to_name(const uint32_t identifier)
            {
                auto name = utils::string::to_hex_number(identifier, true);
                name.insert(0, 8 - name.size(), '0');
                return u8_to_u16(name);
            }

            uint64_t get_default_keyboard_layout(registry_manager& registry)
            {
                return keyboard_layout_to_handle(get_keyboard_layouts(registry).front());
            }
        }

        NTSTATUS handle_NtInitializeNlsFiles(const syscall_context& c, const emulator_object<uint64_t> base_address,
                                             const emulator_object<LCID> default_locale_id,
                                             const emulator_object<LARGE_INTEGER> /*default_casing_table_size*/)
        {
            const auto locale_file = utils::io::read_file(c.win_emu.file_sys.translate(R"(C:\Windows\System32\locale.nls)"));
            if (locale_file.empty())
            {
                return STATUS_FILE_INVALID;
            }

            const auto size = static_cast<size_t>(page_align_up(locale_file.size()));
            const auto base = c.win_emu.memory.allocate_memory(size, memory_permission::read);
            c.emu.write_memory(base, locale_file.data(), locale_file.size());

            base_address.write(base);
            default_locale_id.write(get_system_locale_id(c.win_emu.registry));

            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtQueryDefaultLocale(const syscall_context& c, const BOOLEAN user_profile,
                                             const emulator_object<LCID> default_locale_id)
        {
            default_locale_id.write(user_profile ? get_user_locale_id(c.win_emu.registry) : get_system_locale_id(c.win_emu.registry));
            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtGetNlsSectionPtr(const syscall_context& c, const ULONG section_type, const ULONG section_data,
                                           emulator_pointer /*context_data*/, const emulator_object<uint64_t> section_pointer,
                                           const emulator_object<ULONG> section_size)
        {
            if (section_type == 11)
            {
                const auto file_path = R"(C:\Windows\System32\C_)" + std::to_string(section_data) + ".NLS";
                const auto locale_file = utils::io::read_file(c.win_emu.file_sys.translate(file_path));
                if (locale_file.empty())
                {
                    return STATUS_OBJECT_NAME_NOT_FOUND;
                }

                const auto size = static_cast<size_t>(page_align_up(locale_file.size()));
                const auto section_memory = c.win_emu.memory.allocate_memory(size, memory_permission::read);
                c.emu.write_memory(section_memory, locale_file.data(), locale_file.size());

                section_pointer.write_if_valid(section_memory);
                section_size.write_if_valid(static_cast<ULONG>(size));

                return STATUS_SUCCESS;
            }

            c.win_emu.log.warn("Unsupported section type: %X\n", static_cast<uint32_t>(section_type));
            return STATUS_NOT_SUPPORTED;
        }

        NTSTATUS handle_NtGetMUIRegistryInfo()
        {
            return STATUS_NOT_SUPPORTED;
        }

        NTSTATUS handle_NtIsUILanguageComitted()
        {
            return STATUS_NOT_SUPPORTED;
        }

        uint64_t handle_NtUserActivateKeyboardLayout(const syscall_context& c, const uint64_t keyboard_layout, const uint32_t /*flags*/)
        {
            return keyboard_layout == 0 ? get_default_keyboard_layout(c.win_emu.registry) : keyboard_layout;
        }

        uint64_t handle_NtUserLoadKeyboardLayoutEx(const syscall_context& c, const handle /*file*/, const uint32_t /*table_offset*/,
                                                   const emulator_pointer /*tables*/, const uint64_t /*old_keyboard_layout*/,
                                                   const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> keyboard_layout_id,
                                                   const uint32_t /*new_keyboard_layout*/, const uint32_t /*flags*/)
        {
            if (keyboard_layout_id)
            {
                const auto requested_layout = read_unicode_string(c.emu, keyboard_layout_id);
                if (requested_layout.size() == 8)
                {
                    if (const auto identifier = parse_hex_identifier(requested_layout))
                    {
                        return keyboard_layout_to_handle(*identifier);
                    }
                }
            }

            return get_default_keyboard_layout(c.win_emu.registry);
        }

        uint64_t handle_NtUserGetKeyboardLayout(const syscall_context& c, const uint32_t /*thread_id*/)
        {
            return get_default_keyboard_layout(c.win_emu.registry);
        }

        uint32_t handle_NtUserGetKeyboardLayoutList(const syscall_context& c, const uint32_t buffer_count,
                                                    const emulator_pointer keyboard_layouts)
        {
            const auto layouts = get_keyboard_layouts(c.win_emu.registry);
            if (buffer_count == 0)
            {
                return static_cast<uint32_t>(layouts.size());
            }

            if (keyboard_layouts == 0)
            {
                return 0;
            }

            const auto count = std::min<size_t>(buffer_count, layouts.size());
            for (size_t i = 0; i < count; ++i)
            {
                const auto keyboard_layout = keyboard_layout_to_handle(layouts[i]);
                c.emu.write_memory(keyboard_layouts + i * sizeof(uint64_t), keyboard_layout);
            }

            return static_cast<uint32_t>(count);
        }

        BOOL handle_NtUserGetKeyboardLayoutName(const syscall_context& c, const emulator_pointer name)
        {
            if (name == 0)
            {
                return FALSE;
            }

            auto keyboard_layout_name = keyboard_layout_to_name(get_keyboard_layouts(c.win_emu.registry).front());
            keyboard_layout_name.push_back(u'\0');
            c.emu.write_memory(name, keyboard_layout_name.data(), keyboard_layout_name.size() * sizeof(char16_t));
            return TRUE;
        }

        NTSTATUS handle_NtQueryDefaultUILanguage(const syscall_context& c, const emulator_object<LANGID> language_id)
        {
            language_id.write(static_cast<LANGID>(get_user_locale_id(c.win_emu.registry)));
            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtQueryInstallUILanguage(const syscall_context& c, const emulator_object<LANGID> language_id)
        {
            language_id.write(get_install_language_id(c.win_emu.registry));
            return STATUS_SUCCESS;
        }
    }

} // namespace sogen
