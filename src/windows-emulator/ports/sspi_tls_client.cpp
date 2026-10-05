#include "../std_include.hpp"
#include "sspi_tls_client.hpp"

#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/params.h>
#include <openssl/ssl.h>

namespace sogen::sspi
{
    namespace
    {
        template <typename T, void (*Free)(T*)>
        using openssl_ptr = std::unique_ptr<T, decltype(Free)>;

        template <size_t Size>
        struct secret_bytes
        {
            ~secret_bytes()
            {
                OPENSSL_cleanse(bytes.data(), bytes.size());
            }

            std::array<uint8_t, Size> bytes{};
        };
    }

    struct tls_client::impl
    {
        openssl_ptr<SSL_CTX, SSL_CTX_free> context{nullptr, SSL_CTX_free};
        openssl_ptr<SSL, SSL_free> ssl{nullptr, SSL_free};
        BIO* read_bio{};
        BIO* write_bio{};
        tls_record_counter inbound_counter{};
        tls_record_counter outbound_counter{};
        bool complete{};
        bool handoff_taken{};

        bool drain_output(std::vector<uint8_t>& output)
        {
            const size_t pending = BIO_ctrl_pending(write_bio);
            if (pending == 0)
            {
                return true;
            }
            if (pending > static_cast<size_t>(std::numeric_limits<int>::max()))
            {
                return false;
            }

            const size_t original_size = output.size();
            output.resize(original_size + pending);
            const int read = BIO_read(write_bio, output.data() + original_size, static_cast<int>(pending));
            if (read != static_cast<int>(pending))
            {
                return false;
            }

            const auto records = std::span<const uint8_t>{output}.subspan(original_size);
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

        handshake_status advance(std::vector<uint8_t>& output)
        {
            const int result = SSL_do_handshake(ssl.get());
            const int error = result == 1 ? SSL_ERROR_NONE : SSL_get_error(ssl.get(), result);
            if (!drain_output(output))
            {
                return handshake_status::failed;
            }
            if (result == 1 && SSL_is_init_finished(ssl.get()))
            {
                complete = true;
                return handshake_status::complete;
            }
            if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE)
            {
                return handshake_status::continue_needed;
            }
            return handshake_status::failed;
        }

        std::optional<provider_context_input> extract_handoff()
        {
            constexpr uint32_t tls_1_2 = 0x0303;
            constexpr uint32_t cipher_suite = 0xc02b;
            if (!complete || handoff_taken || SSL_version(ssl.get()) != TLS1_2_VERSION || SSL_session_reused(ssl.get()) != 0)
            {
                return std::nullopt;
            }
            const SSL_CIPHER* cipher = SSL_get_current_cipher(ssl.get());
            if (cipher == nullptr || SSL_CIPHER_get_protocol_id(cipher) != cipher_suite)
            {
                return std::nullopt;
            }
            const EVP_MD* digest = SSL_CIPHER_get_handshake_digest(cipher);
            if (digest == nullptr || EVP_MD_is_a(digest, "SHA256") != 1)
            {
                return std::nullopt;
            }

            openssl_ptr<SSL_SESSION, SSL_SESSION_free> session{SSL_get1_session(ssl.get()), SSL_SESSION_free};
            if (!session)
            {
                return std::nullopt;
            }
            const size_t master_size = SSL_SESSION_get_master_key(session.get(), nullptr, 0);
            if (master_size == 0 || master_size > 64)
            {
                return std::nullopt;
            }

            secret_bytes<64> master{};
            if (SSL_SESSION_get_master_key(session.get(), master.bytes.data(), master.bytes.size()) != master_size)
            {
                return std::nullopt;
            }
            std::array<uint8_t, 32> client_random{};
            std::array<uint8_t, 32> server_random{};
            if (SSL_get_client_random(ssl.get(), client_random.data(), client_random.size()) != client_random.size() ||
                SSL_get_server_random(ssl.get(), server_random.data(), server_random.size()) != server_random.size())
            {
                return std::nullopt;
            }

            constexpr std::string_view label = "key expansion";
            std::array<uint8_t, label.size() + 64> seed{};
            std::memcpy(seed.data(), label.data(), label.size());
            std::memcpy(seed.data() + label.size(), server_random.data(), server_random.size());
            std::memcpy(seed.data() + label.size() + server_random.size(), client_random.data(), client_random.size());

            openssl_ptr<EVP_KDF, EVP_KDF_free> kdf{EVP_KDF_fetch(nullptr, "TLS1-PRF", nullptr), EVP_KDF_free};
            if (!kdf)
            {
                return std::nullopt;
            }
            openssl_ptr<EVP_KDF_CTX, EVP_KDF_CTX_free> kdf_context{EVP_KDF_CTX_new(kdf.get()), EVP_KDF_CTX_free};
            if (!kdf_context)
            {
                return std::nullopt;
            }

            secret_bytes<40> key_block{};
            char digest_name[] = "SHA256";
            OSSL_PARAM parameters[] = {
                OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST, digest_name, 0),
                OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SECRET, master.bytes.data(), master_size),
                OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SEED, seed.data(), seed.size()),
                OSSL_PARAM_construct_end(),
            };
            if (EVP_KDF_derive(kdf_context.get(), key_block.bytes.data(), key_block.bytes.size(), parameters) != 1)
            {
                return std::nullopt;
            }

            provider_context_input result{
                .protocol = tls_1_2,
                .cipher_suite = cipher_suite,
                .inbound_sequence = inbound_counter.value(),
                .outbound_sequence = outbound_counter.value(),
                .serialized_context_flags = 0x0000000008008200,
            };
            std::memcpy(result.outbound_raw_key.data(), key_block.bytes.data(), result.outbound_raw_key.size());
            std::memcpy(result.inbound_raw_key.data(), key_block.bytes.data() + 16, result.inbound_raw_key.size());
            std::memcpy(result.outbound_fixed_iv.data(), key_block.bytes.data() + 32, result.outbound_fixed_iv.size());
            std::memcpy(result.inbound_fixed_iv.data(), key_block.bytes.data() + 36, result.inbound_fixed_iv.size());
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
        state->context.reset(SSL_CTX_new(TLS_client_method()));
        if (!state->context || SSL_CTX_set_min_proto_version(state->context.get(), TLS1_2_VERSION) != 1 ||
            SSL_CTX_set_max_proto_version(state->context.get(), TLS1_2_VERSION) != 1 ||
            SSL_CTX_set_cipher_list(state->context.get(), "ECDHE-ECDSA-AES128-GCM-SHA256") != 1)
        {
            return nullptr;
        }
        SSL_CTX_set_verify(state->context.get(), SSL_VERIFY_NONE, nullptr);
        SSL_CTX_set_session_cache_mode(state->context.get(), SSL_SESS_CACHE_OFF);
        SSL_CTX_set_options(state->context.get(), SSL_OP_NO_RENEGOTIATION);

        state->ssl.reset(SSL_new(state->context.get()));
        BIO* read_bio = BIO_new(BIO_s_mem());
        BIO* write_bio = BIO_new(BIO_s_mem());
        if (!state->ssl || read_bio == nullptr || write_bio == nullptr)
        {
            BIO_free(read_bio);
            BIO_free(write_bio);
            return nullptr;
        }
        state->read_bio = read_bio;
        state->write_bio = write_bio;
        SSL_set_bio(state->ssl.get(), read_bio, write_bio);
        if (SSL_set_tlsext_host_name(state->ssl.get(), std::string{target}.c_str()) != 1)
        {
            return nullptr;
        }
        SSL_set_connect_state(state->ssl.get());
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
            const size_t record_size = tls_record_header_size + (static_cast<size_t>(input[offset + 3]) << 8) + input[offset + 4];
            const auto record = input.subspan(offset, record_size);
            if (BIO_write(state_->read_bio, record.data(), static_cast<int>(record.size())) != static_cast<int>(record.size()))
            {
                result.status = handshake_status::failed;
                return result;
            }
            result.status = state_->advance(result.output_token);
            if (result.status == handshake_status::failed)
            {
                return result;
            }
            if (!state_->inbound_counter.observe(record))
            {
                result.status = handshake_status::failed;
                return result;
            }
            offset += record_size;
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
