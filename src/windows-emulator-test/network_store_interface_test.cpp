#include "emulation_test_utils.hpp"
#include <io_device.hpp>

namespace sogen::test
{
    namespace
    {
        constexpr ULONG ioctl_get_parameter = 0x120007;
        constexpr ULONG ioctl_get_all_parameters = 0x12000F;
        constexpr ULONG ioctl_enumerate_all_parameters = 0x12001B;

        constexpr uint64_t loopback_luid = 0x0018000000000000;
        constexpr uint64_t ethernet_luid = 0x0006000001000000;

        // iphlpapi's NPI_MS_*_MODULEID: Length 0x18, MIT_GUID, then {eb004aXX-9b1a-11d4-9123-0050047759bc}.
        std::array<uint8_t, 24> module_id(const uint8_t module)
        {
            return {0x18, 0,    0,    0,    1,    0,    0,    0,    module, 0x4a, 0x00, 0xeb,
                    0x1a, 0x9b, 0xd4, 0x11, 0x91, 0x23, 0x00, 0x50, 0x04,   0x77, 0x59, 0xbc};
        }

        constexpr uint8_t ndis_module = 0x11;
        constexpr uint8_t ipv4_module = 0x00;

        windows_emulator create_nsi_emulator(const network::network_mode mode)
        {
            emulator_interfaces interfaces{};
            interfaces.socket_factory = network::create_static_socket_factory();
            interfaces.dns_lookup = create_sample_dns_lookup();
            return create_emulator(emulator_settings{.use_relative_time = true, .network = mode}, {}, std::move(interfaces));
        }

        // The modeled interfaces depend on the network mode, so each mode has its fixture.
        template <network::network_mode Mode>
        class NetworkStoreInterfaceFixture : public testing::Test
        {
          protected:
            void SetUp() override
            {
                this->storage = this->emu.memory.allocate_memory(0x4000, memory_permission::read_write);
            }

            template <typename T>
            static void put(std::array<uint8_t, 0x70>& request, const size_t offset, const T& value)
            {
                std::memcpy(request.data() + offset, &value, sizeof(value));
            }

            // The 64-bit request nsi.dll builds: 16 reserved bytes, the module pointer, the object index and the store.
            std::array<uint8_t, 0x70> request(const uint8_t module, const uint32_t object)
            {
                this->emu.memory.write_memory(this->module_address(), module_id(module));
                std::array<uint8_t, 0x70> request{};
                put(request, 0x10, this->module_address());
                put(request, 0x18, object);
                put(request, 0x20, uint32_t{1});
                return request;
            }

            NTSTATUS send(const ULONG code, const std::array<uint8_t, 0x70>& request, const ULONG length)
            {
                this->emu.memory.write_memory(this->storage, request.data(), length);
                this->context.io_control_code = code;
                this->context.io_status_block.set_address(this->storage + 0x80);
                this->context.input_buffer = this->storage;
                this->context.input_buffer_length = length;
                this->context.output_buffer = this->storage;
                this->context.output_buffer_length = length;

                io_device_container device{u"Nsi", this->emu, {}};
                return device.execute_ioctl(this->emu, this->context);
            }

            uint32_t best_interface(const std::array<uint8_t, 4> destination)
            {
                std::array<uint8_t, 32> key{};
                std::memcpy(key.data() + 4, destination.data(), destination.size());
                this->emu.memory.write_memory(this->key_address(), key);

                auto r = this->request(ipv4_module, 0);
                put(r, 0x28, this->key_address());
                put(r, 0x30, static_cast<uint32_t>(key.size()));
                put(r, 0x38, uint32_t{1}); // dynamic
                put(r, 0x40, this->output_address());
                put(r, 0x48, uint32_t{4});
                put(r, 0x4C, uint32_t{88});
                EXPECT_EQ(STATUS_SUCCESS, this->send(ioctl_get_parameter, r, 0x50));
                return this->emu.memory.template read_memory<uint32_t>(this->output_address());
            }

            uint64_t module_address() const
            {
                return this->storage + 0x100;
            }

            uint64_t key_address() const
            {
                return this->storage + 0x200;
            }

            uint64_t output_address() const
            {
                return this->storage + 0x300;
            }

            uint64_t dynamic_address() const
            {
                return this->storage + 0x800;
            }

            uint64_t static_address() const
            {
                return this->storage + 0x2000;
            }

            windows_emulator emu = create_nsi_emulator(Mode);
            io_device_context context{this->emu.memory};
            uint64_t storage{};
        };

        using NetworkStoreInterfaceTest = NetworkStoreInterfaceFixture<network::network_mode::live>;
        using OfflineNetworkStoreInterfaceTest = NetworkStoreInterfaceFixture<network::network_mode::offline>;
    }

    TEST_F(NetworkStoreInterfaceTest, BestInterfaceIsEthernetExceptForLoopbackDestinations)
    {
        EXPECT_EQ(7u, this->best_interface({0, 0, 0, 0}));
        EXPECT_EQ(7u, this->best_interface({8, 8, 8, 8}));
        EXPECT_EQ(1u, this->best_interface({127, 0, 0, 1}));
    }

    TEST_F(NetworkStoreInterfaceTest, InterfaceIndexMapsToItsLuid)
    {
        for (const auto& [index, expected] : {std::pair{7u, STATUS_SUCCESS}, std::pair{99u, STATUS_NOT_FOUND}})
        {
            this->emu.memory.write_memory(this->key_address(), index);
            auto r = this->request(ndis_module, 2);
            put(r, 0x28, this->key_address());
            put(r, 0x30, uint32_t{4});
            put(r, 0x38, uint32_t{2}); // static
            put(r, 0x40, this->output_address());
            put(r, 0x48, uint32_t{8});
            EXPECT_EQ(expected, this->send(ioctl_get_parameter, r, 0x50));
        }

        this->emu.memory.write_memory(this->key_address(), uint32_t{7});
        auto r = this->request(ndis_module, 2);
        put(r, 0x28, this->key_address());
        put(r, 0x30, uint32_t{4});
        put(r, 0x38, uint32_t{2});
        put(r, 0x40, this->output_address());
        put(r, 0x48, uint32_t{8});
        ASSERT_EQ(STATUS_SUCCESS, this->send(ioctl_get_parameter, r, 0x50));
        EXPECT_EQ(ethernet_luid, this->emu.memory.read_memory<uint64_t>(this->output_address()));
    }

    TEST_F(NetworkStoreInterfaceTest, EthernetRowIsAConnectedLan)
    {
        // GetIfEntry2Ex(MibIfEntryNormalWithoutStatistics) reads object 1: no rw part, 656-byte dynamic, 568-byte static.
        this->emu.memory.write_memory(this->key_address(), ethernet_luid);
        auto r = this->request(ndis_module, 1);
        put(r, 0x28, this->key_address());
        put(r, 0x30, uint32_t{8});
        put(r, 0x48, this->dynamic_address());
        put(r, 0x50, uint32_t{656});
        put(r, 0x58, this->static_address());
        put(r, 0x60, uint32_t{568});
        ASSERT_EQ(STATUS_SUCCESS, this->send(ioctl_get_all_parameters, r, 0x68));

        const auto dynamic = [&](const uint64_t offset) {
            return this->emu.memory.read_memory<uint32_t>(this->dynamic_address() + offset);
        };
        const auto static_parameter = [&](const uint64_t offset) {
            return this->emu.memory.read_memory<uint32_t>(this->static_address() + offset);
        };
        EXPECT_EQ(7u, static_parameter(0));            // InterfaceIndex
        EXPECT_EQ(6u, static_parameter(520) & 0xFFFF); // IF_TYPE_ETHERNET_CSMACD
        EXPECT_EQ(2u, static_parameter(524));          // NET_IF_ACCESS_BROADCAST
        EXPECT_EQ(0u, static_parameter(528));          // NET_IF_DIRECTION_SENDRECEIVE
        EXPECT_EQ(1u, static_parameter(532));          // NET_IF_CONNECTION_DEDICATED
        EXPECT_EQ(1u, dynamic(536));                   // IfOperStatusUp
        EXPECT_EQ(6u, dynamic(548) & 0xFFFF);          // PhysicalAddressLength
        EXPECT_EQ(1u, dynamic(616));                   // NET_IF_ADMIN_STATUS_UP
        EXPECT_EQ(1u, dynamic(644));                   // MediaConnectStateConnected

        this->emu.memory.write_memory(this->key_address(), uint64_t{0x1234});
        r = this->request(ndis_module, 1);
        put(r, 0x28, this->key_address());
        put(r, 0x30, uint32_t{8});
        EXPECT_EQ(STATUS_NOT_FOUND, this->send(ioctl_get_all_parameters, r, 0x68));
    }

    TEST_F(NetworkStoreInterfaceTest, EnumerationCountsThenFillsLoopbackAndEthernet)
    {
        auto r = this->request(ndis_module, 1);
        ASSERT_EQ(STATUS_SUCCESS, this->send(ioctl_enumerate_all_parameters, r, 0x70));
        EXPECT_EQ(2u, this->emu.memory.read_memory<uint32_t>(this->storage + 0x68));

        r = this->request(ndis_module, 1);
        put(r, 0x28, this->key_address());
        put(r, 0x30, uint32_t{8});
        put(r, 0x48, this->dynamic_address());
        put(r, 0x50, uint32_t{656});
        put(r, 0x58, this->static_address());
        put(r, 0x60, uint32_t{568});
        put(r, 0x68, uint32_t{4});
        ASSERT_EQ(STATUS_SUCCESS, this->send(ioctl_enumerate_all_parameters, r, 0x70));
        ASSERT_EQ(2u, this->emu.memory.read_memory<uint32_t>(this->storage + 0x68));
        EXPECT_EQ(loopback_luid, this->emu.memory.read_memory<uint64_t>(this->key_address()));
        EXPECT_EQ(ethernet_luid, this->emu.memory.read_memory<uint64_t>(this->key_address() + 8));
        EXPECT_EQ(1u, this->emu.memory.read_memory<uint32_t>(this->dynamic_address()));       // loopback compartment
        EXPECT_EQ(1u, this->emu.memory.read_memory<uint32_t>(this->dynamic_address() + 656)); // Ethernet compartment
        EXPECT_EQ(7u, this->emu.memory.read_memory<uint32_t>(this->static_address() + 568));  // Ethernet InterfaceIndex
    }

    // Offline the guest has only its loopback, as the AFD address list says: enumerations list it alone, the Ethernet
    // adapter is not found, and every destination's best interface is the loopback, so the guest takes its offline path.
    TEST_F(OfflineNetworkStoreInterfaceTest, OnlyTheLoopbackIsReported)
    {
        EXPECT_EQ(1u, this->best_interface({8, 8, 8, 8}));
        EXPECT_EQ(1u, this->best_interface({127, 0, 0, 1}));

        auto r = this->request(ndis_module, 1);
        ASSERT_EQ(STATUS_SUCCESS, this->send(ioctl_enumerate_all_parameters, r, 0x70));
        EXPECT_EQ(1u, this->emu.memory.read_memory<uint32_t>(this->storage + 0x68));

        this->emu.memory.write_memory(this->key_address(), ethernet_luid);
        r = this->request(ndis_module, 1);
        put(r, 0x28, this->key_address());
        put(r, 0x30, uint32_t{8});
        EXPECT_EQ(STATUS_NOT_FOUND, this->send(ioctl_get_all_parameters, r, 0x68));
    }

    // The requests are METHOD_NEITHER: an output buffer that cannot take its write fails the request with nothing written,
    // and so does a declared size the caller cannot write, instead of the emulator clearing it.
    TEST_F(NetworkStoreInterfaceTest, OutputBuffersThatCannotBeWrittenFailTheRequest)
    {
        const auto read_only = this->emu.memory.allocate_memory(0x1000, memory_permission::read);
        this->emu.memory.write_memory(this->key_address(), ethernet_luid);
        auto r = this->request(ndis_module, 1);
        put(r, 0x28, this->key_address());
        put(r, 0x30, uint32_t{8});
        put(r, 0x48, read_only);
        put(r, 0x50, uint32_t{656});
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->send(ioctl_get_all_parameters, r, 0x68));
        EXPECT_EQ(0u, this->emu.memory.read_memory<uint64_t>(read_only));

        put(r, 0x48, this->dynamic_address());
        put(r, 0x50, uint32_t{0xFFFFFFF0});
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->send(ioctl_get_all_parameters, r, 0x68));

        r = this->request(ndis_module, 1);
        put(r, 0x28, read_only);
        put(r, 0x30, uint32_t{8});
        put(r, 0x68, uint32_t{4});
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->send(ioctl_enumerate_all_parameters, r, 0x70));
        EXPECT_EQ(0u, this->emu.memory.read_memory<uint64_t>(read_only));
    }

    // NsippProbeAndAllocateParameters probes the key for write and reads the module ID's 24 bytes, so a key or a module ID
    // the caller cannot access fails the request with STATUS_ACCESS_VIOLATION before anything is answered.
    TEST_F(NetworkStoreInterfaceTest, KeyOrModuleIdThatCannotBeAccessedFailsTheRequest)
    {
        const auto no_access = this->emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        this->emu.memory.write_memory(no_access, ethernet_luid);
        this->emu.memory.write_memory(no_access + 0x100, module_id(ndis_module));
        ASSERT_TRUE(this->emu.memory.protect_memory(no_access, 0x1000, memory_permission::none));

        auto r = this->request(ndis_module, 1);
        put(r, 0x28, no_access);
        put(r, 0x30, uint32_t{8});
        put(r, 0x48, this->dynamic_address());
        put(r, 0x50, uint32_t{656});
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->send(ioctl_get_all_parameters, r, 0x68));
        EXPECT_EQ(0u, this->emu.memory.read_memory<uint32_t>(this->dynamic_address()));

        r = this->request(ndis_module, 2);
        put(r, 0x28, no_access);
        put(r, 0x30, uint32_t{4});
        put(r, 0x38, uint32_t{2}); // static
        put(r, 0x40, this->output_address());
        put(r, 0x48, uint32_t{8});
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->send(ioctl_get_parameter, r, 0x50));
        EXPECT_EQ(0u, this->emu.memory.read_memory<uint64_t>(this->output_address()));

        for (const auto module : {uint64_t{0}, no_access + 0x100})
        {
            this->emu.memory.write_memory(this->key_address(), ethernet_luid);
            r = this->request(ndis_module, 1);
            put(r, 0x10, module);
            put(r, 0x28, this->key_address());
            put(r, 0x30, uint32_t{8});
            put(r, 0x48, this->dynamic_address());
            put(r, 0x50, uint32_t{656});
            EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->send(ioctl_get_all_parameters, r, 0x68)) << std::hex << module;
            EXPECT_EQ(0u, this->emu.memory.read_memory<uint32_t>(this->dynamic_address()));
        }
    }

    // NsippEnumerateObjectsAllParameters refuses a request shorter than its 0x70-byte layout before it writes the count back.
    TEST_F(NetworkStoreInterfaceTest, ShortEnumerationFailsWithNothingWritten)
    {
        for (const ULONG length : {0x6CU, 0x6FU})
        {
            auto r = this->request(ndis_module, 1);
            put(r, 0x68, uint32_t{0xAAAA});
            EXPECT_EQ(STATUS_INVALID_PARAMETER, this->send(ioctl_enumerate_all_parameters, r, length)) << length;
            EXPECT_EQ(0xAAAAu, this->emu.memory.read_memory<uint32_t>(this->storage + 0x68)) << length;
        }
    }
}
