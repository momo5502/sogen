#include "cryptography.hpp"
#include "../windows_emulator.hpp"

#include <utils/finally.hpp>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace sogen
{
    namespace
    {
        class cryptography_device final : public io_device
        {
            uint64_t request_index_{};

            void serialize_object(utils::buffer_serializer& buffer) const override
            {
                buffer.write(this->request_index_);
            }

            void deserialize_object(utils::buffer_deserializer& buffer) override
            {
                buffer.read(this->request_index_);
            }

            NTSTATUS io_control(windows_emulator& win_emu, const io_device_context& context) override
            {
                if (context.io_control_code != 0x390008)
                {
                    return STATUS_NOT_SUPPORTED;
                }
                if (context.input_buffer_length || (context.output_buffer_length && !context.output_buffer))
                {
                    return STATUS_INVALID_PARAMETER;
                }
                if (!context.output_buffer_length)
                {
                    return STATUS_SUCCESS;
                }
                if (context.output_buffer > std::numeric_limits<uint64_t>::max() - context.output_buffer_length)
                {
                    return STATUS_ACCESS_VIOLATION;
                }

                mbedtls_entropy_context entropy{};
                mbedtls_ctr_drbg_context random{};
                mbedtls_entropy_init(&entropy);
                mbedtls_ctr_drbg_init(&random);
                const auto cleanup = utils::finally([&] {
                    mbedtls_ctr_drbg_free(&random);
                    mbedtls_entropy_free(&entropy);
                });
                std::array<uint8_t, MBEDTLS_CTR_DRBG_ENTROPY_LEN> deterministic_seed{};
                auto entropy_function = mbedtls_entropy_func;
                void* entropy_context = &entropy;
                if (win_emu.uses_relative_time())
                {
                    // Reproducible analysis must replay RNG output after restoring a snapshot, too.
                    const auto instructions = win_emu.get_executed_instructions();
                    const auto request_index = this->request_index_++;
                    std::memcpy(deterministic_seed.data(), &instructions, sizeof(instructions));
                    std::memcpy(deterministic_seed.data() + sizeof(instructions), &request_index, sizeof(request_index));
                    entropy_context = &deterministic_seed;
                    entropy_function = [](void* seed, unsigned char* output, const size_t size) {
                        const auto& bytes = *static_cast<std::array<uint8_t, MBEDTLS_CTR_DRBG_ENTROPY_LEN>*>(seed);
                        if (size > bytes.size())
                        {
                            return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
                        }
                        std::memcpy(output, bytes.data(), size);
                        return 0;
                    };
                }
                const auto seed_result = mbedtls_ctr_drbg_seed(&random, entropy_function, entropy_context, nullptr, 0);
                if (seed_result != 0)
                {
                    return STATUS_UNSUCCESSFUL;
                }

                std::array<uint8_t, MBEDTLS_CTR_DRBG_MAX_REQUEST> bytes{};
                uint32_t offset{};
                while (offset < context.output_buffer_length)
                {
                    const auto size = std::min<size_t>(bytes.size(), context.output_buffer_length - offset);
                    if (mbedtls_ctr_drbg_random(&random, bytes.data(), size) != 0)
                    {
                        return STATUS_UNSUCCESSFUL;
                    }
                    if (!win_emu.memory.try_write_memory(context.output_buffer + offset, bytes.data(), size))
                    {
                        return STATUS_ACCESS_VIOLATION;
                    }
                    offset += static_cast<uint32_t>(size);
                }
                if (context.io_status_block)
                {
                    context.io_status_block.access([&](auto& status) { status.Information = context.output_buffer_length; });
                }
                return STATUS_SUCCESS;
            }
        };
    }

    std::unique_ptr<io_device> create_cryptography_device(const device_creation_context&)
    {
        return std::make_unique<cryptography_device>();
    }
}
