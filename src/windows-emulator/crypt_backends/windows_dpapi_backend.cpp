#include "../std_include.hpp"
#include "dpapi_primitives.hpp"

#include <platform/crypt_protect_backend.hpp>
#include <utils/io.hpp>

#include <random>

namespace sogen
{
    namespace
    {
        constexpr uint32_t k_blob_version = 1;
        constexpr uint32_t k_file_version = 1;
        constexpr size_t k_salt_len = 32;
        constexpr size_t k_hmac_len = 64;
        constexpr size_t k_aes_key_len = 32;
        constexpr uint32_t k_crypt_bits = 256;
        constexpr uint32_t k_hash_bits = 512;
        constexpr std::array<char, 8> k_master_key_magic{'S', 'O', 'G', 'E', 'N', 'M', 'K', '1'};

        void append_u32(std::vector<uint8_t>& out, const uint32_t value)
        {
            const std::array<uint8_t, 4> bytes{
                static_cast<uint8_t>(value),
                static_cast<uint8_t>(value >> 8),
                static_cast<uint8_t>(value >> 16),
                static_cast<uint8_t>(value >> 24),
            };
            out.insert(out.end(), bytes.begin(), bytes.end());
        }

        bool read_u32(const std::span<const uint8_t> data, size_t& offset, uint32_t& value)
        {
            if (offset + 4 > data.size())
            {
                return false;
            }

            const auto* bytes = data.data() + offset;
            value = static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) | (static_cast<uint32_t>(bytes[2]) << 16) |
                    (static_cast<uint32_t>(bytes[3]) << 24);
            offset += 4;
            return true;
        }

        bool read_bytes(const std::span<const uint8_t> data, size_t& offset, const size_t count, std::span<uint8_t> dest)
        {
            if (count > dest.size() || offset + count > data.size())
            {
                return false;
            }

            if (count != 0)
            {
                std::memcpy(dest.data(), data.data() + offset, count);
            }
            offset += count;
            return true;
        }

        bool skip_bytes(const std::span<const uint8_t> data, size_t& offset, const size_t count)
        {
            if (offset + count > data.size())
            {
                return false;
            }

            offset += count;
            return true;
        }

        void fill_system_random(const std::span<uint8_t> out)
        {
            std::random_device device;
            for (uint8_t& byte : out)
            {
                byte = static_cast<uint8_t>(device());
            }
        }

        std::filesystem::path master_key_path(const std::filesystem::path& emulation_root)
        {
            return emulation_root / "dpapi" / "master-key";
        }

        bool load_master_key_file(const std::filesystem::path& path, std::array<uint8_t, k_dpapi_master_key_size>& key)
        {
            const auto bytes = utils::io::read_file(path);
            constexpr size_t expected = sizeof(k_master_key_magic) + sizeof(uint32_t) + k_dpapi_master_key_size;
            if (bytes.size() != expected)
            {
                return false;
            }

            if (std::memcmp(bytes.data(), k_master_key_magic.data(), k_master_key_magic.size()) != 0)
            {
                return false;
            }

            uint32_t version{};
            std::memcpy(&version, bytes.data() + k_master_key_magic.size(), sizeof(version));
            if (version != k_file_version)
            {
                return false;
            }

            std::memcpy(key.data(), bytes.data() + k_master_key_magic.size() + sizeof(version), key.size());
            return true;
        }

        bool save_master_key_file(const std::filesystem::path& path, const std::array<uint8_t, k_dpapi_master_key_size>& key)
        {
            utils::io::create_directory(path.parent_path());
            std::vector<std::byte> bytes(k_master_key_magic.size() + sizeof(uint32_t) + key.size());
            std::memcpy(bytes.data(), k_master_key_magic.data(), k_master_key_magic.size());
            uint32_t version = k_file_version;
            std::memcpy(bytes.data() + k_master_key_magic.size(), &version, sizeof(version));
            std::memcpy(bytes.data() + k_master_key_magic.size() + sizeof(version), key.data(), key.size());
            return utils::io::write_file(path, bytes);
        }

        struct parsed_blob
        {
            dpapi_blob_view view{};
            std::u16string description{};
            std::span<const uint8_t> salt{};
            std::span<const uint8_t> hmac_key{};
            std::span<const uint8_t> mac_salt{};
            std::span<const uint8_t> ciphertext{};
            std::span<const uint8_t> signature{};
            std::span<const uint8_t> mac_prefix{};
        };

        bool parse_blob_full(const std::span<const uint8_t> blob, parsed_blob& parsed)
        {
            if (blob.size() < 20)
            {
                return false;
            }

            size_t offset = 0;
            if (!read_u32(blob, offset, parsed.view.outer_version) || parsed.view.outer_version != k_blob_version)
            {
                return false;
            }

            if (!read_bytes(blob, offset, 16, parsed.view.provider_guid))
            {
                return false;
            }

            const size_t inner_begin = offset;
            if (!read_u32(blob, offset, parsed.view.inner_version) || parsed.view.inner_version != k_blob_version)
            {
                return false;
            }

            if (!read_bytes(blob, offset, 16, parsed.view.master_key_guid))
            {
                return false;
            }

            uint32_t desc_len{};
            if (!read_u32(blob, offset, parsed.view.flags) || !read_u32(blob, offset, desc_len) || (desc_len % 2) != 0)
            {
                return false;
            }

            if (desc_len != 0)
            {
                if (offset + desc_len > blob.size())
                {
                    return false;
                }

                parsed.description.assign(reinterpret_cast<const char16_t*>(blob.data() + offset), desc_len / 2);
                if (!parsed.description.empty() && parsed.description.back() == 0)
                {
                    parsed.description.pop_back();
                }
                offset += desc_len;
            }

            if (!read_u32(blob, offset, parsed.view.alg_crypt) || !read_u32(blob, offset, parsed.view.crypt_bits) ||
                !read_u32(blob, offset, parsed.view.salt_len))
            {
                return false;
            }

            const size_t salt_off = offset;
            if (!skip_bytes(blob, offset, parsed.view.salt_len) || !read_u32(blob, offset, parsed.view.hmac_key_len))
            {
                return false;
            }

            const size_t hmac_key_off = offset;
            if (!skip_bytes(blob, offset, parsed.view.hmac_key_len) || !read_u32(blob, offset, parsed.view.alg_hash) ||
                !read_u32(blob, offset, parsed.view.hash_bits) || !read_u32(blob, offset, parsed.view.mac_salt_len))
            {
                return false;
            }

            const size_t mac_salt_off = offset;
            if (!skip_bytes(blob, offset, parsed.view.mac_salt_len) || !read_u32(blob, offset, parsed.view.data_len))
            {
                return false;
            }

            const size_t data_off = offset;
            if (!skip_bytes(blob, offset, parsed.view.data_len) || !read_u32(blob, offset, parsed.view.sign_len))
            {
                return false;
            }

            const size_t sig_off = offset;
            if (!skip_bytes(blob, offset, parsed.view.sign_len) || offset != blob.size())
            {
                return false;
            }

            parsed.salt = blob.subspan(salt_off, parsed.view.salt_len);
            parsed.hmac_key = blob.subspan(hmac_key_off, parsed.view.hmac_key_len);
            parsed.mac_salt = blob.subspan(mac_salt_off, parsed.view.mac_salt_len);
            parsed.ciphertext = blob.subspan(data_off, parsed.view.data_len);
            parsed.signature = blob.subspan(sig_off, parsed.view.sign_len);
            parsed.mac_prefix = blob.subspan(inner_begin, data_off + parsed.view.data_len - inner_begin);
            return true;
        }

        std::array<uint8_t, k_aes_key_len> derive_aes_key(const std::array<uint8_t, 20>& mk_sha1, const std::span<const uint8_t> salt,
                                                          const std::span<const uint8_t> entropy)
        {
            std::vector<uint8_t> hmac_data;
            hmac_data.reserve(salt.size() + entropy.size());
            hmac_data.insert(hmac_data.end(), salt.begin(), salt.end());
            hmac_data.insert(hmac_data.end(), entropy.begin(), entropy.end());
            const auto digest = dpapi_primitives::hmac_sha512(mk_sha1, hmac_data);
            std::array<uint8_t, k_aes_key_len> key{};
            std::memcpy(key.data(), digest.data(), key.size());
            return key;
        }

        std::array<uint8_t, k_hmac_len> compute_blob_mac(const std::array<uint8_t, 20>& mk_sha1, const std::span<const uint8_t> mac_salt,
                                                         const std::span<const uint8_t> entropy, const std::span<const uint8_t> prefix)
        {
            std::vector<uint8_t> hmac_data;
            hmac_data.reserve(mac_salt.size() + entropy.size() + prefix.size());
            hmac_data.insert(hmac_data.end(), mac_salt.begin(), mac_salt.end());
            hmac_data.insert(hmac_data.end(), entropy.begin(), entropy.end());
            hmac_data.insert(hmac_data.end(), prefix.begin(), prefix.end());
            return dpapi_primitives::hmac_sha512(mk_sha1, hmac_data);
        }

        class windows_dpapi_backend final : public crypt_protect_backend
        {
          public:
            windows_dpapi_backend(std::filesystem::path emulation_root, crypt_protect_rng rng, std::span<const uint8_t> master_key)
                : emulation_root_(std::move(emulation_root)),
                  rng_(std::move(rng))
            {
                if (master_key.size() == k_dpapi_master_key_size)
                {
                    std::memcpy(master_key_.data(), master_key.data(), master_key.size());
                    if (!this->emulation_root_.empty())
                    {
                        save_master_key_file(master_key_path(this->emulation_root_), this->master_key_);
                    }
                    return;
                }

                if (!this->emulation_root_.empty() && load_master_key_file(master_key_path(this->emulation_root_), this->master_key_))
                {
                    return;
                }

                this->fill_random(this->master_key_);
                if (!this->emulation_root_.empty())
                {
                    save_master_key_file(master_key_path(this->emulation_root_), this->master_key_);
                }
            }

            crypt_protect_result protect(const crypt_protect_request& request) override
            {
                crypt_protect_result result{};
                std::array<uint8_t, k_salt_len> salt{};
                std::array<uint8_t, k_salt_len> mac_salt{};
                this->fill_random(salt);
                this->fill_random(mac_salt);

                const auto mk_sha1 = dpapi_primitives::sha1(this->master_key_);
                const auto aes_key = derive_aes_key(mk_sha1, salt, request.entropy);

                std::vector<uint8_t> ciphertext{};
                if (!dpapi_primitives::aes256_cbc_encrypt(aes_key, request.data, ciphertext))
                {
                    result.last_error = k_error_invalid_data;
                    return result;
                }

                std::vector<uint8_t> inner;
                inner.reserve(128 + request.description.size() * 2 + ciphertext.size());
                append_u32(inner, k_blob_version);
                inner.insert(inner.end(), k_emulator_dpapi_master_key_guid.begin(), k_emulator_dpapi_master_key_guid.end());
                append_u32(inner, request.flags & k_cryptprotect_local_machine);

                if (request.description.empty())
                {
                    append_u32(inner, 0);
                }
                else
                {
                    const auto desc_bytes = static_cast<uint32_t>((request.description.size() + 1) * sizeof(char16_t));
                    append_u32(inner, desc_bytes);
                    const auto* descr = reinterpret_cast<const uint8_t*>(request.description.data());
                    inner.insert(inner.end(), descr, descr + request.description.size() * sizeof(char16_t));
                    inner.insert(inner.end(), sizeof(char16_t), 0);
                }

                append_u32(inner, k_calg_aes_256);
                append_u32(inner, k_crypt_bits);
                append_u32(inner, static_cast<uint32_t>(salt.size()));
                inner.insert(inner.end(), salt.begin(), salt.end());
                append_u32(inner, 0);
                append_u32(inner, k_calg_sha_512);
                append_u32(inner, k_hash_bits);
                append_u32(inner, static_cast<uint32_t>(mac_salt.size()));
                inner.insert(inner.end(), mac_salt.begin(), mac_salt.end());
                append_u32(inner, static_cast<uint32_t>(ciphertext.size()));
                inner.insert(inner.end(), ciphertext.begin(), ciphertext.end());

                const auto signature = compute_blob_mac(mk_sha1, mac_salt, request.entropy, inner);
                append_u32(inner, static_cast<uint32_t>(signature.size()));
                inner.insert(inner.end(), signature.begin(), signature.end());

                result.data.reserve(20 + inner.size());
                append_u32(result.data, k_blob_version);
                result.data.insert(result.data.end(), k_dpapi_default_provider_guid.begin(), k_dpapi_default_provider_guid.end());
                result.data.insert(result.data.end(), inner.begin(), inner.end());
                result.ok = true;
                return result;
            }

            crypt_protect_result unprotect(const crypt_protect_request& request, const bool return_description) override
            {
                crypt_protect_result result{};
                parsed_blob parsed{};
                if (!parse_blob_full(request.data, parsed))
                {
                    result.last_error = k_error_invalid_data;
                    return result;
                }

                if (parsed.view.master_key_guid != k_emulator_dpapi_master_key_guid || parsed.view.alg_crypt != k_calg_aes_256 ||
                    parsed.view.alg_hash != k_calg_sha_512 || parsed.view.salt_len != k_salt_len ||
                    parsed.view.mac_salt_len != k_salt_len || parsed.view.hmac_key_len != 0 || parsed.view.sign_len != k_hmac_len)
                {
                    result.last_error = k_error_invalid_data;
                    return result;
                }

                const auto mk_sha1 = dpapi_primitives::sha1(this->master_key_);
                const auto expected = compute_blob_mac(mk_sha1, parsed.mac_salt, request.entropy, parsed.mac_prefix);
                if (!dpapi_primitives::constant_time_equal(expected, parsed.signature))
                {
                    result.last_error = k_error_invalid_data;
                    return result;
                }

                const auto aes_key = derive_aes_key(mk_sha1, parsed.salt, request.entropy);
                if (!dpapi_primitives::aes256_cbc_decrypt(aes_key, parsed.ciphertext, result.data))
                {
                    result.last_error = k_error_invalid_data;
                    return result;
                }

                if (return_description)
                {
                    result.description = std::move(parsed.description);
                }

                result.ok = true;
                return result;
            }

          private:
            void fill_random(const std::span<uint8_t> out)
            {
                if (this->rng_)
                {
                    this->rng_(out);
                    return;
                }

                fill_system_random(out);
            }

            std::filesystem::path emulation_root_{};
            crypt_protect_rng rng_{};
            std::array<uint8_t, k_dpapi_master_key_size> master_key_{};
        };
    }

    bool parse_dpapi_blob(const std::span<const uint8_t> blob, dpapi_blob_view& view)
    {
        parsed_blob parsed{};
        if (!parse_blob_full(blob, parsed))
        {
            return false;
        }

        view = parsed.view;
        return true;
    }

    std::unique_ptr<crypt_protect_backend> create_windows_dpapi_backend(const std::filesystem::path& emulation_root, crypt_protect_rng rng,
                                                                        const std::span<const uint8_t> master_key)
    {
        return std::make_unique<windows_dpapi_backend>(emulation_root, std::move(rng), master_key);
    }
}
