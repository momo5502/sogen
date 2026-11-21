#include "std_include.hpp"

#include "rpc_test_utils.hpp"
#include <ports/nsi_service.hpp>

namespace sogen::test
{
    namespace
    {
        constexpr std::array<uint8_t, 16> exporter_interface{0xe6, 0x73, 0x0c, 0xe6, 0xf9, 0x88, 0xcf, 0x11,
                                                             0x9a, 0xf1, 0x00, 0x20, 0xaf, 0x6e, 0x72, 0xf4};
        constexpr std::array<uint8_t, 16> endpoint_mapper_interface{0x08, 0x83, 0xaf, 0xe1, 0x1f, 0x5d, 0xc9, 0x11,
                                                                    0x91, 0xa4, 0x08, 0x00, 0x2b, 0x14, 0xa0, 0xfa};
        using connection_context = std::array<uint8_t, 20>;

        class LocalObjectExporterTest : public RpcProtocolTest
        {
          protected:
            std::vector<uint8_t> connect_request(uint32_t count = 3)
            {
                std::vector<uint8_t> request;
                utils::aligned_binary_writer writer(request, this->pointer_size);
                writer.write_ndr_pointer(false);
                writer.write_ndr_pointer(false);
                writer.write_ndr_pointer(false);
                writer.write<uint32_t>(0);
                writer.write<uint16_t>(9);
                writer.write(count);
                return request;
            }

            std::vector<uint8_t> allocate_request(const connection_context& context, uint32_t count = 2)
            {
                std::vector<uint8_t> request;
                utils::aligned_binary_writer writer(request, this->pointer_size);
                writer.write(context);
                writer.write(count);
                writer.write<uint32_t>(8);
                writer.write(static_cast<uint32_t>(this->emu->process.process_id));
                writer.write<uint32_t>(5);
                writer.write<uint16_t>(5);
                writer.write<uint16_t>(7);
                writer.write<uint32_t>(3);
                writer.write<uint64_t>(0);
                writer.write_ndr_pointer(false);
                writer.write(std::array<uint8_t, 16>{});
                writer.write<uint32_t>(0x04000000);
                writer.write_ndr_pointer(false);
                writer.write(std::array<uint8_t, 16>{});
                writer.write<uint64_t>(0);
                writer.write<uint32_t>(0);
                return request;
            }

            std::vector<uint8_t> free_request(const connection_context& context, uint64_t oxid, const std::vector<uint64_t>& object_ids)
            {
                std::vector<uint8_t> request;
                utils::aligned_binary_writer writer(request, this->pointer_size);
                writer.write(context);
                writer.write(oxid);
                writer.write(static_cast<uint32_t>(object_ids.size()));
                writer.write(static_cast<uint32_t>(object_ids.size()));
                for (const auto id : object_ids)
                {
                    writer.write(id);
                }
                return request;
            }
        };
    }

    TEST_F(LocalObjectExporterTest, AllocatesDistinctIdsAndDisconnectsBothNdrWidths)
    {
        for (const size_t width : {size_t{4}, size_t{8}})
        {
            this->bind(u"\\RPC Control\\epmapper", exporter_interface, width);
            auto result = this->call(0, this->connect_request());
            ASSERT_EQ(STATUS_SUCCESS, result.status);
            ASSERT_TRUE(result.payload.has_value());
            const auto context = read<connection_context>(*result.payload, 24);
            EXPECT_NE(connection_context{}, context);
            EXPECT_EQ(360u, read<uint32_t>(*result.payload, 44));
            EXPECT_EQ(0u, read<uint32_t>(*result.payload, result.payload->size() - 4));
            result = this->call(6, this->allocate_request(context));
            ASSERT_EQ(STATUS_SUCCESS, result.status);
            ASSERT_TRUE(result.payload.has_value());
            const auto server_oxid = read<uint64_t>(*result.payload, 24);
            const auto primary_oxid = read<uint64_t>(*result.payload, 32);
            EXPECT_NE(0u, server_oxid);
            EXPECT_NE(0u, primary_oxid);
            EXPECT_NE(server_oxid, primary_oxid);
            const auto first_oid = width == 8 ? 64u : 56u;
            EXPECT_EQ(5u, read<uint64_t>(*result.payload, first_oid));
            EXPECT_EQ(6u, read<uint64_t>(*result.payload, first_oid + 8));
            const auto second_result = this->call(6, this->allocate_request(context, 1));
            ASSERT_EQ(STATUS_SUCCESS, second_result.status);
            ASSERT_TRUE(second_result.payload.has_value());
            EXPECT_NE(server_oxid, read<uint64_t>(*second_result.payload, 24));
            EXPECT_EQ(primary_oxid, read<uint64_t>(*second_result.payload, 32));
            std::vector<uint8_t> disconnect;
            utils::aligned_binary_writer writer(disconnect, width);
            writer.write(context);
            writer.write<uint32_t>(0);
            result = this->call(10, disconnect);
            ASSERT_EQ(STATUS_SUCCESS, result.status);
            ASSERT_TRUE(result.payload.has_value());
            EXPECT_EQ(connection_context{}, read<connection_context>(*result.payload, 24));
            EXPECT_EQ(STATUS_INVALID_PARAMETER, this->call(6, this->allocate_request(context)).status);
        }
    }

    TEST_F(LocalObjectExporterTest, EndpointMapperReportsNoRegisteredEndpoints)
    {
        for (const size_t width : {size_t{4}, size_t{8}})
        {
            this->bind(u"\\RPC Control\\epmapper", endpoint_mapper_interface, width);
            for (const uint32_t procedure : {3u, 7u, 8u})
            {
                std::vector<uint8_t> request;
                utils::aligned_binary_writer writer(request, width);
                writer.write_ndr_pointer(false);
                writer.write_ndr_pointer(false);
                writer.write(connection_context{});
                writer.write<uint32_t>(4);
                const auto result = this->call(procedure, request);
                ASSERT_EQ(STATUS_SUCCESS, result.status);
                ASSERT_TRUE(result.payload.has_value());
                EXPECT_EQ(connection_context{}, read<connection_context>(*result.payload, 24));
                EXPECT_EQ(0u, read<uint32_t>(*result.payload, 44));
                EXPECT_EQ(4u, read<uint32_t>(*result.payload, 48));
                EXPECT_EQ(0x16c9a0d6u, read<uint32_t>(*result.payload, result.payload->size() - 4));
            }
            EXPECT_EQ(STATUS_NOT_SUPPORTED, this->call(2, {}).status);
        }
    }

    namespace
    {
        constexpr std::array<uint8_t, 16> ndr_syntax{0x04, 0x5d, 0x88, 0x8a, 0xeb, 0x1c, 0xc9, 0x11,
                                                     0x9f, 0xe8, 0x08, 0x00, 0x2b, 0x10, 0x48, 0x60};
        constexpr std::array<uint8_t, 16> ndr64_syntax{0x33, 0x05, 0x71, 0x71, 0xba, 0xbe, 0x37, 0x49,
                                                       0x83, 0x19, 0xb5, 0xdb, 0xef, 0x9c, 0xcc, 0x36};

        struct tower_floors
        {
            uint16_t interface_major{1};
            bool ndr64{};
            uint8_t transport{0x0c};
            uint8_t endpoint{0x10};
        };

        // A four-floor ncalrpc tower: the interface (UUID and major version, minor version), the NDR or NDR64 transfer syntax
        // (2.0 or 1.0), LRPC and the endpoint name.
        std::vector<uint8_t> lookup_tower(const std::array<uint8_t, 16>& interface_id, const tower_floors& floors = {})
        {
            std::vector<uint8_t> tower{};
            const auto add = [&](const std::vector<uint8_t>& bytes) { tower.insert(tower.end(), bytes.begin(), bytes.end()); };
            const auto floor = [&](std::vector<uint8_t> lhs, std::vector<uint8_t> rhs) {
                add({static_cast<uint8_t>(lhs.size()), 0});
                add(lhs);
                add({static_cast<uint8_t>(rhs.size()), 0});
                add(rhs);
            };
            const auto uuid_floor = [&](const std::array<uint8_t, 16>& uuid, const uint16_t major) {
                std::vector<uint8_t> lhs{0x0d};
                lhs.insert(lhs.end(), uuid.begin(), uuid.end());
                lhs.insert(lhs.end(), {static_cast<uint8_t>(major), static_cast<uint8_t>(major >> 8)});
                floor(lhs, {0, 0});
            };
            add({4, 0});
            uuid_floor(interface_id, floors.interface_major);
            uuid_floor(floors.ndr64 ? ndr64_syntax : ndr_syntax, floors.ndr64 ? 1 : 2);
            floor({floors.transport}, {0, 0});
            floor({floors.endpoint}, {'x', 0});
            return tower;
        }

        std::vector<uint8_t> map_request(const size_t width, const std::optional<std::array<uint8_t, 16>>& object,
                                         const std::vector<uint8_t>& tower)
        {
            std::vector<uint8_t> request;
            utils::aligned_binary_writer writer(request, width);
            writer.write_ndr_pointer(object.has_value());
            if (object)
            {
                writer.write(object->data(), object->size(), 4);
            }
            writer.write_ndr_pointer(true);
            writer.write_pointer_sized(tower.size());
            writer.write<uint32_t>(static_cast<uint32_t>(tower.size()));
            writer.write(tower.data(), tower.size(), 1);
            writer.align_to(4);
            writer.write(std::array<uint8_t, 20>{});
            writer.write<uint32_t>(4);
            return request;
        }

        bool contains(const std::vector<uint8_t>& data, const std::string_view text)
        {
            return !std::ranges::search(data, text).empty();
        }
    }

    TEST_F(LocalObjectExporterTest, EndpointMapperAnswersTheNsiServiceTowerWithItsEndpoint)
    {
        for (const size_t width : {size_t{4}, size_t{8}})
        {
            this->bind(u"\\RPC Control\\epmapper", endpoint_mapper_interface, width);
            const auto result = this->call(3, map_request(width, std::nullopt, lookup_tower(nsi_service_interface, {.ndr64 = width == 8})));
            ASSERT_EQ(STATUS_SUCCESS, result.status);
            ASSERT_TRUE(result.payload.has_value());
            EXPECT_EQ(1u, read<uint32_t>(*result.payload, 44));
            EXPECT_TRUE(contains(*result.payload, "SogenNsiService"));
            EXPECT_EQ(0u, read<uint32_t>(*result.payload, result.payload->size() - 4));
        }
    }

    TEST_F(LocalObjectExporterTest, EndpointMapperOnlyAnswersTheSupportedNsiTower)
    {
        this->bind(u"\\RPC Control\\epmapper", endpoint_mapper_interface, 8);
        for (const auto& floors : {tower_floors{.interface_major = 2}, tower_floors{.transport = 0x07}, tower_floors{.endpoint = 0x0f}})
        {
            const auto result = this->call(3, map_request(8, std::nullopt, lookup_tower(nsi_service_interface, floors)));
            ASSERT_TRUE(result.payload.has_value());
            EXPECT_EQ(0x16c9a0d6u, read<uint32_t>(*result.payload, result.payload->size() - 4));
        }
    }

    TEST_F(LocalObjectExporterTest, EndpointMapperOnlyReadsTheInterfaceFromTheTowerFloor)
    {
        constexpr std::array<uint8_t, 16> other_interface{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
        this->bind(u"\\RPC Control\\epmapper", endpoint_mapper_interface, 8);

        // The NSI interface id as the object UUID, with a tower for another interface.
        auto result = this->call(3, map_request(8, nsi_service_interface, lookup_tower(other_interface)));
        ASSERT_TRUE(result.payload.has_value());
        EXPECT_EQ(0x16c9a0d6u, read<uint32_t>(*result.payload, result.payload->size() - 4));

        // A tower with trailing bytes past its last floor.
        auto tower = lookup_tower(nsi_service_interface);
        tower.push_back(0);
        result = this->call(3, map_request(8, std::nullopt, tower));
        ASSERT_TRUE(result.payload.has_value());
        EXPECT_EQ(0x16c9a0d6u, read<uint32_t>(*result.payload, result.payload->size() - 4));
    }

    TEST_F(LocalObjectExporterTest, RegistrationSurvivesSerializationAndCanBeReleased)
    {
        this->bind(u"\\RPC Control\\epmapper", exporter_interface, 4);
        auto result = this->call(0, this->connect_request());
        ASSERT_EQ(STATUS_SUCCESS, result.status);
        ASSERT_TRUE(result.payload.has_value());
        const auto context = read<connection_context>(*result.payload, 24);
        result = this->call(6, this->allocate_request(context));
        ASSERT_EQ(STATUS_SUCCESS, result.status);
        ASSERT_TRUE(result.payload.has_value());
        const auto server_oxid = read<uint64_t>(*result.payload, 24);
        const auto primary_oxid = read<uint64_t>(*result.payload, 32);
        utils::buffer_serializer state;
        this->endpoint->serialize(state);
        utils::buffer_deserializer reader{state};
        this->endpoint->deserialize(reader);
        EXPECT_EQ(STATUS_SUCCESS, this->call(8, this->free_request(context, primary_oxid, {})).status);
        EXPECT_EQ(STATUS_SUCCESS, this->call(8, this->free_request(context, server_oxid, {5, 6})).status);
        EXPECT_EQ(STATUS_INVALID_HANDLE, this->call(8, this->free_request(context, primary_oxid, {})).status);
    }

    TEST_F(LocalObjectExporterTest, ReadsWindows10ConnectWithoutArchitecture)
    {
        this->emu->version.set_windows_build_number(19041);
        this->bind(u"\\RPC Control\\epmapper", exporter_interface, 4);
        std::vector<uint8_t> request;
        utils::aligned_binary_writer writer(request, this->pointer_size);
        writer.write_ndr_pointer(true);
        writer.write_ndr_u16string(u"WinSta0\\Default");
        writer.write_ndr_pointer(true);
        writer.write_ndr_u16string(u"program.exe");
        writer.write_ndr_pointer(true);
        writer.align_to(8);
        writer.write<uint32_t>(0);
        writer.write<uint64_t>(0);
        writer.write(std::array<uint32_t, 16>{});
        writer.write(std::array<uint8_t, 16>{});
        writer.write<uint32_t>(0);
        writer.write<uint32_t>(3);
        auto result = this->call(0, request);
        ASSERT_EQ(STATUS_SUCCESS, result.status);
        ASSERT_TRUE(result.payload.has_value());
        result = this->call(6, this->allocate_request(read<connection_context>(*result.payload, 24)));
        ASSERT_EQ(STATUS_SUCCESS, result.status);
        ASSERT_TRUE(result.payload.has_value());
        EXPECT_EQ(5u, read<uint64_t>(*result.payload, 56));
    }

    TEST_F(LocalObjectExporterTest, RejectsTruncatedRequestsAndExcessiveReservations)
    {
        this->bind(u"\\RPC Control\\epmapper", exporter_interface, 4);
        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->call(0, {}).status);
        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->call(0, this->connect_request(0x20001)).status);
        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->call(6, {}).status);
        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->call(10, {}).status);
    }
}
