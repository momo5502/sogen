#include "../std_include.hpp"
#include "security_support_provider.hpp"

#include "../ksec_memory_crypt.hpp"
#include "../windows_emulator.hpp"

#include <utils/string.hpp>

namespace sogen
{

    namespace
    {
        struct ksec_algorithm_request
        {
            std::array<uint8_t, 6> reserved0;
            uint16_t operation;
            std::array<uint8_t, 0x28> reserved1;
            std::array<char16_t, 8> algorithm_name;
        };

        static_assert(offsetof(ksec_algorithm_request, operation) == 6);
        static_assert(offsetof(ksec_algorithm_request, algorithm_name) == 0x30);
        static_assert(sizeof(ksec_algorithm_request) == 0x40);

        constexpr std::size_t ksec_algorithm_request_min_size = offsetof(ksec_algorithm_request, algorithm_name);

        struct security_support_provider : stateless_device
        {
            // RNG Microsoft Primitive Provider
            // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays)
            std::uint8_t rng_output_data[216] = //
                {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00,
                 0x00, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x50, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                 0x58, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
                 0xFF, 0xFF, 0xFF, 0xFF, 0x98, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                 0x52, 0x00, 0x4E, 0x00, 0x47, 0x00, 0x00, 0x00, 0x4D, 0x00, 0x69, 0x00, 0x63, 0x00, 0x72, 0x00, 0x6F, 0x00, 0x73, 0x00,
                 0x6F, 0x00, 0x66, 0x00, 0x74, 0x00, 0x20, 0x00, 0x50, 0x00, 0x72, 0x00, 0x69, 0x00, 0x6D, 0x00, 0x69, 0x00, 0x74, 0x00,
                 0x69, 0x00, 0x76, 0x00, 0x65, 0x00, 0x20, 0x00, 0x50, 0x00, 0x72, 0x00, 0x6F, 0x00, 0x76, 0x00, 0x69, 0x00, 0x64, 0x00,
                 0x65, 0x00, 0x72, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xA8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x62, 0x00, 0x63, 0x00, 0x72, 0x00, 0x79, 0x00, 0x70, 0x00, 0x74, 0x00,
                 0x70, 0x00, 0x72, 0x00, 0x69, 0x00, 0x6D, 0x00, 0x69, 0x00, 0x74, 0x00, 0x69, 0x00, 0x76, 0x00, 0x65, 0x00, 0x73, 0x00,
                 0x2E, 0x00, 0x64, 0x00, 0x6C, 0x00, 0x6C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

            // SHA256 Microsoft Primitive Provider
            // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays)
            std::uint8_t sha256_output_data[224] = //
                {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00,
                 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x50, 0x00, 0x00, 0x00, 0x00, 0x00,
                 0x00, 0x00, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x41, 0x00, 0x32, 0x00, 0xFF,
                 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xA0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
                 0xFF, 0xFF, 0xFF, 0xFF, 0x53, 0x00, 0x48, 0x00, 0x41, 0x00, 0x32, 0x00, 0x35, 0x00, 0x36, 0x00, 0x00, 0x00, 0x00,
                 0x00, 0x4D, 0x00, 0x69, 0x00, 0x63, 0x00, 0x72, 0x00, 0x6F, 0x00, 0x73, 0x00, 0x6F, 0x00, 0x66, 0x00, 0x74, 0x00,
                 0x20, 0x00, 0x50, 0x00, 0x72, 0x00, 0x69, 0x00, 0x6D, 0x00, 0x69, 0x00, 0x74, 0x00, 0x69, 0x00, 0x76, 0x00, 0x65,
                 0x00, 0x20, 0x00, 0x50, 0x00, 0x72, 0x00, 0x6F, 0x00, 0x76, 0x00, 0x69, 0x00, 0x64, 0x00, 0x65, 0x00, 0x72, 0x00,
                 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xB0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
                 0x00, 0x00, 0x00, 0x00, 0x00, 0x62, 0x00, 0x63, 0x00, 0x72, 0x00, 0x79, 0x00, 0x70, 0x00, 0x74, 0x00, 0x70, 0x00,
                 0x72, 0x00, 0x69, 0x00, 0x6D, 0x00, 0x69, 0x00, 0x74, 0x00, 0x69, 0x00, 0x76, 0x00, 0x65, 0x00, 0x73, 0x00, 0x2E,
                 0x00, 0x64, 0x00, 0x6C, 0x00, 0x6C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

            // MD5 Microsoft Primitive Provider (shared layout for the MD family)
            // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays)
            std::uint8_t md5_output_data[216] = //
                {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00,
                 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x50, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                 0x58, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x35, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
                 0xFF, 0xFF, 0xFF, 0xFF, 0x98, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                 0x4D, 0x00, 0x44, 0x00, 0x35, 0x00, 0x00, 0x00, 0x4D, 0x00, 0x69, 0x00, 0x63, 0x00, 0x72, 0x00, 0x6F, 0x00, 0x73, 0x00,
                 0x6F, 0x00, 0x66, 0x00, 0x74, 0x00, 0x20, 0x00, 0x50, 0x00, 0x72, 0x00, 0x69, 0x00, 0x6D, 0x00, 0x69, 0x00, 0x74, 0x00,
                 0x69, 0x00, 0x76, 0x00, 0x65, 0x00, 0x20, 0x00, 0x50, 0x00, 0x72, 0x00, 0x6F, 0x00, 0x76, 0x00, 0x69, 0x00, 0x64, 0x00,
                 0x65, 0x00, 0x72, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xA8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x62, 0x00, 0x63, 0x00, 0x72, 0x00, 0x79, 0x00, 0x70, 0x00, 0x74, 0x00,
                 0x70, 0x00, 0x72, 0x00, 0x69, 0x00, 0x6D, 0x00, 0x69, 0x00, 0x74, 0x00, 0x69, 0x00, 0x76, 0x00, 0x65, 0x00, 0x73, 0x00,
                 0x2E, 0x00, 0x64, 0x00, 0x6C, 0x00, 0x6C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

            // Offsets into a provider response. The algorithm name is a fixed-width
            // field rather than a packed string, and 0x34 holds a two-character
            // abbreviation of it that BCrypt cross-checks.
            static constexpr std::size_t response_name_offset = 0x50;
            static constexpr std::size_t response_abbreviation_offset = 0x34;
            static constexpr std::size_t response_abbreviation_size = 0x04;

            struct hash_algorithm
            {
                std::u16string_view name;
                // Third and fourth characters of the name: "SHA1" -> A1, "MD5" -> 5.
                std::u16string_view abbreviation;
                std::span<const std::uint8_t> response;
                // The provider name follows the algorithm name directly, so the
                // field is only as wide as the longest name sharing the template.
                std::size_t name_size;
            };

            std::optional<hash_algorithm> find_hash_algorithm(const std::u16string_view name)
            {
                const std::array<hash_algorithm, 7> algorithms{{
                    {.name = u"SHA1", .abbreviation = u"A1", .response = sha256_output_data, .name_size = 0x10},
                    {.name = u"SHA256", .abbreviation = u"A2", .response = sha256_output_data, .name_size = 0x10},
                    {.name = u"SHA384", .abbreviation = u"A3", .response = sha256_output_data, .name_size = 0x10},
                    {.name = u"SHA512", .abbreviation = u"A5", .response = sha256_output_data, .name_size = 0x10},
                    {.name = u"MD2", .abbreviation = u"2", .response = md5_output_data, .name_size = 0x08},
                    {.name = u"MD4", .abbreviation = u"4", .response = md5_output_data, .name_size = 0x08},
                    {.name = u"MD5", .abbreviation = u"5", .response = md5_output_data, .name_size = 0x08},
                }};

                for (const auto& algorithm : algorithms)
                {
                    if (algorithm.name == name)
                    {
                        return algorithm;
                    }
                }

                return std::nullopt;
            }

            static void patch_algorithm_name(std::vector<std::uint8_t>& response, const hash_algorithm& algorithm)
            {
                const auto write_utf16 = [&](const std::size_t offset, const std::u16string_view text) {
                    for (std::size_t i = 0; i < text.size(); ++i)
                    {
                        response[offset + (i * 2)] = static_cast<std::uint8_t>(text[i] & 0xFF);
                        response[offset + (i * 2) + 1] = static_cast<std::uint8_t>(text[i] >> 8);
                    }
                };

                std::fill_n(response.begin() + response_name_offset, algorithm.name_size, std::uint8_t{});
                std::fill_n(response.begin() + response_abbreviation_offset, response_abbreviation_size, std::uint8_t{});
                write_utf16(response_name_offset, algorithm.name);
                write_utf16(response_abbreviation_offset, algorithm.abbreviation);
            }

            NTSTATUS complete_status_ioctl(windows_emulator& win_emu, const io_device_context& c, const NTSTATUS payload)
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
                const auto length = c.input_buffer_length ? c.input_buffer_length : c.output_buffer_length;
                if (!source || !dest || length == 0)
                {
                    return STATUS_INVALID_PARAMETER;
                }

                std::vector<uint8_t> data(length);
                win_emu.emu().read_memory(source, data.data(), data.size());
                xor_ksec_memory(data);
                win_emu.emu().write_memory(dest, data.data(), data.size());
                if (c.io_status_block)
                {
                    IO_STATUS_BLOCK<EmulatorTraits<Emu64>> block{};
                    block.Information = data.size();
                    c.io_status_block.write(block);
                }

                return STATUS_SUCCESS;
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

                // BCryptOpenAlgorithmProvider uses a dedicated output buffer sized for the provider blob.
                // Other KsecDD clients issue 0x390400 in-place (same input/output pointer) with a small
                // buffer and expect DeviceIoControl to succeed with a NTSTATUS in the first dword.
                const bool in_place = c.input_buffer != 0 && c.input_buffer == c.output_buffer;
                if (!c.input_buffer || c.input_buffer_length < ksec_algorithm_request_min_size ||
                    (in_place && c.output_buffer_length < sizeof(rng_output_data)))
                {
                    return complete_status_ioctl(win_emu, c, STATUS_SUCCESS);
                }

                const auto request =
                    win_emu.emu().read_memory<ksec_algorithm_request>(c.input_buffer, static_cast<size_t>(c.input_buffer_length));

                if (request.operation != 2)
                {
                    return STATUS_SUCCESS;
                }

                // bcrypt sizes the request to the name it carries (0x38 bytes for "MD5"), so it can be shorter than
                // the struct. The field is guest-controlled and may not be NUL-terminated.
                const auto algorithm_name = utils::string::to_string_view<char16_t>(request.algorithm_name);

                const auto write_response = [&](const auto& output_data) -> NTSTATUS {
                    if (!c.output_buffer || c.output_buffer_length < sizeof(output_data))
                    {
                        return STATUS_BUFFER_TOO_SMALL;
                    }

                    win_emu.emu().write_memory(c.output_buffer, output_data);

                    if (c.io_status_block)
                    {
                        IO_STATUS_BLOCK<EmulatorTraits<Emu64>> block{};
                        block.Information = sizeof(output_data);
                        c.io_status_block.write(block);
                    }

                    return STATUS_SUCCESS;
                };

                // Hash providers differ from their same-length sibling only in the
                // algorithm name and a two-character abbreviation of it, so the two
                // captured layouts cover the whole family. Falling through to the
                // RNG response instead makes BCryptOpenAlgorithmProvider fail, which
                // takes CryptCreateHash with it.
                if (const auto hash = find_hash_algorithm(algorithm_name))
                {
                    if (!c.output_buffer || c.output_buffer_length < hash->response.size())
                    {
                        return STATUS_BUFFER_TOO_SMALL;
                    }

                    // Copy: the template is shared between algorithms of the same size.
                    std::vector<std::uint8_t> response{hash->response.begin(), hash->response.end()};
                    patch_algorithm_name(response, *hash);

                    win_emu.emu().write_memory(c.output_buffer, response.data(), response.size());

                    if (c.io_status_block)
                    {
                        IO_STATUS_BLOCK<EmulatorTraits<Emu64>> block{};
                        block.Information = response.size();
                        c.io_status_block.write(block);
                    }

                    return STATUS_SUCCESS;
                }

                return write_response(rng_output_data);
            }
        };
    }

    std::unique_ptr<io_device> create_security_support_provider(const device_creation_context&)
    {
        return std::make_unique<security_support_provider>();
    }

} // namespace sogen
