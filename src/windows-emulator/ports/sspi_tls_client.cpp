#include "../std_include.hpp"
#include "sspi_tls_client.hpp"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ssl.h>

namespace sogen::sspi
{
    struct tls_client::impl
    {
        mbedtls_ssl_context ssl{};
        mbedtls_ssl_config config{};
        mbedtls_entropy_context entropy{};
        mbedtls_ctr_drbg_context random{};
        std::vector<uint8_t> input{};
        size_t input_offset{};
        std::vector<uint8_t>* output{};
        tls_record_counter inbound_counter{};
        tls_record_counter outbound_counter{};
        std::array<uint8_t, 48> master_secret{};
        std::array<uint8_t, 32> client_random{};
        std::array<uint8_t, 32> server_random{};
        mbedtls_tls_prf_types prf{MBEDTLS_SSL_TLS_PRF_NONE};
        bool exported{};
        bool complete{};
        bool handoff_taken{};

        impl()
        {
            mbedtls_ssl_init(&ssl);
            mbedtls_ssl_config_init(&config);
            mbedtls_entropy_init(&entropy);
            mbedtls_ctr_drbg_init(&random);
        }

        ~impl()
        {
            mbedtls_platform_zeroize(master_secret.data(), master_secret.size());
            mbedtls_ssl_free(&ssl);
            mbedtls_ssl_config_free(&config);
            mbedtls_ctr_drbg_free(&random);
            mbedtls_entropy_free(&entropy);
        }

        static int send(void* context, const unsigned char* bytes, const size_t size)
        {
            auto& self = *static_cast<impl*>(context);
            if (self.output == nullptr || size > static_cast<size_t>(std::numeric_limits<int>::max()))
            {
                return MBEDTLS_ERR_SSL_ALLOC_FAILED;
            }
            try
            {
                self.output->insert(self.output->end(), bytes, bytes + size);
            }
            catch (...)
            {
                return MBEDTLS_ERR_SSL_ALLOC_FAILED;
            }
            return static_cast<int>(size);
        }

        static int receive(void* context, unsigned char* bytes, const size_t size)
        {
            auto& self = *static_cast<impl*>(context);
            const size_t available = self.input.size() - self.input_offset;
            if (available == 0)
            {
                return MBEDTLS_ERR_SSL_WANT_READ;
            }
            const size_t received = std::min(size, available);
            std::memcpy(bytes, self.input.data() + self.input_offset, received);
            self.input_offset += received;
            if (self.input_offset == self.input.size())
            {
                self.input.clear();
                self.input_offset = 0;
            }
            return static_cast<int>(received);
        }

        static void export_keys(void* context, const mbedtls_ssl_key_export_type type, const unsigned char* secret,
                                const size_t secret_size, const unsigned char* client, const unsigned char* server,
                                const mbedtls_tls_prf_types key_prf)
        {
            auto& self = *static_cast<impl*>(context);
            if (type != MBEDTLS_SSL_KEY_EXPORT_TLS12_MASTER_SECRET || secret_size != self.master_secret.size())
            {
                return;
            }
            std::memcpy(self.master_secret.data(), secret, self.master_secret.size());
            std::memcpy(self.client_random.data(), client, self.client_random.size());
            std::memcpy(self.server_random.data(), server, self.server_random.size());
            self.prf = key_prf;
            self.exported = true;
        }

        bool count_outbound(const std::span<const uint8_t> records)
        {
            if (records.empty())
            {
                return true;
            }
            const auto framing = frame_tls_records(records);
            if (framing.missing_size != 0 || framing.complete_size != records.size())
            {
                return false;
            }
            size_t offset{};
            while (offset < records.size())
            {
                const size_t size = tls_record_header_size + (static_cast<size_t>(records[offset + 3]) << 8) + records[offset + 4];
                if (!outbound_counter.observe(records.subspan(offset, size)))
                {
                    return false;
                }
                offset += size;
            }
            return true;
        }

        handshake_status advance(std::vector<uint8_t>& output_bytes)
        {
            const size_t output_start = output_bytes.size();
            output = &output_bytes;
            const int status = mbedtls_ssl_handshake(&ssl);
            output = nullptr;
            if (!count_outbound(std::span<const uint8_t>{output_bytes}.subspan(output_start)))
            {
                return handshake_status::failed;
            }
            if (status == 0)
            {
                complete = true;
                return handshake_status::complete;
            }
            if (status == MBEDTLS_ERR_SSL_WANT_READ || status == MBEDTLS_ERR_SSL_WANT_WRITE)
            {
                return handshake_status::continue_needed;
            }
            return handshake_status::failed;
        }

        std::optional<provider_context_input> extract_handoff()
        {
            constexpr uint32_t tls_1_2 = 0x0303;
            const auto cipher_suite = mbedtls_ssl_get_ciphersuite_id_from_ssl(&ssl);
            if (!complete || handoff_taken || !exported || mbedtls_ssl_get_version_number(&ssl) != MBEDTLS_SSL_VERSION_TLS1_2 ||
                (cipher_suite != MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256 &&
                 cipher_suite != MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256))
            {
                return std::nullopt;
            }

            std::array<uint8_t, 64> randoms{};
            std::memcpy(randoms.data(), server_random.data(), server_random.size());
            std::memcpy(randoms.data() + server_random.size(), client_random.data(), client_random.size());
            std::array<uint8_t, 40> key_block{};
            if (mbedtls_ssl_tls_prf(prf, master_secret.data(), master_secret.size(), "key expansion", randoms.data(), randoms.size(),
                                    key_block.data(), key_block.size()) != 0)
            {
                return std::nullopt;
            }

            provider_context_input result{
                .protocol = tls_1_2,
                .cipher_suite = static_cast<uint32_t>(cipher_suite),
                .inbound_sequence = inbound_counter.value(),
                .outbound_sequence = outbound_counter.value(),
                .serialized_context_flags = 0x0000000008008200,
            };
            std::memcpy(result.outbound_raw_key.data(), key_block.data(), result.outbound_raw_key.size());
            std::memcpy(result.inbound_raw_key.data(), key_block.data() + 16, result.inbound_raw_key.size());
            std::memcpy(result.outbound_fixed_iv.data(), key_block.data() + 32, result.outbound_fixed_iv.size());
            std::memcpy(result.inbound_fixed_iv.data(), key_block.data() + 36, result.inbound_fixed_iv.size());
            for (const auto* certificate = mbedtls_ssl_get_peer_cert(&ssl); certificate != nullptr; certificate = certificate->next)
            {
                result.peer_certificates.emplace_back(certificate->raw.p, certificate->raw.p + certificate->raw.len);
            }
            // Session tickets are disabled so this is the Finished value from a full handshake in the pinned mbedTLS revision.
            if (ssl.MBEDTLS_PRIVATE(verify_data_len) != result.tls_unique.size())
            {
                return std::nullopt;
            }
            std::memcpy(result.tls_unique.data(), ssl.MBEDTLS_PRIVATE(own_verify_data), result.tls_unique.size());
            mbedtls_platform_zeroize(key_block.data(), key_block.size());
            handoff_taken = true;
            return result;
        }
    };

    std::unique_ptr<tls_client> tls_client::create(const std::string_view target)
    {
        if (target.empty() || target.find('\0') != std::string_view::npos)
        {
            return nullptr;
        }

        auto state = std::make_unique<impl>();
        constexpr std::string_view personalization = "sogen-sspi-tls-client";
        static constexpr std::array cipher_suites{MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
                                                  MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256, 0};
        if (mbedtls_ctr_drbg_seed(&state->random, mbedtls_entropy_func, &state->entropy,
                                  reinterpret_cast<const unsigned char*>(personalization.data()), personalization.size()) != 0 ||
            mbedtls_ssl_config_defaults(&state->config, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) !=
                0)
        {
            return nullptr;
        }
        mbedtls_ssl_conf_rng(&state->config, mbedtls_ctr_drbg_random, &state->random);
        mbedtls_ssl_conf_authmode(&state->config, MBEDTLS_SSL_VERIFY_NONE);
        mbedtls_ssl_conf_min_tls_version(&state->config, MBEDTLS_SSL_VERSION_TLS1_2);
        mbedtls_ssl_conf_max_tls_version(&state->config, MBEDTLS_SSL_VERSION_TLS1_2);
        mbedtls_ssl_conf_ciphersuites(&state->config, cipher_suites.data());
        mbedtls_ssl_conf_session_tickets(&state->config, MBEDTLS_SSL_SESSION_TICKETS_DISABLED);
        if (mbedtls_ssl_setup(&state->ssl, &state->config) != 0)
        {
            return nullptr;
        }
        mbedtls_ssl_set_export_keys_cb(&state->ssl, impl::export_keys, state.get());
        mbedtls_ssl_set_bio(&state->ssl, state.get(), impl::send, impl::receive, nullptr);
        const std::string hostname{target};
        if (mbedtls_ssl_set_hostname(&state->ssl, hostname.c_str()) != 0)
        {
            return nullptr;
        }
        return std::unique_ptr<tls_client>(new tls_client(std::move(state)));
    }

    tls_client::tls_client(std::unique_ptr<impl> state)
        : state_(std::move(state))
    {
    }

    tls_client::~tls_client() = default;

    handshake_result tls_client::process(const std::span<const uint8_t> input)
    {
        handshake_result result{};
        if (!state_ || state_->complete)
        {
            return result;
        }
        if (input.empty())
        {
            result.status = state_->advance(result.output_token);
            return result;
        }

        const auto framing = frame_tls_records(input);
        if (framing.missing_size != 0)
        {
            result.status = handshake_status::incomplete_message;
            result.missing_size = framing.missing_size;
            return result;
        }

        size_t offset{};
        while (offset < input.size())
        {
            const size_t size = tls_record_header_size + (static_cast<size_t>(input[offset + 3]) << 8) + input[offset + 4];
            const auto record = input.subspan(offset, size);
            try
            {
                state_->input.insert(state_->input.end(), record.begin(), record.end());
            }
            catch (...)
            {
                result.status = handshake_status::failed;
                return result;
            }
            result.status = state_->advance(result.output_token);
            if (result.status == handshake_status::failed || !state_->inbound_counter.observe(record))
            {
                result.status = handshake_status::failed;
                return result;
            }
            offset += size;
            if (result.status == handshake_status::complete)
            {
                result.extra_size = input.size() - offset;
                return result;
            }
        }
        return result;
    }

    std::optional<provider_context_input> tls_client::take_handoff_state()
    {
        return state_ ? state_->extract_handoff() : std::nullopt;
    }
}
