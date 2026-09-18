#include "../std_include.hpp"
#include "protected_storage.hpp"

#include "binary_writer.hpp"
#include "../ksec_memory_crypt.hpp"
#include "../windows_emulator.hpp"

#include <platform/crypt_protect_backend.hpp>

namespace sogen
{

    namespace
    {
        // ICryptProtect on \RPC Control\protected_storage ({11220835-5b26-4d94-ae86-c3e475a809de} v1.0).
        // Request/reply NDR matches the public NtObjectManager reconstruction of dpapisrv:
        //   0 s_SSCryptProtectData   in: bytes, len, string, unique bytes, unique prompt, flags, unique extra
        //   1 s_SSCryptUnprotectData in: bytes, len, unique bytes, unique prompt, flags, unique extra
        //   2 s_SSCryptUpdateProtectedState
        // Unique pointees are embedded immediately after the referent. Out blobs are unique + conformant
        // array, then DWORD length, then DWORD Win32 status. Description is a required NDR string on
        // protect (crypt32 marshals L"" when the caller passes NULL).
        constexpr std::array<uint8_t, 16> k_iface_icrypt_protect{0x35, 0x08, 0x22, 0x11, 0x26, 0x5b, 0x94, 0x4d,
                                                                 0xae, 0x86, 0xc3, 0xe4, 0x75, 0xa8, 0x09, 0xde};

        constexpr uint32_t k_op_protect = 0;
        constexpr uint32_t k_op_unprotect = 1;
        constexpr uint32_t k_op_update = 2;

        struct ndr_cursor
        {
            std::span<const uint8_t> bytes{};
            size_t offset{};
            size_t pointer_size{8};

            bool remaining(const size_t count) const
            {
                return offset <= bytes.size() && (bytes.size() - offset) >= count;
            }

            bool align_to(const size_t alignment)
            {
                if (alignment == 0)
                {
                    return false;
                }

                const auto misaligned = offset % alignment;
                if (misaligned == 0)
                {
                    return true;
                }

                return skip(alignment - misaligned);
            }

            bool skip(const size_t count)
            {
                if (!remaining(count))
                {
                    return false;
                }

                offset += count;
                return true;
            }

            bool read_u32(uint32_t& value)
            {
                if (!align_to(sizeof(uint32_t)) || !remaining(sizeof(uint32_t)))
                {
                    return false;
                }

                std::memcpy(&value, bytes.data() + offset, sizeof(value));
                offset += sizeof(value);
                return true;
            }

            bool read_count(size_t& value)
            {
                if (pointer_size == sizeof(uint32_t))
                {
                    uint32_t narrowed{};
                    if (!read_u32(narrowed))
                    {
                        return false;
                    }

                    value = narrowed;
                    return true;
                }

                if (!align_to(sizeof(uint64_t)) || !remaining(sizeof(uint64_t)))
                {
                    return false;
                }

                uint64_t wide{};
                std::memcpy(&wide, bytes.data() + offset, sizeof(wide));
                offset += sizeof(wide);
                value = static_cast<size_t>(wide);
                return true;
            }

            bool read_pointer(uint64_t& value)
            {
                size_t raw{};
                if (!read_count(raw))
                {
                    return false;
                }

                value = raw;
                return true;
            }

            bool read_bytes(const size_t count, std::vector<uint8_t>& out)
            {
                if (!remaining(count))
                {
                    return false;
                }

                out.assign(bytes.data() + offset, bytes.data() + offset + count);
                offset += count;
                return true;
            }

            bool read_conformant_bytes(std::vector<uint8_t>& out)
            {
                size_t count{};
                if (!read_count(count))
                {
                    return false;
                }

                return read_bytes(count, out);
            }

            bool read_unique_bytes(std::vector<uint8_t>& out)
            {
                uint64_t referent{};
                if (!read_pointer(referent))
                {
                    return false;
                }

                if (referent == 0)
                {
                    out.clear();
                    return true;
                }

                return read_conformant_bytes(out);
            }

            bool read_ndr_string(std::u16string& out)
            {
                size_t max_chars{};
                size_t first{};
                size_t actual{};
                if (!read_count(max_chars) || !read_count(first) || !read_count(actual))
                {
                    return false;
                }

                (void)max_chars;
                (void)first;
                const auto bytes_count = actual * sizeof(char16_t);
                if (!remaining(bytes_count))
                {
                    return false;
                }

                out.assign(reinterpret_cast<const char16_t*>(bytes.data() + offset), actual);
                offset += bytes_count;
                if (!out.empty() && out.back() == 0)
                {
                    out.pop_back();
                }

                return true;
            }

            bool skip_unique_prompt()
            {
                uint64_t referent{};
                if (!read_pointer(referent))
                {
                    return false;
                }

                if (referent == 0)
                {
                    return true;
                }

                uint32_t ignored{};
                return read_u32(ignored) && read_u32(ignored);
            }
        };

        void write_conformant_bytes(utils::aligned_binary_writer& writer, const std::span<const uint8_t> data)
        {
            writer.write_pointer_sized(data.size());
            if (!data.empty())
            {
                writer.write(data.data(), data.size(), 1);
            }
        }

        uint32_t win32_status(const crypt_protect_result& result)
        {
            if (result.ok)
            {
                return 0;
            }

            if (result.last_error != 0)
            {
                return result.last_error;
            }

            return k_error_invalid_data;
        }

        void write_protect_reply(utils::aligned_binary_writer& writer, const crypt_protect_result& result)
        {
            writer.write_ndr_pointer(result.ok);
            if (result.ok)
            {
                write_conformant_bytes(writer, result.data);
            }

            writer.align_to(sizeof(uint32_t));
            writer.write<uint32_t>(result.ok ? static_cast<uint32_t>(result.data.size()) : 0);
            writer.write<uint32_t>(win32_status(result));
        }

        void write_unprotect_reply(utils::aligned_binary_writer& writer, const crypt_protect_result& result)
        {
            const auto ok = result.ok;
            writer.write_ndr_pointer(ok);
            if (ok)
            {
                write_conformant_bytes(writer, result.data);
            }

            writer.align_to(sizeof(uint32_t));
            writer.write<uint32_t>(ok ? static_cast<uint32_t>(result.data.size()) : 0);
            writer.write_ndr_pointer(ok);
            if (ok)
            {
                writer.write_ndr_u16string(result.description);
            }

            writer.align_to(sizeof(uint32_t));
            writer.write<uint32_t>(win32_status(result));
        }

        void bind_request(crypt_protect_request& request, std::vector<uint8_t>& data, std::vector<uint8_t>& entropy,
                          std::u16string& description)
        {
            request.data = data;
            request.entropy = entropy;
            request.description = description;
        }

        bool parse_protect_request(ndr_cursor& cursor, std::vector<uint8_t>& data, std::vector<uint8_t>& entropy,
                                   std::u16string& description, uint32_t& flags)
        {
            uint32_t data_len{};
            if (!cursor.read_conformant_bytes(data) || !cursor.read_u32(data_len))
            {
                return false;
            }

            if (data_len < data.size())
            {
                data.resize(data_len);
            }

            if (!cursor.read_ndr_string(description))
            {
                return false;
            }

            uint32_t entropy_len{};
            if (!cursor.read_unique_bytes(entropy) || !cursor.read_u32(entropy_len))
            {
                return false;
            }

            if (entropy_len < entropy.size())
            {
                entropy.resize(entropy_len);
            }

            if (!cursor.skip_unique_prompt())
            {
                return false;
            }

            std::vector<uint8_t> extra{};
            uint32_t extra_len{};
            if (!cursor.read_u32(flags) || !cursor.read_unique_bytes(extra) || !cursor.read_u32(extra_len))
            {
                return false;
            }

            return true;
        }

        bool parse_unprotect_request(ndr_cursor& cursor, std::vector<uint8_t>& data, std::vector<uint8_t>& entropy, uint32_t& flags)
        {
            uint32_t data_len{};
            if (!cursor.read_conformant_bytes(data) || !cursor.read_u32(data_len))
            {
                return false;
            }

            if (data_len < data.size())
            {
                data.resize(data_len);
            }

            uint32_t entropy_len{};
            if (!cursor.read_unique_bytes(entropy) || !cursor.read_u32(entropy_len))
            {
                return false;
            }

            if (entropy_len < entropy.size())
            {
                entropy.resize(entropy_len);
            }

            if (!cursor.skip_unique_prompt())
            {
                return false;
            }

            std::vector<uint8_t> extra{};
            uint32_t extra_len{};
            if (!cursor.read_u32(flags) || !cursor.read_unique_bytes(extra) || !cursor.read_u32(extra_len))
            {
                return false;
            }

            return true;
        }

        struct protected_storage_port : rpc_port
        {
            NTSTATUS handle_rpc(windows_emulator& win_emu, const uint32_t procedure_id, const lpc_request_context& c,
                                utils::aligned_binary_writer& writer, std::vector<alpc_reply_handle>& /*reply_handles*/) override
            {
                if (this->bound_interface() != std::array<uint8_t, 16>{} && this->bound_interface() != k_iface_icrypt_protect)
                {
                    win_emu.log.print(color::gray, "Unexpected protected_storage interface bind\n");
                    return STATUS_NOT_SUPPORTED;
                }

                switch (procedure_id)
                {
                case k_op_protect:
                    return handle_protect(win_emu, c, writer, false);
                case k_op_unprotect:
                    return handle_protect(win_emu, c, writer, true);
                case k_op_update:
                    writer.write<uint32_t>(0);
                    writer.write<uint32_t>(0);
                    writer.write<uint32_t>(0);
                    return STATUS_SUCCESS;
                default:
                    win_emu.log.print(color::gray, "Unexpected protected_storage procedure: %u\n", procedure_id);
                    return STATUS_NOT_SUPPORTED;
                }
            }

            static NTSTATUS handle_protect(windows_emulator& win_emu, const lpc_request_context& c, utils::aligned_binary_writer& writer,
                                           const bool unprotect)
            {
                std::vector<uint8_t> payload(c.send_buffer_length, 0);
                if (c.send_buffer && c.send_buffer_length)
                {
                    win_emu.emu().read_memory(c.send_buffer, payload.data(), payload.size());
                }

                ndr_cursor cursor{
                    .bytes = payload,
                    .pointer_size = writer.pointer_size(),
                };

                std::vector<uint8_t> data{};
                std::vector<uint8_t> entropy{};
                std::u16string description{};
                uint32_t flags{};
                const auto parsed = unprotect ? parse_unprotect_request(cursor, data, entropy, flags)
                                              : parse_protect_request(cursor, data, entropy, description, flags);
                if (!parsed)
                {
                    win_emu.log.print(color::gray, "Failed to parse protected_storage %s NDR\n", unprotect ? "Unprotect" : "Protect");
                    crypt_protect_result failed{};
                    failed.last_error = k_error_invalid_parameter;
                    if (unprotect)
                    {
                        write_unprotect_reply(writer, failed);
                    }
                    else
                    {
                        write_protect_reply(writer, failed);
                    }
                    return STATUS_SUCCESS;
                }

                // dpapi.dll SystemFunction040's the padded payload before RPC; dpapisrv
                // SystemFunction041's it (SPCryptProtect) before wrapping, and 040's again
                // (SPCryptUnprotect) before returning. Same XOR as KsecDD.
                if (!unprotect)
                {
                    xor_ksec_memory(data);
                }

                crypt_protect_request request{};
                bind_request(request, data, entropy, description);
                request.flags = flags;

                win_emu.callbacks.on_generic_activity(unprotect ? "CryptUnprotectData" : "CryptProtectData");
                auto result = unprotect ? win_emu.crypt_protect().unprotect(request, true) : win_emu.crypt_protect().protect(request);
                if (unprotect && result.ok)
                {
                    xor_ksec_memory(result.data);
                }

                if (unprotect)
                {
                    write_unprotect_reply(writer, result);
                }
                else
                {
                    write_protect_reply(writer, result);
                }

                return STATUS_SUCCESS;
            }
        };
    }

    std::unique_ptr<port> create_protected_storage_port()
    {
        return std::make_unique<protected_storage_port>();
    }

} // namespace sogen
