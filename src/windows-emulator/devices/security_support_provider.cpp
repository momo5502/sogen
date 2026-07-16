#include "../std_include.hpp"
#include "security_support_provider.hpp"

#include "../windows_emulator.hpp"

#include <utils/string.hpp>

namespace sogen
{

    namespace
    {
        // Partial layout of the KsecDD ioctl 0x390400 request, covering the two fields the handler
        // consumes: the operation selector and the requested algorithm name.
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

            NTSTATUS io_control(windows_emulator& win_emu, const io_device_context& c) override
            {
                if (c.io_control_code != 0x390400)
                {
                    return STATUS_NOT_SUPPORTED;
                }

                // Real ntdll/bcrypt.dll packs this request as a variable-length blob: a fixed ~0x30-byte
                // header followed by just the requested algorithm name's own (length-dependent) tail -
                // NOT a constant sizeof(ksec_algorithm_request). A short name like "RSA"/"AES"/"DSA"
                // therefore produces a genuinely smaller buffer than our fixed struct (confirmed via a
                // live capture: exactly 0x38 bytes for "RSA" vs the struct's 0x40, while "SHA1" happens to
                // land right at 0x40) - requiring the full fixed-size struct here rejected every short
                // algorithm name with STATUS_INVALID_PARAMETER before its own name was ever inspected,
                // which is what made BCryptOpenAlgorithmProvider(L"RSA", ...) fail while the otherwise
                // identical BCryptOpenAlgorithmProvider(L"SHA1", ...) succeeded. Only require enough to
                // read `operation`; bound the algorithm_name read to whatever the guest actually sent.
                constexpr size_t min_request_size = offsetof(ksec_algorithm_request, operation) + sizeof(uint16_t);
                if (!c.input_buffer || c.input_buffer_length < min_request_size)
                {
                    return STATUS_INVALID_PARAMETER;
                }

                const auto operation = win_emu.emu().read_memory<uint16_t>(c.input_buffer + offsetof(ksec_algorithm_request, operation));

                if (operation == 1)
                {
                    // Precedes the op=2 provider-descriptor fetch below (the classic query-size-then-
                    // fetch-data idiom). The guest only ever asks for an 8-byte reply here; a value of 0
                    // reads as "no matching provider" and would abort the lookup before op=2 is ever
                    // issued, so report a single matching provider instead.
                    if (c.output_buffer && c.output_buffer_length >= sizeof(uint64_t))
                    {
                        constexpr uint64_t provider_count = 1;
                        win_emu.emu().write_memory(c.output_buffer, &provider_count, sizeof(provider_count));

                        if (c.io_status_block)
                        {
                            IO_STATUS_BLOCK<EmulatorTraits<Emu64>> block{};
                            block.Information = sizeof(provider_count);
                            c.io_status_block.write(block);
                        }
                    }

                    return STATUS_SUCCESS;
                }

                if (operation == 2)
                {
                    // algorithm_name is guest-controlled, may be shorter than the fixed array (see above -
                    // the guest only packs as many bytes as the name actually needs), and may not be
                    // NUL-terminated; read only what the guest declared, zero-fill the rest, and let the
                    // util bound the view to the fixed array so we never scan past it for a terminator.
                    constexpr size_t name_offset = offsetof(ksec_algorithm_request, algorithm_name);
                    std::array<char16_t, 8> algorithm_name_buffer{};
                    if (c.input_buffer_length > name_offset)
                    {
                        const size_t available = c.input_buffer_length - name_offset;
                        const size_t to_read = std::min(available, sizeof(algorithm_name_buffer));
                        win_emu.emu().try_read_memory(c.input_buffer + name_offset, algorithm_name_buffer.data(), to_read);
                    }
                    const auto algorithm_name = utils::string::to_string_view<char16_t>(algorithm_name_buffer);

                    // The response is a fixed-size record; never write past the guest-declared output buffer.
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

                    if (algorithm_name == u"SHA256")
                    {
                        return write_response(sha256_output_data);
                    }

                    if (algorithm_name == u"SHA1" || algorithm_name == u"MD5" || algorithm_name == u"MD4" || algorithm_name == u"MD2")
                    {
                        // All hash algorithms share the same structure; only the name differs.
                        std::array<std::uint8_t, sizeof(sha256_output_data)> hash_output_data{};
                        std::ranges::copy(sha256_output_data, hash_output_data.begin());
                        std::ranges::fill_n(hash_output_data.begin() + 0x50, 0x10, std::uint8_t{0});
                        std::memcpy(hash_output_data.data() + 0x50, algorithm_name.data(), algorithm_name.size() * sizeof(char16_t));

                        return write_response(hash_output_data);
                    }

                    if (algorithm_name == u"RSA")
                    {
                        // BCRYPT_ASYMMETRIC_ENCRYPTION_INTERFACE = 3
                        // rng_output_data has an 8-byte name slot at 0x50 ("RNG\0") — safe for 3-char names.
                        std::array<std::uint8_t, sizeof(rng_output_data)> rsa_output_data{};
                        std::ranges::copy(rng_output_data, rsa_output_data.begin());
                        rsa_output_data[0x18] = 0x03;
                        constexpr std::array<char16_t, 4> rsa_name{u'R', u'S', u'A', u'\0'};
                        std::memcpy(rsa_output_data.data() + 0x50, rsa_name.data(), rsa_name.size() * sizeof(char16_t));

                        return write_response(rsa_output_data);
                    }

                    if (algorithm_name == u"DSA")
                    {
                        // BCRYPT_SIGNATURE_INTERFACE = 5
                        std::array<std::uint8_t, sizeof(rng_output_data)> sig_output_data{};
                        std::ranges::copy(rng_output_data, sig_output_data.begin());
                        sig_output_data[0x18] = 0x05;
                        constexpr std::array<char16_t, 4> dsa_name{u'D', u'S', u'A', u'\0'};
                        std::memcpy(sig_output_data.data() + 0x50, dsa_name.data(), dsa_name.size() * sizeof(char16_t));

                        return write_response(sig_output_data);
                    }

                    if (algorithm_name == u"AES" || algorithm_name == u"DES" || algorithm_name == u"RC2" || algorithm_name == u"RC4")
                    {
                        // BCRYPT_CIPHER_INTERFACE = 1
                        std::array<std::uint8_t, sizeof(rng_output_data)> cipher_output_data{};
                        std::ranges::copy(rng_output_data, cipher_output_data.begin());
                        cipher_output_data[0x18] = 0x01;
                        std::ranges::fill_n(cipher_output_data.begin() + 0x50, 0x08, std::uint8_t{0});
                        std::memcpy(cipher_output_data.data() + 0x50, algorithm_name.data(), algorithm_name.size() * sizeof(char16_t));

                        return write_response(cipher_output_data);
                    }

                    return write_response(rng_output_data);
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
