#include "rpc_test_utils.hpp"

namespace sogen::test
{
    namespace
    {
        constexpr std::array<uint8_t, 16> catalog_database_interface{0x00, 0xac, 0x0a, 0xf5, 0xf3, 0xc7, 0x8e, 0x42,
                                                                     0xa0, 0x22, 0xa6, 0xb7, 0x1b, 0xfb, 0x9d, 0x43};
        constexpr uint32_t enum_catalogs_procedure = 2;
        constexpr uint32_t prepare_for_call_procedure = 5;
        constexpr size_t reply_header_size = 0x18;
        constexpr std::u16string_view driver_subsystem = u"{F750E6C3-38EE-11D1-85E5-00C04FC295EE}";
        constexpr uint32_t sha1_hash_type = 0;
        constexpr uint32_t sha256_hash_type = 1;
        constexpr std::array<uint8_t, 16> cert_protect_interface{0xd4, 0xa7, 0x72, 0x0d, 0x48, 0x61, 0xd1, 0x11,
                                                                 0xb4, 0xaa, 0x00, 0xc0, 0x4f, 0xb6, 0x6e, 0xa0};
        constexpr uint32_t cert_protect_procedure = 0;
        constexpr uint32_t trigger_prefetch_function = 1000;

        class CryptServiceTest : public RpcProtocolTest
        {
          protected:
            std::vector<uint8_t> enum_catalogs_request(const std::u16string_view subsystem, const uint32_t hash_type,
                                                       const std::span<const uint8_t> hash) const
            {
                std::vector<uint8_t> request{};
                utils::aligned_binary_writer writer(request, this->pointer_size);
                writer.write_ndr_u16string(subsystem);
                if (this->pointer_size == utils::aligned_binary_writer::pointer_size_32)
                {
                    writer.write(static_cast<uint16_t>(hash_type));
                }
                else
                {
                    writer.write(hash_type);
                }
                writer.write(static_cast<uint32_t>(hash.size()));
                writer.write_pointer_sized(hash.size());
                writer.write(hash.data(), hash.size());
                return request;
            }

            std::vector<uint8_t> enum_catalogs(const std::u16string_view subsystem, const uint32_t hash_type,
                                               const std::span<const uint8_t> hash)
            {
                const auto reply = this->call(enum_catalogs_procedure, this->enum_catalogs_request(subsystem, hash_type, hash));
                EXPECT_EQ(STATUS_SUCCESS, reply.status);
                return reply.payload.value_or(std::vector<uint8_t>{});
            }

            std::vector<uint8_t> cert_protect_request(const uint32_t function_id) const
            {
                std::vector<uint8_t> request{};
                utils::aligned_binary_writer writer(request, this->pointer_size);
                writer.write(function_id);
                writer.write<uint32_t>(0);
                writer.write_ndr_u16string(u"");
                writer.write_pointer_sized(0);
                writer.write<uint32_t>(0);
                return request;
            }

            uint32_t prepare_for_call(const std::u16string_view subsystem)
            {
                std::vector<uint8_t> request{};
                utils::aligned_binary_writer writer(request, this->pointer_size);
                writer.write_ndr_u16string(subsystem);
                const auto reply = this->call(prepare_for_call_procedure, request);
                EXPECT_EQ(STATUS_SUCCESS, reply.status);
                return read<uint32_t>(reply.payload.value_or(std::vector<uint8_t>{}), reply_header_size);
            }

            void expect_no_catalogs(const std::vector<uint8_t>& reply, const uint32_t status) const
            {
                const auto names_offset = (reply_header_size + sizeof(uint32_t) + this->pointer_size - 1) & ~(this->pointer_size - 1);
                const auto status_offset = names_offset + this->pointer_size;
                EXPECT_EQ(0u, read<uint32_t>(reply, reply_header_size));
                EXPECT_EQ(0u, this->pointer_size == utils::aligned_binary_writer::pointer_size_32 ? read<uint32_t>(reply, names_offset)
                                                                                                  : read<uint64_t>(reply, names_offset));
                EXPECT_EQ(status, read<uint32_t>(reply, status_offset));
                EXPECT_EQ(status_offset + sizeof(uint32_t), reply.size());
            }
        };
    }

    TEST_F(CryptServiceTest, EnumCatalogsFindsNoCatalogForAWellFormedHash)
    {
        for (const auto width : {utils::aligned_binary_writer::pointer_size_32, utils::aligned_binary_writer::pointer_size_64})
        {
            this->bind(u"\\RPC Control\\keysvc", catalog_database_interface, width);
            constexpr std::array<uint8_t, 20> sha1{};
            constexpr std::array<uint8_t, 32> sha256{};
            this->expect_no_catalogs(this->enum_catalogs(driver_subsystem, sha1_hash_type, sha1), 0);
            this->expect_no_catalogs(this->enum_catalogs(driver_subsystem, sha256_hash_type, sha256), 0);
        }
    }

    TEST_F(CryptServiceTest, EnumCatalogsRejectsMismatchedHashesAndSubsystems)
    {
        constexpr uint32_t error_bad_arguments = 0xA0;
        this->bind(u"\\RPC Control\\keysvc", catalog_database_interface, utils::aligned_binary_writer::pointer_size_64);
        constexpr std::array<uint8_t, 20> sha1{};
        this->expect_no_catalogs(this->enum_catalogs(driver_subsystem, sha256_hash_type, sha1), error_bad_arguments);
        this->expect_no_catalogs(this->enum_catalogs(driver_subsystem, 2, sha1), error_bad_arguments);
        this->expect_no_catalogs(this->enum_catalogs(u"F750E6C3-38EE-11D1-85E5-00C04FC295EE", sha1_hash_type, sha1), error_bad_arguments);
    }

    TEST_F(CryptServiceTest, PrefetchTriggerSucceedsWithoutOutput)
    {
        for (const auto width : {utils::aligned_binary_writer::pointer_size_32, utils::aligned_binary_writer::pointer_size_64})
        {
            this->bind(u"\\RPC Control\\keysvc", cert_protect_interface, width);
            const auto reply = this->call(cert_protect_procedure, this->cert_protect_request(trigger_prefetch_function));
            ASSERT_EQ(STATUS_SUCCESS, reply.status);
            ASSERT_TRUE(reply.payload.has_value());

            const auto& payload = *reply.payload;
            const auto output_offset = reply_header_size + width;
            EXPECT_EQ(0u, width == utils::aligned_binary_writer::pointer_size_32 ? read<uint32_t>(payload, reply_header_size)
                                                                                 : read<uint64_t>(payload, reply_header_size));
            EXPECT_EQ(0u, read<uint32_t>(payload, output_offset));
            EXPECT_EQ(0u, read<uint32_t>(payload, output_offset + sizeof(uint32_t)));
            EXPECT_EQ(output_offset + 2 * sizeof(uint32_t), payload.size());
        }
    }

    TEST_F(CryptServiceTest, PrepareForCallRejectsMalformedSubsystems)
    {
        constexpr uint32_t error_invalid_parameter = 87;
        this->bind(u"\\RPC Control\\keysvc", catalog_database_interface, utils::aligned_binary_writer::pointer_size_64);
        EXPECT_EQ(error_invalid_parameter, this->prepare_for_call(u""));
        EXPECT_EQ(error_invalid_parameter, this->prepare_for_call(u"{F750E6C3-38EE-11D1-85E5-00C04FC295E}"));
        EXPECT_EQ(error_invalid_parameter, this->prepare_for_call(u"{F750E6C3-38EE-11D1-85E5-00C04FC295EG}"));
        EXPECT_EQ(error_invalid_parameter, this->prepare_for_call(u"(F750E6C3-38EE-11D1-85E5-00C04FC295EE)"));
    }
}
