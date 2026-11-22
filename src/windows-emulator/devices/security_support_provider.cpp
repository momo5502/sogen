#include "../std_include.hpp"
#include "security_support_provider.hpp"
#include "security_support_provider_data.hpp"

#include "../ksec_memory_crypt.hpp"
#include "../windows_emulator.hpp"

namespace sogen
{

    namespace
    {
        constexpr uint32_t ksec_request_signature = 0x1a2b3c4d;
        constexpr uint32_t ksec_resolve_providers_function = 0x20000;
        constexpr uint32_t ksec_last_registry_function = 3;
        constexpr uint32_t ksec_configuration_function_class = 1;
        constexpr uint32_t ksec_resolve_function_class = 2;
        constexpr std::size_t ksec_minimum_request_size = 0x08;
        constexpr std::size_t ksec_minimum_reply_size = 0x08;
        constexpr std::size_t ksec_max_request_size = 0x1000;
        constexpr std::size_t ksec_in_place_reply_threshold = 216;
        constexpr std::size_t ksec_function_offset = 0x04;
        constexpr std::size_t ksec_interface_offset = 0x10;
        constexpr std::size_t ksec_mode_offset = 0x28;
        constexpr std::size_t ksec_flags_offset = 0x2c;
        constexpr std::size_t ksec_strings_offset = 0x30;
        constexpr std::array<std::size_t, 3> ksec_string_offset_fields{0x08, 0x18, 0x20};
        constexpr uint64_t ksec_no_string = std::numeric_limits<uint64_t>::max();
        constexpr std::size_t ksec_abbreviation_offset = 4;
        constexpr uint32_t ksec_default_marker = std::numeric_limits<uint32_t>::max();
        constexpr uint32_t ksec_first_mode = 1;
        constexpr uint32_t ksec_last_mode = 3;
        constexpr uint32_t ksec_user_mode_bit = 1;
        constexpr uint32_t ksec_kernel_mode_bit = 2;
        constexpr uint32_t ksec_all_functions_flag = 1;
        constexpr uint32_t ksec_valid_flags = 3;
        constexpr uint32_t ksec_property_type_dword = 4;
        constexpr std::size_t ksec_reply_array_offset = 0x10;
        constexpr std::size_t ksec_provider_reference_size = 0x38;
        constexpr std::size_t ksec_property_size = 0x18;
        constexpr std::size_t ksec_image_reference_size = 0x10;
        constexpr std::u16string_view ksec_key_length_property = u"KeyLength";
        constexpr std::u16string_view ksec_default_context = u"Default";
        constexpr std::u16string_view ksec_ssl_context = u"SSL";
        constexpr uint32_t ksec_ssl_interface = 0x10002;

        struct ksec_resolve_request
        {
            std::optional<std::u16string> context;
            std::optional<std::u16string> function;
            std::optional<std::u16string> provider;
            bool function_offset_present{};
            std::array<char16_t, 2> abbreviation{};
            uint32_t interface_id{};
            uint32_t mode{};
            uint32_t flags{};
        };

        struct ksec_registration
        {
            const ksecdd_data::function_registration* function{};
            const ksecdd_data::provider_registration* provider{};
            uint32_t interface_id{};
            uint32_t marker{};
        };

        bool equals_ignoring_case(const std::u16string_view left, const std::u16string_view right)
        {
            return std::ranges::equal(left, right, [](const char16_t a, const char16_t b) {
                const auto fold = [](const char16_t c) { return c >= u'a' && c <= u'z' ? static_cast<char16_t>(c - u'a' + u'A') : c; };
                return fold(a) == fold(b);
            });
        }

        uint64_t wire_offset(const std::optional<size_t> offset)
        {
            return offset ? static_cast<uint64_t>(*offset) : ksec_no_string;
        }

        template <typename T>
        T read_request_field(const std::span<const uint8_t> request, const size_t offset)
        {
            T value{};
            std::memcpy(&value, request.data() + offset, sizeof(value));
            return value;
        }

        std::optional<std::u16string> read_request_string(const std::span<const uint8_t> request, const size_t field)
        {
            const auto offset = read_request_field<uint64_t>(request, field);
            if (offset == ksec_no_string)
            {
                return std::nullopt;
            }

            if (offset >= request.size())
            {
                return std::nullopt;
            }

            std::u16string text;
            for (auto position = static_cast<size_t>(offset); position + sizeof(char16_t) <= request.size(); position += sizeof(char16_t))
            {
                const auto character =
                    static_cast<char16_t>(static_cast<uint16_t>(request[position]) | (static_cast<uint16_t>(request[position + 1]) << 8));
                if (character == u'\0')
                {
                    return text;
                }

                text.push_back(character);
            }

            return std::nullopt;
        }

        std::array<char16_t, 2> read_abbreviation(const std::span<const uint8_t> request)
        {
            std::array<char16_t, 2> abbreviation{};
            for (const auto field : ksec_string_offset_fields)
            {
                const auto offset = read_request_field<uint64_t>(request, field);
                if (offset == ksec_no_string)
                {
                    continue;
                }

                const auto start = static_cast<size_t>(offset & ~uint64_t{1}) + ksec_abbreviation_offset;
                if (start + sizeof(abbreviation) <= request.size())
                {
                    std::memcpy(abbreviation.data(), request.data() + start, sizeof(abbreviation));
                }

                break;
            }

            return abbreviation;
        }

        bool has_invalid_string(const std::span<const uint8_t> request)
        {
            return std::ranges::any_of(ksec_string_offset_fields, [&](const size_t field) {
                const auto offset = read_request_field<uint64_t>(request, field);
                if (offset == ksec_no_string)
                {
                    return false;
                }

                return offset < ksec_strings_offset || offset >= request.size() || !read_request_string(request, field);
            });
        }

        const ksecdd_data::function_registration* find_function(const std::u16string_view name)
        {
            for (const auto& function : ksecdd_data::functions)
            {
                if (equals_ignoring_case(function.name, name))
                {
                    return &function;
                }
            }

            return nullptr;
        }

        const ksecdd_data::interface_default* find_interface(const uint32_t interface_id)
        {
            for (const auto& interface_default : ksecdd_data::interface_defaults)
            {
                if (interface_default.interface_id == interface_id)
                {
                    return &interface_default;
                }
            }

            return nullptr;
        }

        bool context_contains_interface(const std::u16string_view context, const uint32_t interface_id)
        {
            return equals_ignoring_case(context, ksec_default_context) ||
                   (equals_ignoring_case(context, ksec_ssl_context) && interface_id == ksec_ssl_interface);
        }

        bool supports_requested_modes(const ksecdd_data::function_registration& function, const uint32_t mode)
        {
            return (mode & ksec_kernel_mode_bit) == 0 ||
                   (function.kernel_mode && !ksecdd_data::provider_of(function.provider).kernel_mode.name.empty());
        }

        class reply_writer
        {
          public:
            size_t size() const
            {
                return this->bytes_.size();
            }

            size_t append_zeros(const size_t count)
            {
                const auto offset = this->bytes_.size();
                this->bytes_.resize(offset + count);
                return offset;
            }

            template <typename T>
            size_t append(const T value)
            {
                const auto offset = this->append_zeros(sizeof(value));
                this->put(offset, value);
                return offset;
            }

            template <typename T>
            void put(const size_t offset, const T value)
            {
                std::memcpy(this->bytes_.data() + offset, &value, sizeof(value));
            }

            size_t append_string(const std::u16string_view text)
            {
                const auto size = (((text.size() + 1) * sizeof(char16_t)) + 7) & ~size_t{7};
                const auto offset = this->append_zeros(size);
                std::memcpy(this->bytes_.data() + offset, text.data(), text.size() * sizeof(char16_t));
                return offset;
            }

            size_t append_image(const ksecdd_data::image_registration& image)
            {
                const auto reference = this->append_zeros(ksec_image_reference_size);
                this->put(reference, wire_offset(this->append_string(image.name)));
                this->put(reference + sizeof(uint64_t), image.type);
                return reference;
            }

            std::vector<uint8_t> take()
            {
                return std::move(this->bytes_);
            }

          private:
            std::vector<uint8_t> bytes_;
        };

        std::vector<uint8_t> build_resolution_reply(const std::span<const ksec_registration> registrations, const uint32_t mode,
                                                    const std::array<char16_t, 2>& abbreviation)
        {
            reply_writer reply;
            reply.append(static_cast<uint32_t>(registrations.size()));
            reply.append(ksec_resolve_providers_function);
            reply.append(static_cast<uint64_t>(ksec_reply_array_offset));
            reply.append_zeros(registrations.size() * sizeof(uint64_t));

            for (size_t i = 0; i < registrations.size(); ++i)
            {
                const auto& [function, provider, interface_id, marker] = registrations[i];
                const auto reference = reply.append_zeros(ksec_provider_reference_size);
                reply.put(ksec_reply_array_offset + (i * sizeof(uint64_t)), wire_offset(reference));

                std::optional<size_t> function_name;
                if (function)
                {
                    function_name = reply.append_string(function->name);
                }

                const auto provider_name = reply.append_string(provider->name);

                std::optional<size_t> properties;
                if (function && function->key_length != 0)
                {
                    properties = reply.append_zeros(sizeof(uint64_t) + ksec_property_size);
                    const auto property = *properties + sizeof(uint64_t);
                    reply.put(*properties, wire_offset(property));
                    reply.put(property, wire_offset(reply.append_string(ksec_key_length_property)));
                    reply.put(property + sizeof(uint64_t), uint64_t{ksec_property_type_dword});
                    reply.put(property + (2 * sizeof(uint64_t)), wire_offset(reply.append(uint64_t{function->key_length})));
                }

                std::optional<size_t> user_mode_image;
                if ((mode & ksec_user_mode_bit) != 0)
                {
                    user_mode_image = reply.append_image(provider->user_mode);
                }

                std::optional<size_t> kernel_mode_image;
                if ((mode & ksec_kernel_mode_bit) != 0)
                {
                    kernel_mode_image = reply.append_image(provider->kernel_mode);
                }

                reply.put(reference, interface_id);
                reply.put(reference + 4, marker);
                reply.put(reference + 8, wire_offset(function_name));
                reply.put(reference + 16, wire_offset(provider_name));
                reply.put(reference + 24, static_cast<uint32_t>(properties ? 1 : 0));
                reply.put(reference + 28, abbreviation);
                reply.put(reference + 32, wire_offset(properties));
                reply.put(reference + 40, wire_offset(user_mode_image));
                reply.put(reference + 48, wire_offset(kernel_mode_image));
            }

            return reply.take();
        }

        std::optional<ksec_resolve_request> parse_resolve_request(const std::span<const uint8_t> request)
        {
            if (has_invalid_string(request))
            {
                return std::nullopt;
            }

            const auto read_name = [&](const size_t field) {
                auto name = read_request_string(request, field);
                return name && name->empty() ? std::nullopt : name;
            };

            ksec_resolve_request parsed{};
            parsed.context = read_name(ksec_string_offset_fields[0]);
            parsed.function = read_name(ksec_string_offset_fields[1]);
            parsed.provider = read_name(ksec_string_offset_fields[2]);
            parsed.function_offset_present = read_request_field<uint64_t>(request, ksec_string_offset_fields[1]) != ksec_no_string;
            parsed.interface_id = read_request_field<uint32_t>(request, ksec_interface_offset);
            parsed.mode = read_request_field<uint32_t>(request, ksec_mode_offset);
            parsed.flags = read_request_field<uint32_t>(request, ksec_flags_offset);
            parsed.abbreviation = read_abbreviation(request);
            return parsed;
        }

        const ksecdd_data::provider_registration& provider_of(const ksecdd_data::function_registration& function)
        {
            return ksecdd_data::provider_of(function.provider);
        }

        std::vector<ksec_registration> list_interface_functions(const uint32_t interface_id, const uint32_t mode, const uint32_t flags)
        {
            std::vector<ksec_registration> registrations;
            for (const auto& function : ksecdd_data::functions)
            {
                if (function.interface_id == interface_id && supports_requested_modes(function, mode))
                {
                    const auto marker = function.listing_marker == ksecdd_data::marker_request_flags ? flags : function.listing_marker;
                    registrations.push_back(
                        {.function = &function, .provider = &provider_of(function), .interface_id = interface_id, .marker = marker});
                }
            }

            return registrations;
        }

        NTSTATUS select_registrations(windows_emulator& win_emu, const ksec_resolve_request& request,
                                      std::vector<ksec_registration>& selected)
        {
            if (request.function)
            {
                if (request.interface_id != 0 && !find_interface(request.interface_id))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                const auto* function = find_function(*request.function);
                if (!function || (request.interface_id != 0 && function->interface_id != request.interface_id) ||
                    (request.provider && !equals_ignoring_case(*request.provider, provider_of(*function).name)) ||
                    (request.context && !context_contains_interface(*request.context, function->interface_id)) ||
                    !supports_requested_modes(*function, request.mode))
                {
                    return STATUS_NOT_FOUND;
                }

                selected.push_back(
                    {.function = function, .provider = &provider_of(*function), .interface_id = function->interface_id, .marker = 0});
                return STATUS_SUCCESS;
            }

            const auto* interface_default = find_interface(request.interface_id);
            if (!interface_default)
            {
                return STATUS_INVALID_PARAMETER;
            }

            if (request.context && !context_contains_interface(*request.context, request.interface_id))
            {
                return STATUS_NOT_FOUND;
            }

            const auto* default_function = find_function(interface_default->function);
            if (!default_function || !supports_requested_modes(*default_function, request.mode))
            {
                return STATUS_NOT_FOUND;
            }

            if (request.provider)
            {
                const auto registrations = list_interface_functions(request.interface_id, request.mode, request.flags);
                const auto match = std::ranges::find_if(registrations, [&](const ksec_registration& registration) {
                    return equals_ignoring_case(*request.provider, registration.provider->name);
                });
                if (match == registrations.end())
                {
                    return STATUS_NOT_FOUND;
                }

                selected.push_back({.function = nullptr,
                                    .provider = match->provider,
                                    .interface_id = request.interface_id,
                                    .marker = ksec_default_marker});
                return STATUS_SUCCESS;
            }

            if ((request.flags & ksec_all_functions_flag) != 0)
            {
                if (request.context)
                {
                    win_emu.log.error("Unverified KsecDD provider list for context %s\n", u16_to_u8(*request.context).c_str());
                }

                selected = list_interface_functions(request.interface_id, request.mode, request.flags);
                return STATUS_SUCCESS;
            }

            selected.push_back({.function = default_function,
                                .provider = &provider_of(*default_function),
                                .interface_id = request.interface_id,
                                .marker = request.function_offset_present ? 0 : ksec_default_marker});
            return STATUS_SUCCESS;
        }

        struct security_support_provider : stateless_device
        {
            static NTSTATUS complete_status_ioctl(windows_emulator& win_emu, const io_device_context& c, const NTSTATUS payload)
            {
                ULONG info = 0;
                if (c.output_buffer && c.output_buffer_length >= sizeof(payload))
                {
                    win_emu.emu().write_memory(c.output_buffer, &payload, sizeof(payload));
                    info = sizeof(payload);
                }

                if (c.io_status_block)
                {
                    IO_STATUS_BLOCK<EmulatorTraits<Emu64>> block{};
                    block.Information = info;
                    c.io_status_block.write(block);
                }

                return STATUS_SUCCESS;
            }

            static constexpr ULONG k_ioctl_encrypt_same_process = 0x39000E;
            static constexpr ULONG k_ioctl_decrypt_same_process = 0x390012;
            static constexpr ULONG k_ioctl_encrypt_cross_process = 0x390016;
            static constexpr ULONG k_ioctl_decrypt_cross_process = 0x39001A;
            static constexpr ULONG k_ioctl_encrypt_same_logon = 0x39001E;
            static constexpr ULONG k_ioctl_decrypt_same_logon = 0x390022;

            static bool is_memory_crypt_ioctl(const ULONG code)
            {
                switch (code)
                {
                case k_ioctl_encrypt_same_process:
                case k_ioctl_decrypt_same_process:
                case k_ioctl_encrypt_cross_process:
                case k_ioctl_decrypt_cross_process:
                case k_ioctl_encrypt_same_logon:
                case k_ioctl_decrypt_same_logon:
                    return true;
                default:
                    return false;
                }
            }

            static NTSTATUS handle_memory_crypt(windows_emulator& win_emu, const io_device_context& c)
            {
                const auto source = c.input_buffer ? c.input_buffer : c.output_buffer;
                const auto dest = c.output_buffer ? c.output_buffer : c.input_buffer;
                const auto in_len = static_cast<size_t>(c.input_buffer_length);
                const auto out_len = static_cast<size_t>(c.output_buffer_length);
                // Cross-process and same-logon ioctls can pass distinct buffers. Writing the
                // input length into a shorter output buffer overwrites adjacent guest memory.
                if (in_len != 0 && out_len != 0 && out_len < in_len)
                {
                    return STATUS_INVALID_PARAMETER;
                }

                const auto length = in_len != 0 ? in_len : out_len;
                if (!source || !dest || length == 0)
                {
                    return STATUS_INVALID_PARAMETER;
                }

                std::array<uint8_t, 0x1000> chunk{};
                static_assert(chunk.size() % k_ksec_memory_crypt_key.size() == 0);

                for (size_t offset = 0; offset < length; offset += chunk.size())
                {
                    const auto count = std::min(chunk.size(), length - offset);
                    const std::span<uint8_t> bytes{chunk.data(), count};
                    win_emu.emu().read_memory(source + offset, bytes.data(), bytes.size());
                    xor_ksec_memory(bytes);
                    win_emu.emu().write_memory(dest + offset, bytes.data(), bytes.size());
                }

                if (c.io_status_block)
                {
                    IO_STATUS_BLOCK<EmulatorTraits<Emu64>> block{};
                    block.Information = length;
                    c.io_status_block.write(block);
                }

                return STATUS_SUCCESS;
            }

            static NTSTATUS complete_with_error_status(windows_emulator& win_emu, const io_device_context& c, const NTSTATUS status)
            {
                win_emu.emu().write_memory(c.output_buffer, status);
                write_io_information(c, sizeof(status));
                return STATUS_BUFFER_OVERFLOW;
            }

            static NTSTATUS complete_with_reply(windows_emulator& win_emu, const io_device_context& c, const std::span<const uint8_t> reply)
            {
                if (c.output_buffer_length < reply.size())
                {
                    const std::array<uint32_t, 2> size_hint{
                        static_cast<uint32_t>(STATUS_BUFFER_TOO_SMALL),
                        static_cast<uint32_t>(reply.size()),
                    };
                    win_emu.emu().write_memory(c.output_buffer, size_hint);
                    write_io_information(c, sizeof(size_hint));
                    return STATUS_BUFFER_OVERFLOW;
                }

                win_emu.emu().write_memory(c.output_buffer, reply.data(), reply.size());
                write_io_information(c, reply.size());
                return STATUS_SUCCESS;
            }

            static void write_io_information(const io_device_context& c, const size_t information)
            {
                if (c.io_status_block)
                {
                    IO_STATUS_BLOCK<EmulatorTraits<Emu64>> block{};
                    block.Information = information;
                    c.io_status_block.write(block);
                }
            }

            static NTSTATUS resolve_providers(windows_emulator& win_emu, const io_device_context& c, const std::span<const uint8_t> request,
                                              const uint32_t function)
            {
                if (request.size() < ksec_strings_offset)
                {
                    return complete_with_error_status(
                        win_emu, c, request.size() == ksec_minimum_request_size ? STATUS_INVALID_PARAMETER : STATUS_INTERNAL_ERROR);
                }

                const auto parsed = parse_resolve_request(request);
                if (!parsed)
                {
                    return complete_with_error_status(win_emu, c, STATUS_INTERNAL_ERROR);
                }

                if (parsed->mode < ksec_first_mode || parsed->mode > ksec_last_mode || (parsed->flags & ~ksec_valid_flags) != 0)
                {
                    return complete_with_error_status(win_emu, c, STATUS_INVALID_PARAMETER);
                }

                if (function != ksec_resolve_providers_function)
                {
                    return STATUS_ILLEGAL_FUNCTION;
                }

                std::vector<ksec_registration> selected;
                const auto status = select_registrations(win_emu, *parsed, selected);
                if (status != STATUS_SUCCESS)
                {
                    if (parsed->function && !find_function(*parsed->function))
                    {
                        win_emu.log.error("Unsupported KsecDD function: %s\n", u16_to_u8(*parsed->function).c_str());
                    }

                    return complete_with_error_status(win_emu, c, status);
                }

                const auto reply = build_resolution_reply(selected, parsed->mode, parsed->abbreviation);
                return complete_with_reply(win_emu, c, reply);
            }

            static bool is_unmodelled_function(const uint32_t function)
            {
                return function <= ksec_last_registry_function || (function >> 16) == ksec_configuration_function_class;
            }

            NTSTATUS io_control(windows_emulator& win_emu, const io_device_context& c) override
            {
                if (is_memory_crypt_ioctl(c.io_control_code))
                {
                    return handle_memory_crypt(win_emu, c);
                }

                if (c.io_control_code != 0x390400)
                {
                    return STATUS_NOT_SUPPORTED;
                }

                if (!c.output_buffer || c.output_buffer_length < ksec_minimum_reply_size)
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                if (!c.input_buffer || c.input_buffer_length < ksec_minimum_request_size)
                {
                    return STATUS_ILLEGAL_FUNCTION;
                }

                const auto request_size = std::min<size_t>(c.input_buffer_length, ksec_max_request_size);
                std::vector<uint8_t> request(request_size);
                win_emu.emu().read_memory(c.input_buffer, request.data(), request.size());

                const auto function = read_request_field<uint32_t>(request, ksec_function_offset);
                if (read_request_field<uint32_t>(request, 0) != ksec_request_signature)
                {
                    return STATUS_ILLEGAL_FUNCTION;
                }

                if ((function >> 16) == ksec_resolve_function_class)
                {
                    return resolve_providers(win_emu, c, request, function);
                }

                if (!is_unmodelled_function(function))
                {
                    return STATUS_ILLEGAL_FUNCTION;
                }

                // Registry and configuration functions are not modelled; they only report success.
                const bool in_place = c.input_buffer == c.output_buffer;
                if (c.input_buffer_length < ksec_strings_offset || (in_place && c.output_buffer_length < ksec_in_place_reply_threshold))
                {
                    return complete_status_ioctl(win_emu, c, STATUS_SUCCESS);
                }

                return STATUS_SUCCESS;
            }
        };
    }

    std::unique_ptr<io_device> create_security_support_provider(const device_creation_context&)
    {
        return std::make_unique<security_support_provider>();
    }

} // namespace sogen
