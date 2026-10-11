#include "cryptography.hpp"
#include "../windows_emulator.hpp"

#include <utils/finally.hpp>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>

#include <algorithm>
#include <array>
#include <limits>

namespace sogen
{
    namespace
    {
        class cryptography_device final : public stateless_device
        {
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
                const auto seed_result = mbedtls_ctr_drbg_seed(&random, mbedtls_entropy_func, &entropy, nullptr, 0);
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
