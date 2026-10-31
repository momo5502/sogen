#include "../std_include.hpp"
#include "crypt_service.hpp"
#include "ndr_cursor.hpp"

#include "binary_writer.hpp"
#include "../windows_emulator.hpp"

namespace sogen
{

    namespace
    {
        // \RPC Control\keysvc is CryptSvc's endpoint. wintrust reaches the catalog database through ICatDBSvc
        // ({F50AAC00-C7F3-428E-A022-A6B71BFB9D43} v2.0, served by cryptcatsvc.dll), crypt32 its protected-root,
        // auto-update and prefetch helpers through ICertProtect ({0D72A7D4-6148-11D1-B4AA-00C04FB66EA0} v1.0,
        // served by cryptsvc.dll's s_SSCertProtectFunction).
        constexpr std::array<uint8_t, 16> k_iface_catalog_database{0x00, 0xac, 0x0a, 0xf5, 0xf3, 0xc7, 0x8e, 0x42,
                                                                   0xa0, 0x22, 0xa6, 0xb7, 0x1b, 0xfb, 0x9d, 0x43};
        constexpr std::array<uint8_t, 16> k_iface_cert_protect{0xd4, 0xa7, 0x72, 0x0d, 0x48, 0x61, 0xd1, 0x11,
                                                               0xb4, 0xaa, 0x00, 0xc0, 0x4f, 0xb6, 0x6e, 0xa0};

        constexpr uint32_t k_catalog_enum_catalogs = 2;    // s_SSCatDBEnumCatalogs
        constexpr uint32_t k_catalog_prepare_for_call = 5; // s_SSCatDBPrepareForCall
        constexpr uint32_t k_cert_protect_function = 0;    // s_SSCertProtectFunction

        constexpr uint32_t k_error_success = 0;
        constexpr uint32_t k_error_path_not_found = 3;
        constexpr uint32_t k_error_access_denied = 5;
        constexpr uint32_t k_error_invalid_parameter = 87;
        constexpr uint32_t k_error_bad_arguments = 0xA0;

        // The digest size cryptcatsvc keys each catalog hash type (SHA1, SHA256) by.
        constexpr std::array<uint32_t, 2> k_catalog_hash_sizes{20, 32};

        void write_return(utils::aligned_binary_writer& writer, const uint32_t win32_error)
        {
            writer.align_to(sizeof(uint32_t));
            writer.write<uint32_t>(win32_error);
        }

        // cryptcatsvc's _CatDBDVerifyGUIDString: every '0' of the template takes a hex digit, the rest must match.
        bool is_catalog_subsystem_guid(const std::u16string_view value)
        {
            constexpr std::u16string_view pattern = u"{00000000-0000-0000-0000-000000000000}";
            if (value.size() != pattern.size())
            {
                return false;
            }

            for (size_t i = 0; i < pattern.size(); ++i)
            {
                const auto c = value[i];
                if (pattern[i] != u'0')
                {
                    if (c != pattern[i])
                    {
                        return false;
                    }

                    continue;
                }

                const auto is_hex_digit = (c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'f') || (c >= u'A' && c <= u'F');
                if (!is_hex_digit)
                {
                    return false;
                }
            }

            return true;
        }

        // NDR20 marshals the catalog hash type (an enum) as FC_ENUM16, NDR64 as a 32-bit integer.
        bool read_catalog_hash_type(ndr_cursor& cursor, uint32_t& hash_type)
        {
            if (cursor.pointer_size == sizeof(uint32_t))
            {
                uint16_t narrowed{};
                if (!cursor.read_u16(narrowed))
                {
                    return false;
                }

                hash_type = narrowed;
                return true;
            }

            return cursor.read_u32(hash_type);
        }

        // s_SSCatDBPrepareForCall: the client must be able to write the catalog root, then cryptcatsvc makes sure
        // the subsystem's CatRoot and CatRoot2 directories exist.
        uint32_t prepare_catalog_directories(windows_emulator& win_emu, const std::u16string& subsystem)
        {
            if (!is_catalog_subsystem_guid(subsystem))
            {
                return k_error_invalid_parameter;
            }

            const auto system_directory = win_emu.version.get_system_root() / u"System32";
            const auto catalog_root = system_directory / u"CatRoot";
            if (!win_emu.file_sys.is_writable(catalog_root))
            {
                return k_error_access_denied;
            }

            for (const auto& base : {catalog_root, system_directory / u"CatRoot2"})
            {
                for (const auto& directory : {base, base / subsystem})
                {
                    const auto status = win_emu.file_sys.create_directory(directory);
                    if (status != STATUS_SUCCESS && status != STATUS_OBJECT_NAME_COLLISION)
                    {
                        return status == STATUS_ACCESS_DENIED ? k_error_access_denied : k_error_path_not_found;
                    }
                }
            }

            return k_error_success;
        }

        NTSTATUS prepare_for_call(windows_emulator& win_emu, ndr_cursor& cursor, utils::aligned_binary_writer& writer)
        {
            std::u16string subsystem{};
            if (!cursor.read_ndr_string(subsystem))
            {
                return STATUS_INVALID_PARAMETER;
            }

            write_return(writer, prepare_catalog_directories(win_emu, subsystem));
            return STATUS_SUCCESS;
        }

        NTSTATUS enum_catalogs(ndr_cursor& cursor, utils::aligned_binary_writer& writer)
        {
            std::u16string subsystem{};
            uint32_t hash_type{};
            uint32_t hash_size{};
            std::vector<uint8_t> hash{};
            if (!cursor.read_ndr_string(subsystem) || !read_catalog_hash_type(cursor, hash_type) || !cursor.read_u32(hash_size) ||
                !cursor.read_conformant_bytes(hash))
            {
                return STATUS_INVALID_PARAMETER;
            }

            const auto valid = hash_type < k_catalog_hash_sizes.size() && k_catalog_hash_sizes[hash_type] == hash_size &&
                               is_catalog_subsystem_guid(subsystem);

            // Catalog files are not indexed, so every lookup misses the database, which cryptcatsvc reports as
            // success with no catalog names.
            writer.write<uint32_t>(0);       // *pdwNumCatalogNames
            writer.write_ndr_pointer(false); // *ppCatalogNames
            write_return(writer, valid ? k_error_success : k_error_bad_arguments);
            return STATUS_SUCCESS;
        }

        NTSTATUS cert_protect_function(windows_emulator& win_emu, ndr_cursor& cursor)
        {
            uint32_t function_id{};
            if (!cursor.read_u32(function_id))
            {
                return STATUS_INVALID_PARAMETER;
            }

            win_emu.log.print(color::gray, "Unimplemented ICertProtect function: %u\n", function_id);
            return STATUS_NOT_SUPPORTED;
        }

        struct crypt_service_port : rpc_port
        {
            NTSTATUS handle_rpc(windows_emulator& win_emu, const uint32_t procedure_id, const lpc_request_context& c,
                                utils::aligned_binary_writer& writer, std::vector<alpc_reply_handle>& /*reply_handles*/) override
            {
                const auto request = read_rpc_request(win_emu.memory, c);
                ndr_cursor cursor{.bytes = request, .pointer_size = writer.pointer_size()};

                if (this->bound_interface() == k_iface_catalog_database)
                {
                    switch (procedure_id)
                    {
                    case k_catalog_enum_catalogs:
                        return enum_catalogs(cursor, writer);
                    case k_catalog_prepare_for_call:
                        return prepare_for_call(win_emu, cursor, writer);
                    default:
                        break;
                    }
                }
                else if (this->bound_interface() == k_iface_cert_protect && procedure_id == k_cert_protect_function)
                {
                    return cert_protect_function(win_emu, cursor);
                }

                win_emu.log.print(color::gray, "Unimplemented keysvc procedure: %u\n", procedure_id);
                return STATUS_NOT_SUPPORTED;
            }
        };
    }

    std::unique_ptr<port> create_crypt_service_port()
    {
        return std::make_unique<crypt_service_port>();
    }

} // namespace sogen
