#include "../std_include.hpp"
#include "network_store_interface.hpp"

#include "../windows_emulator.hpp"
#include "../syscall_utils.hpp"

namespace sogen
{
    namespace
    {
        constexpr ULONG k_nsi_get_parameter = 0x120007;
        constexpr ULONG k_nsi_get_all_parameters = 0x12000F;
        constexpr ULONG k_nsi_enumerate_objects_all_parameters = 0x12001B;
        constexpr uint64_t k_adapter_index = 7;

        constexpr std::array<uint8_t, 0xB8> k_adapter_parameter = {
            0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x19, 0x00, 0x00, 0x00, 0x30, 0x75, 0x00, 0x00, 0xE8, 0x03, 0x00, 0x00, 0xC0, 0x27,
            0x09, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x64, 0x19, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00,
            0x00, 0x07, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
            0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
            0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xDC, 0x05, 0x00, 0x00, 0x40, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x07, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x35, 0x97, 0x98, 0x00, 0x00, 0x00, 0x00, 0x00,
        };

        struct nsi_buffer
        {
            uint64_t address{};
            uint64_t size{};
        };

        // nsi.dll passes the NPI module by pointer; iphlpapi's NPI_MS_*_MODULEID GUIDs are {eb004aXX-9b1a-11d4-9123-0050047759bc}.
        enum class nsi_module
        {
            other,
            ndis,
            ipv4,
            ipv6,
        };

        constexpr uint32_t k_ndis_interface_info = 0; // GetIfEntry2, GetIfTable2
        constexpr uint32_t k_ndis_interface_enum = 1; // GetIfEntry2Ex(MibIfEntryNormalWithoutStatistics)
        constexpr uint32_t k_ndis_index_luid = 2;     // interface index -> NET_LUID
        constexpr uint32_t k_ip_best_route = 0;       // GetBestInterfaceEx

        enum class nsi_parameter_type : uint32_t
        {
            rw = 0,
            dynamic = 1,
            static_parameters = 2,
        };

        struct emulated_interface
        {
            uint32_t index{};
            uint64_t luid{};
            uint16_t type{};
            std::u16string_view alias{};
            std::u16string_view description{};
            uint16_t physical_address_length{};
            std::array<uint8_t, 6> physical_address{};
            uint32_t mtu{};
            uint64_t link_speed{};
            uint32_t access_type{};
            uint32_t media_type{};
            uint32_t physical_medium{};
            bool hardware{};
            std::array<uint8_t, 16> guid{};
        };

        // The guest sees its loopback and one connected Ethernet adapter, the interface its traffic leaves through.
        constexpr std::array<emulated_interface, 2> k_interfaces{{
            {
                .index = 1,
                .luid = 0x0018000000000000, // IF_TYPE_SOFTWARE_LOOPBACK
                .type = 24,
                .alias = u"Loopback Pseudo-Interface 1",
                .description = u"Software Loopback Interface 1",
                .mtu = 0xFFFFFFFF,
                .link_speed = 1073741824,
                .access_type = 1, // NET_IF_ACCESS_LOOPBACK
                .media_type = 17, // NdisMediumLoopback
                .physical_medium = 0,
                .guid = {0x01, 0x4f, 0x67, 0x53, 0x6e, 0x65, 0x6c, 0x4c, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01},
            },
            {
                .index = static_cast<uint32_t>(k_adapter_index),
                .luid = 0x0006000001000000, // IF_TYPE_ETHERNET_CSMACD, NetLuidIndex 1
                .type = 6,
                .alias = u"Ethernet",
                .description = u"Sogen Ethernet Adapter",
                .physical_address_length = 6,
                .physical_address = {0x02, 0x00, 0x00, 0x00, 0x00, 0x07},
                .mtu = 1500,
                .link_speed = 1000000000,
                .access_type = 2,      // NET_IF_ACCESS_BROADCAST
                .media_type = 0,       // NdisMedium802_3
                .physical_medium = 14, // NdisPhysicalMedium802_3
                .hardware = true,
                .guid = {0x07, 0x4f, 0x67, 0x53, 0x6e, 0x65, 0x6c, 0x4c, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07},
            },
        }};

        constexpr uint32_t k_default_compartment = 1;
        constexpr uint32_t k_status_up = 1;    // NET_IF_ADMIN_STATUS_UP, IfOperStatusUp
        constexpr uint32_t k_connected = 1;    // MediaConnectStateConnected
        constexpr uint32_t k_dedicated = 1;    // NET_IF_CONNECTION_DEDICATED
        constexpr uint32_t k_send_receive = 0; // NET_IF_DIRECTION_SENDRECEIVE

        // Offline the guest has only its loopback, as the AFD address list reports and the offline socket factory serves;
        // only a live network adds the Ethernet adapter its traffic leaves through.
        std::span<const emulated_interface> visible_interfaces(const windows_emulator& win_emu)
        {
            const std::span<const emulated_interface> interfaces{k_interfaces};
            return win_emu.network_mode() == network::network_mode::live ? interfaces : interfaces.first(1);
        }

        const emulated_interface* find_interface_by_luid(const windows_emulator& win_emu, const uint64_t luid)
        {
            const auto interfaces = visible_interfaces(win_emu);
            const auto entry = std::ranges::find(interfaces, luid, &emulated_interface::luid);
            return entry == interfaces.end() ? nullptr : &*entry;
        }

        const emulated_interface* find_interface_by_index(const windows_emulator& win_emu, const uint32_t index)
        {
            const auto interfaces = visible_interfaces(win_emu);
            const auto entry = std::ranges::find(interfaces, index, &emulated_interface::index);
            return entry == interfaces.end() ? nullptr : &*entry;
        }

        template <typename T>
        void put(std::vector<uint8_t>& bytes, const size_t offset, const T& value)
        {
            std::memcpy(bytes.data() + offset, &value, sizeof(value));
        }

        void put_counted_string(std::vector<uint8_t>& bytes, const size_t offset, const std::u16string_view text)
        {
            put(bytes, offset, static_cast<uint16_t>(text.size() * sizeof(char16_t)));
            std::memcpy(bytes.data() + offset + sizeof(uint16_t), text.data(), text.size() * sizeof(char16_t));
        }

        // The NDIS interface objects, laid out as iphlpapi's FillIfEntryWithInterfaceInfo (object 0: rw 1092, dynamic 216,
        // static 600 bytes) and FillIfEntryWithInterfaceEnum (object 1: dynamic 656, static 568 bytes) read them.
        std::vector<uint8_t> interface_parameters(const emulated_interface& iface, const uint32_t object, const nsi_parameter_type type)
        {
            const bool info = object == k_ndis_interface_info;
            if (type == nsi_parameter_type::rw)
            {
                if (!info)
                {
                    return {};
                }

                std::vector<uint8_t> rw(1092);
                put(rw, 0, iface.guid); // network GUID
                put(rw, 16, k_status_up);
                put_counted_string(rw, 20, iface.alias);
                put(rw, 536, iface.physical_address_length);
                put(rw, 538, iface.physical_address);
                return rw;
            }

            if (type == nsi_parameter_type::dynamic)
            {
                std::vector<uint8_t> dynamic(info ? 216 : 656);
                if (info)
                {
                    put(dynamic, 0, k_status_up);
                    put(dynamic, 8, k_connected);
                    put(dynamic, 16, iface.mtu);
                    put(dynamic, 24, iface.link_speed);
                    put(dynamic, 32, iface.link_speed);
                    // WinINet's _CheckAdapterConnectedLanAlike rejects an adapter that never carried a unicast packet.
                    put(dynamic, 88, uint64_t{1});  // InUcastPkts
                    put(dynamic, 120, uint64_t{1}); // OutUcastPkts
                    put(dynamic, 208, k_default_compartment);
                }
                else
                {
                    put(dynamic, 0, k_default_compartment);
                    put(dynamic, 4, iface.guid); // network GUID
                    put_counted_string(dynamic, 20, iface.alias);
                    put(dynamic, 536, k_status_up);
                    put(dynamic, 544, iface.mtu);
                    put(dynamic, 548, iface.physical_address_length);
                    put(dynamic, 550, iface.physical_address);
                    put(dynamic, 584, iface.physical_address);
                    put(dynamic, 616, k_status_up);
                    put(dynamic, 624, iface.link_speed);
                    put(dynamic, 632, iface.link_speed);
                    put(dynamic, 644, k_connected);
                }
                return dynamic;
            }

            std::vector<uint8_t> static_parameters(info ? 600 : 568);
            put(static_parameters, 0, iface.index);
            put_counted_string(static_parameters, 4, iface.description);
            put(static_parameters, 520, iface.type);
            put(static_parameters, 524, iface.access_type);
            put(static_parameters, 528, k_send_receive);
            put(static_parameters, 532, k_dedicated);
            put(static_parameters, 536, iface.guid);
            put(static_parameters, 552, static_cast<uint8_t>(iface.hardware)); // connector present
            if (info)
            {
                put(static_parameters, 556, iface.physical_address);
                put(static_parameters, 588, static_cast<uint8_t>(iface.hardware)); // HardwareInterface
                put(static_parameters, 592, iface.media_type);
                put(static_parameters, 596, iface.physical_medium);
            }
            else
            {
                put(static_parameters, 556, static_cast<uint8_t>(iface.hardware)); // HardwareInterface
                put(static_parameters, 560, iface.media_type);
                put(static_parameters, 564, iface.physical_medium);
            }
            return static_parameters;
        }

        // GetAllParameters and EnumerateObjectsAllParameters pass four buffers: the object key and its read-write,
        // dynamic and static parameters. Pointers are native words, sizes are ULONGs, so under WoW64 the descriptors
        // are packed tighter.
        struct nsi_all_parameters
        {
            nsi_buffer key;
            nsi_buffer rw;
            nsi_buffer dynamic;
            nsi_buffer static_parameters;
        };

        uint64_t read_pointer(windows_emulator& win_emu, const io_device_context& c, const ULONG offset, const bool wow64)
        {
            const ULONG width = wow64 ? sizeof(uint32_t) : sizeof(uint64_t);
            if (c.input_buffer_length < offset + width)
            {
                return 0;
            }

            if (wow64)
            {
                return win_emu.emu().read_memory<uint32_t>(c.input_buffer + offset);
            }

            return win_emu.emu().read_memory<uint64_t>(c.input_buffer + offset);
        }

        uint64_t read_size(windows_emulator& win_emu, const io_device_context& c, const ULONG offset)
        {
            if (c.input_buffer_length < offset + sizeof(ULONG))
            {
                return 0;
            }

            return win_emu.emu().read_memory<ULONG>(c.input_buffer + offset);
        }

        nsi_buffer read_buffer(windows_emulator& win_emu, const io_device_context& c, const ULONG offset, const bool wow64)
        {
            const ULONG pointer_size = wow64 ? sizeof(uint32_t) : sizeof(uint64_t);
            return nsi_buffer{.address = read_pointer(win_emu, c, offset, wow64), .size = read_size(win_emu, c, offset + pointer_size)};
        }

        nsi_all_parameters read_all_parameters(windows_emulator& win_emu, const io_device_context& c, const bool wow64)
        {
            const ULONG first = wow64 ? 0x18 : 0x28;
            const ULONG stride = wow64 ? 0x8 : 0x10;
            return nsi_all_parameters{
                .key = read_buffer(win_emu, c, first, wow64),
                .rw = read_buffer(win_emu, c, first + stride, wow64),
                .dynamic = read_buffer(win_emu, c, first + (2 * stride), wow64),
                .static_parameters = read_buffer(win_emu, c, first + (3 * stride), wow64),
            };
        }

        // 64-bit request header: 16 reserved bytes, the NPI_MODULEID pointer, then the object index. probe_request has checked
        // that the module ID can be read.
        nsi_module read_module(windows_emulator& win_emu, const io_device_context& c)
        {
            constexpr std::array<uint8_t, 15> shared_guid_bytes = {0x4a, 0x00, 0xeb, 0x1a, 0x9b, 0xd4, 0x11, 0x91,
                                                                   0x23, 0x00, 0x50, 0x04, 0x77, 0x59, 0xbc};
            const auto module_id = read_pointer(win_emu, c, 0x10, false);
            std::array<uint8_t, 16> guid{};
            win_emu.memory.read_memory(module_id + 8, guid.data(), guid.size());
            if (!std::equal(shared_guid_bytes.begin(), shared_guid_bytes.end(), guid.begin() + 1))
            {
                return nsi_module::other;
            }

            switch (guid[0])
            {
            case 0x11:
                return nsi_module::ndis;
            case 0x00:
                return nsi_module::ipv4;
            case 0x01:
                return nsi_module::ipv6;
            default:
                return nsi_module::other;
            }
        }

        // probe_request has checked the key's declared size.
        template <typename T>
        std::optional<T> read_key(windows_emulator& win_emu, const nsi_buffer key)
        {
            T value{};
            if (!key.address || key.size < sizeof(value))
            {
                return std::nullopt;
            }

            win_emu.memory.read_memory(key.address, &value, sizeof(value));
            return value;
        }

        // NsippProbeAndAllocateParameters, for a 64-bit caller. The requests are METHOD_NEITHER, so the module ID and every
        // buffer a request names are the caller's own memory. The module ID's 24 bytes are read, and each buffer is probed
        // for write with count * size bytes, the key too, since an enumeration writes keys back; a null buffer passes only
        // with no bytes. A fault fails the request with STATUS_ACCESS_VIOLATION, and a size past 32 bits with
        // STATUS_INVALID_PARAMETER, before anything is written.
        NTSTATUS probe_request(windows_emulator& win_emu, const io_device_context& c, const std::span<const nsi_buffer> buffers,
                               const uint64_t count)
        {
            constexpr uint64_t module_id_size = 24;
            if (!is_readable_range(win_emu.memory, read_pointer(win_emu, c, 0x10, false), module_id_size))
            {
                return STATUS_ACCESS_VIOLATION;
            }

            for (const auto& buffer : buffers)
            {
                const auto bytes = buffer.size * count;
                if (bytes > std::numeric_limits<uint32_t>::max())
                {
                    return STATUS_INVALID_PARAMETER;
                }
                if (buffer.address ? !is_writable_range(win_emu.memory, buffer.address, bytes) : bytes != 0)
                {
                    return STATUS_ACCESS_VIOLATION;
                }
            }

            return STATUS_SUCCESS;
        }

        // The modeled row, as far as the caller's buffer holds it; the row is the whole modeled structure, so nothing past it
        // is cleared, whatever size the caller declared.
        void write_parameters(windows_emulator& win_emu, const nsi_buffer buffer, const std::span<const uint8_t> parameters)
        {
            if (!buffer.address || !buffer.size)
            {
                return;
            }

            win_emu.emu().write_memory(buffer.address, parameters.data(),
                                       std::min<size_t>(static_cast<size_t>(buffer.size), parameters.size()));
        }

        void write_io_information(const io_device_context& c, const uint64_t information)
        {
            if (!c.io_status_block)
            {
                return;
            }

            IO_STATUS_BLOCK<EmulatorTraits<Emu64>> block{};
            block.Information = information;
            c.io_status_block.write(block);
        }

        void write_zeroes(windows_emulator& win_emu, const nsi_buffer buffer)
        {
            if (!buffer.address || !buffer.size)
            {
                return;
            }

            win_emu.emu().set_memory(buffer.address, 0, static_cast<size_t>(buffer.size));
        }

        void write_adapter_key(windows_emulator& win_emu, const nsi_buffer buffer)
        {
            if (!buffer.address || buffer.size < sizeof(uint64_t))
            {
                return;
            }

            win_emu.emu().write_memory<uint64_t>(buffer.address, k_adapter_index);
        }

        void write_adapter_parameter(windows_emulator& win_emu, const nsi_buffer buffer)
        {
            if (!buffer.address || !buffer.size)
            {
                return;
            }

            const auto bytes_to_write = std::min<size_t>(static_cast<size_t>(buffer.size), k_adapter_parameter.size());
            win_emu.emu().write_memory(buffer.address, k_adapter_parameter.data(), bytes_to_write);
        }

        void write_adapter_object(windows_emulator& win_emu, const nsi_all_parameters& parameters)
        {
            write_adapter_key(win_emu, parameters.key);
            write_zeroes(win_emu, parameters.rw);
            write_zeroes(win_emu, parameters.dynamic);
            write_adapter_parameter(win_emu, parameters.dynamic);
            write_zeroes(win_emu, parameters.static_parameters);
        }

        struct network_store_interface_device : stateless_device
        {
            NTSTATUS io_control(windows_emulator& win_emu, const io_device_context& c) override
            {
                const bool wow64 = win_emu.process.is_wow64_process;
                // ponytail: 32-bit callers keep the placeholder answers below until their request header is checked.
                if (!wow64)
                {
                    if (const auto status = answer_from_model(win_emu, c))
                    {
                        write_io_information(c, 0);
                        return *status;
                    }
                }

                switch (c.io_control_code)
                {
                case k_nsi_get_parameter:
                    return get_parameter(win_emu, c, wow64);
                case k_nsi_get_all_parameters:
                    write_adapter_object(win_emu, read_all_parameters(win_emu, c, wow64));
                    write_io_information(c, 0);
                    return STATUS_SUCCESS;
                case k_nsi_enumerate_objects_all_parameters:
                    return enumerate_objects_all_parameters(win_emu, c, wow64);
                default:
                    return STATUS_SUCCESS;
                }
            }

            static std::optional<NTSTATUS> answer_from_model(windows_emulator& win_emu, const io_device_context& c)
            {
                // NsippGetParameter, NsippGetAllParameters and NsippEnumerateObjectsAllParameters refuse a request shorter
                // than their 64-bit layout. An enumeration writes its count back into the request, so it probes all of it for
                // write; the others read their layout from it.
                ULONG layout_size{};
                switch (c.io_control_code)
                {
                case k_nsi_get_parameter:
                    layout_size = 0x50;
                    break;
                case k_nsi_get_all_parameters:
                    layout_size = 0x68;
                    break;
                case k_nsi_enumerate_objects_all_parameters:
                    layout_size = 0x70;
                    break;
                default:
                    return std::nullopt;
                }

                const bool enumeration = c.io_control_code == k_nsi_enumerate_objects_all_parameters;
                if (!c.input_buffer || c.input_buffer_length < layout_size)
                {
                    return STATUS_INVALID_PARAMETER;
                }
                if (enumeration ? !is_writable_range(win_emu.memory, c.input_buffer, c.input_buffer_length)
                                : !is_readable_range(win_emu.memory, c.input_buffer, layout_size))
                {
                    return STATUS_ACCESS_VIOLATION;
                }

                const auto parameters = read_all_parameters(win_emu, c, false);
                const auto probed =
                    c.io_control_code == k_nsi_get_parameter
                        ? probe_request(win_emu, c, std::array{parameters.key, read_buffer(win_emu, c, 0x40, false)}, 1)
                        : probe_request(win_emu, c,
                                        std::array{parameters.key, parameters.rw, parameters.dynamic, parameters.static_parameters},
                                        enumeration ? read_size(win_emu, c, 0x68) : 1);
                if (probed != STATUS_SUCCESS)
                {
                    return probed;
                }

                const auto module = read_module(win_emu, c);
                const auto object = static_cast<uint32_t>(read_size(win_emu, c, 0x18));
                const bool interface_object =
                    module == nsi_module::ndis && (object == k_ndis_interface_info || object == k_ndis_interface_enum);

                switch (c.io_control_code)
                {
                case k_nsi_get_all_parameters:
                    return interface_object ? std::optional{get_interface(win_emu, c, object)} : std::nullopt;
                case k_nsi_enumerate_objects_all_parameters:
                    return interface_object ? std::optional{enumerate_interfaces(win_emu, c, object)} : std::nullopt;
                case k_nsi_get_parameter:
                    return get_modeled_parameter(win_emu, c, module, object);
                default:
                    return std::nullopt;
                }
            }

            static NTSTATUS get_interface(windows_emulator& win_emu, const io_device_context& c, const uint32_t object)
            {
                const auto parameters = read_all_parameters(win_emu, c, false);
                const auto luid = read_key<uint64_t>(win_emu, parameters.key);
                if (!luid)
                {
                    return STATUS_INVALID_PARAMETER;
                }

                const auto* iface = find_interface_by_luid(win_emu, *luid);
                if (!iface)
                {
                    return STATUS_NOT_FOUND;
                }

                write_parameters(win_emu, parameters.rw, interface_parameters(*iface, object, nsi_parameter_type::rw));
                write_parameters(win_emu, parameters.dynamic, interface_parameters(*iface, object, nsi_parameter_type::dynamic));
                write_parameters(win_emu, parameters.static_parameters,
                                 interface_parameters(*iface, object, nsi_parameter_type::static_parameters));
                return STATUS_SUCCESS;
            }

            // nsi.dll first asks for the entry count with no buffers, then passes room for that many entries.
            static NTSTATUS enumerate_interfaces(windows_emulator& win_emu, const io_device_context& c, const uint32_t object)
            {
                constexpr ULONG count_offset = 0x68;
                const auto parameters = read_all_parameters(win_emu, c, false);
                const auto capacity = static_cast<size_t>(read_size(win_emu, c, count_offset));
                const bool wants_entries = capacity != 0 && (parameters.key.address || parameters.rw.address ||
                                                             parameters.dynamic.address || parameters.static_parameters.address);
                const auto interfaces = visible_interfaces(win_emu);
                const auto count = wants_entries ? std::min(capacity, interfaces.size()) : interfaces.size();

                if (wants_entries)
                {
                    const auto entry = [](const nsi_buffer buffer, const size_t index) {
                        return nsi_buffer{.address = buffer.address ? buffer.address + (index * buffer.size) : 0, .size = buffer.size};
                    };

                    for (size_t index = 0; index < count; ++index)
                    {
                        const auto& iface = interfaces[index];
                        const auto* luid = reinterpret_cast<const uint8_t*>(&iface.luid);
                        write_parameters(win_emu, entry(parameters.key, index), std::span{luid, sizeof(iface.luid)});
                        write_parameters(win_emu, entry(parameters.rw, index), interface_parameters(iface, object, nsi_parameter_type::rw));
                        write_parameters(win_emu, entry(parameters.dynamic, index),
                                         interface_parameters(iface, object, nsi_parameter_type::dynamic));
                        write_parameters(win_emu, entry(parameters.static_parameters, index),
                                         interface_parameters(iface, object, nsi_parameter_type::static_parameters));
                    }
                }

                win_emu.emu().write_memory<ULONG>(c.input_buffer + count_offset, static_cast<ULONG>(count));
                return STATUS_SUCCESS;
            }

            static std::optional<NTSTATUS> get_modeled_parameter(windows_emulator& win_emu, const io_device_context& c,
                                                                 const nsi_module module, const uint32_t object)
            {
                const auto key = read_buffer(win_emu, c, 0x28, false);
                const auto type = static_cast<nsi_parameter_type>(read_size(win_emu, c, 0x38));
                const auto output = read_buffer(win_emu, c, 0x40, false);
                const auto offset = static_cast<size_t>(read_size(win_emu, c, 0x4C));
                if (!output.address)
                {
                    return std::nullopt;
                }

                std::vector<uint8_t> parameters{};
                const bool ip = module == nsi_module::ipv4 || module == nsi_module::ipv6;
                if (ip && object == k_ip_best_route && type == nsi_parameter_type::dynamic && output.size == sizeof(uint32_t) &&
                    offset == (module == nsi_module::ipv4 ? 88u : 124u))
                {
                    // GetBestInterfaceEx: loopback destinations stay on the loopback, everything else leaves through Ethernet
                    // when the network is live; offline everything stays on the loopback, the only interface.
                    std::array<uint8_t, 16> destination{};
                    if (key.address && key.size >= 4 + destination.size())
                    {
                        win_emu.memory.read_memory(key.address + 4, destination.data(), destination.size());
                    }
                    constexpr std::array<uint8_t, 16> ipv6_loopback = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
                    const bool loopback = module == nsi_module::ipv4 ? destination[0] == 127 : destination == ipv6_loopback;
                    const auto interfaces = visible_interfaces(win_emu);
                    parameters.resize(offset + sizeof(uint32_t));
                    put(parameters, offset, interfaces[loopback ? 0 : interfaces.size() - 1].index);
                }
                else if (module == nsi_module::ndis && object == k_ndis_index_luid && type == nsi_parameter_type::static_parameters)
                {
                    const auto index = read_key<uint32_t>(win_emu, key);
                    const auto* iface = index ? find_interface_by_index(win_emu, *index) : nullptr;
                    if (!iface)
                    {
                        return index ? STATUS_NOT_FOUND : STATUS_INVALID_PARAMETER;
                    }
                    parameters.resize(sizeof(iface->luid));
                    put(parameters, 0, iface->luid);
                }
                else if (module == nsi_module::ndis && (object == k_ndis_interface_info || object == k_ndis_interface_enum))
                {
                    const auto luid = read_key<uint64_t>(win_emu, key);
                    const auto* iface = luid ? find_interface_by_luid(win_emu, *luid) : nullptr;
                    if (!iface)
                    {
                        return luid ? STATUS_NOT_FOUND : STATUS_INVALID_PARAMETER;
                    }
                    parameters = interface_parameters(*iface, object, type);
                }
                else
                {
                    return std::nullopt;
                }

                if (offset > parameters.size() || output.size > parameters.size() - offset)
                {
                    return STATUS_INVALID_PARAMETER;
                }

                win_emu.emu().write_memory(output.address, parameters.data() + offset, static_cast<size_t>(output.size));
                return STATUS_SUCCESS;
            }

            // GetParameter names one parameter: after the key comes the parameter type, then the output buffer, whose
            // ULONG size is followed by the parameter's offset inside its structure.
            static NTSTATUS get_parameter(windows_emulator& win_emu, const io_device_context& c, const bool wow64)
            {
                const ULONG data_offset = wow64 ? 0x24 : 0x40;
                const auto data = read_buffer(win_emu, c, data_offset, wow64);
                const nsi_buffer output{.address = data.address ? data.address : c.output_buffer,
                                        .size = data.address ? data.size : c.output_buffer_length};

                if (output.size == sizeof(uint32_t))
                {
                    if (output.address)
                    {
                        win_emu.emu().write_memory<uint32_t>(output.address, 1);
                    }
                }
                else if (output.size == sizeof(uint64_t))
                {
                    if (output.address)
                    {
                        win_emu.emu().write_memory<uint64_t>(output.address, k_adapter_index);
                    }
                }
                else
                {
                    write_adapter_parameter(win_emu, output);
                }

                write_io_information(c, 0);
                return STATUS_SUCCESS;
            }

            static NTSTATUS enumerate_objects_all_parameters(windows_emulator& win_emu, const io_device_context& c, const bool wow64)
            {
                // Report a single enumerated object by writing the count field that follows the buffer descriptors.
                const ULONG count_offset = wow64 ? 0x38 : 0x68;
                if (c.input_buffer_length >= count_offset + sizeof(ULONG))
                {
                    win_emu.emu().write_memory<ULONG>(c.input_buffer + count_offset, 1);
                }

                write_adapter_object(win_emu, read_all_parameters(win_emu, c, wow64));
                write_io_information(c, 0);
                return STATUS_SUCCESS;
            }
        };
    }

    std::unique_ptr<io_device> create_network_store_interface(const device_creation_context&)
    {
        return std::make_unique<network_store_interface_device>();
    }
}
