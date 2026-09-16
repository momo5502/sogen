#include "../std_include.hpp"
#include "mig_routines_iokit.hpp"

#include "../macos_emulator.hpp"
#include "io_surface_user_client.hpp"

#include <address_utils.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace sogen::mach
{
    namespace
    {
        // sys_iokit | sub_iokit_common. Wire shapes and the code each miss path returns are measured in
        namespace io_return
        {
            constexpr kern_return_t no_resources = static_cast<kern_return_t>(0xE00002BEu);
            constexpr kern_return_t ipc_error = static_cast<kern_return_t>(0xE00002BFu);
            constexpr kern_return_t no_device = static_cast<kern_return_t>(0xE00002C0u);
            constexpr kern_return_t bad_argument = static_cast<kern_return_t>(0xE00002C2u);
            constexpr kern_return_t unsupported = static_cast<kern_return_t>(0xE00002C7u);
            constexpr kern_return_t not_found = static_cast<kern_return_t>(0xE00002F0u);
        }

        namespace os_serialize
        {
            constexpr uint32_t signature = 0x000000D3u;
            constexpr uint32_t dictionary = 0x01000000u;
            constexpr uint32_t symbol = 0x08000000u;
            constexpr uint32_t string = 0x09000000u;
            constexpr uint32_t data = 0x0A000000u;
            constexpr uint32_t end_collection = 0x80000000u;
            constexpr uint32_t type_mask = 0x7F000000u;
            constexpr uint32_t length_mask = 0x00FFFFFFu;
        }

        // The version IOMainPort caches from routine 2877 and then uses to choose between the _bin
        // routine family and the XML one. Measured on 25G76; refusing this call is what pushed every
        // guest onto the legacy family the kernel itself answers kIOReturnUnsupported.
        constexpr uint64_t IOKIT_SERVER_VERSION = 0x0134647Aull;

        constexpr std::string_view IOKIT_ITERATOR_CLASS = "IOUserIterator";
        // io_scalar_inband64_t is array[*:16] of uint64 on both the request and the reply side.
        constexpr uint32_t MAX_CONNECT_SCALARS = 16;

        constexpr std::string_view IO_SURFACE_ROOT_CLASS = "IOSurfaceRoot";
        constexpr std::string_view IO_SURFACE_USER_CLIENT_CLASS = "IOSurfaceRootUserClient";

        // An io_object port carries its identity in the port entry's kernel_object id: either a registry
        // node's entry id, or an iterator, which is the tag below plus a bitmap of the nodes it has yet
        // to hand out. Keeping the iterator's position in the port entry rather than in a side table is
        // what makes it survive a snapshot, since port entries are serialised and nothing else here is.
        constexpr uint64_t IOKIT_ITERATOR_TAG = 0x8000000000000000ull;

        bool is_iterator(const kernel_object& object)
        {
            return object.kind == kernel_object_kind::io_object && (object.id & IOKIT_ITERATOR_TAG) != 0;
        }

        bool is_user_client(const kernel_object& object)
        {
            return object.kind == kernel_object_kind::io_object && (object.id & IOKIT_CONNECT_TAG) != 0;
        }

        enum class property_kind : uint8_t
        {
            data,
            string,
        };

        struct registry_property
        {
            std::string key{};
            property_kind kind{};
            std::string value{};
        };

        constexpr size_t NO_PARENT = static_cast<size_t>(-1);

        struct plane_placement
        {
            std::string plane{};
            std::string name{};
            size_t parent{NO_PARENT};
        };

        struct registry_node
        {
            uint64_t entry_id{};
            std::string class_name{};
            std::vector<std::string> class_chain{};
            std::vector<plane_placement> placements{};
            std::vector<registry_property> properties{};
        };

        std::string padded_bytes(const std::string_view text, const size_t width)
        {
            std::string value(width, '\0');
            std::ranges::copy(text.substr(0, width), value.begin());
            return value;
        }

        std::string terminated_bytes(const std::string_view text)
        {
            return std::string{text} + '\0';
        }

        // The IOPlatformExpertDevice property set measured with ioreg and IORegistryEntryCreateCFProperty
        // on an arm64 host, with the same key names, the same OSData-vs-OSString split and the same byte
        // lengths. The per-unit values -- IOPlatformSerialNumber, IOPlatformUUID, serial-number,
        // region-info -- are synthetic: reproducing the measuring machine's would hand every guest the
        // analyst's hardware identity. board-id is absent on purpose: on Apple Silicon it is four bytes
        // of OSData on IODeviceTree:/chosen, not the Intel-era string on this node, and putting the Intel
        // shape here would tell a guest it is on an Intel Mac while every other property says arm64.
        std::vector<registry_property> platform_expert_properties()
        {
            return {
                {"model", property_kind::data, terminated_bytes("Mac15,11")},
                {"manufacturer", property_kind::data, terminated_bytes("Apple Inc.")},
                {"target-type", property_kind::data, terminated_bytes("J516m")},
                {"target-sub-type", property_kind::data, terminated_bytes("J516mAP")},
                {"compatible", property_kind::data,
                 terminated_bytes("J516mAP") + terminated_bytes("Mac15,11") + terminated_bytes("AppleARM")},
                {"name", property_kind::data, terminated_bytes("device-tree")},
                {"device_type", property_kind::data, terminated_bytes("bootrom")},
                {"IOPlatformSerialNumber", property_kind::string, "SOGEN00001"},
                {"IOPlatformUUID", property_kind::string, "00000000-0000-4000-8000-000000000001"},
                {"serial-number", property_kind::data, padded_bytes("SOGEN00001", 32)},
                {"platform-name", property_kind::data, padded_bytes("t6034", 32)},
                {"region-info", property_kind::data, padded_bytes("ZZ/A", 32)},
                {"clock-frequency", property_kind::data, std::string("\x00\x36\x6e\x01", 4)},
                {"#address-cells", property_kind::data, std::string("\x02\x00\x00\x00", 4)},
                {"#size-cells", property_kind::data, std::string("\x02\x00\x00\x00", 4)},
                {"AAPL,phandle", property_kind::data, std::string("\x01\x00\x00\x00", 4)},
            };
        }

        // IODeviceTree:/product and IODeviceTree:/chosen are the two nodes libMobileGestalt opens by path
        // on every launch, measured under sogen. Everything here is a product-level fact of a Mac15,11
        // read off `ioreg -p IODeviceTree`; the per-unit unique-chip-id is synthetic. The real nodes carry
        // far more -- panic-log geometry, boot manifests, NVRAM images, panel serial numbers -- that is
        // either per-unit or has no meaning in an emulator, and a guest asking for it gets the miss code a
        // registry without it returns. board-id lives HERE, four bytes of OSData on chosen, not on the
        // platform expert as it did on Intel.
        std::vector<registry_property> product_properties()
        {
            return {
                {"name", property_kind::data, terminated_bytes("product")},
                {"product-name", property_kind::data, terminated_bytes("MacBook Pro (16-inch, Nov 2023)")},
                {"product-description", property_kind::data, terminated_bytes("MacBook Pro (16-inch, Nov 2023)")},
                {"product-soc-name", property_kind::data, terminated_bytes("Apple M3 Max")},
                {"unique-model", property_kind::data, terminated_bytes("J516mAP")},
            };
        }

        std::vector<registry_property> chosen_properties()
        {
            return {
                {"name", property_kind::data, terminated_bytes("chosen")},
                {"board-id", property_kind::data, std::string("\x46\x00\x00\x00", 4)},
                {"chip-id", property_kind::data, std::string("\x34\x60\x00\x00", 4)},
                {"unique-chip-id", property_kind::data, std::string("\x01\x00\x00\x00\x00\x00\x00\x00", 8)},
                {"#address-cells", property_kind::data, std::string("\x02\x00\x00\x00", 4)},
            };
        }

        // Root is above every plane -- IORegistryEntryInPlane(root, "IOService") is 0 on the host -- and
        // the platform expert is the plane root of both planes, which is why IORegistryEntryFromPath of
        // "IOService:/" and of "IODeviceTree:/" both land on it.
        const std::vector<registry_node>& registry()
        {
            static const std::vector<registry_node> nodes = [] {
                std::vector<registry_node> built{};

                built.push_back({
                    .entry_id = 0x100000100ull,
                    .class_name = "IORegistryEntry",
                    .class_chain = {"IORegistryEntry"},
                    .placements = {},
                    .properties = {},
                });

                built.push_back({
                    .entry_id = 0x100000200ull,
                    .class_name = "IOPlatformExpertDevice",
                    .class_chain = {"IOPlatformExpertDevice", "IOService", "IORegistryEntry"},
                    .placements = {{.plane = "IOService", .name = "J516mAP", .parent = NO_PARENT},
                                   {.plane = "IODeviceTree", .name = "device-tree", .parent = NO_PARENT}},
                    .properties = platform_expert_properties(),
                });

                built.push_back({
                    .entry_id = 0x100000300ull,
                    .class_name = "IOPlatformDevice",
                    .class_chain = {"IOPlatformDevice", "IOService", "IORegistryEntry"},
                    .placements = {{.plane = "IODeviceTree", .name = "product", .parent = 1}},
                    .properties = product_properties(),
                });

                built.push_back({
                    .entry_id = 0x100000400ull,
                    .class_name = "IOService",
                    .class_chain = {"IOService", "IORegistryEntry"},
                    .placements = {{.plane = "IODeviceTree", .name = "chosen", .parent = 1}},
                    .properties = chosen_properties(),
                });

                // The service every IOSurface client opens before it will answer anything.
                // _iosConnectInitalize reaches it with IOServiceNameMatching("IOSurfaceRoot") and calls
                // abort() -- "unable to locate IOSurface kernel service" -- when the match finds nothing,
                // so a registry without this node stops any process that touches a CoreAnimation backing
                // store. It carries no properties: the real node's are accelerator inventory sogen has no
                // honest value for, and a client asking for one gets the miss code instead.
                built.push_back({
                    .entry_id = 0x100000500ull,
                    .class_name = std::string{IO_SURFACE_ROOT_CLASS},
                    .class_chain = {std::string{IO_SURFACE_ROOT_CLASS}, "IOService", "IORegistryEntry"},
                    .placements = {{.plane = "IOService", .name = std::string{IO_SURFACE_ROOT_CLASS}, .parent = 1}},
                    .properties = {},
                });

                return built;
            }();

            return nodes;
        }

        static_assert(sizeof(uint64_t) * 8 > 4, "an iterator bitmap holds one bit per registry node");

        std::optional<size_t> node_index_for_entry_id(const uint64_t id)
        {
            const auto& nodes = registry();
            for (size_t i = 0; i < nodes.size(); ++i)
            {
                if (nodes[i].entry_id == id)
                {
                    return i;
                }
            }

            return std::nullopt;
        }

        const plane_placement* placement_of(const registry_node& node, const std::string_view plane)
        {
            for (const auto& placement : node.placements)
            {
                if (placement.plane == plane)
                {
                    return &placement;
                }
            }

            return nullptr;
        }

        const registry_property* property_of(const registry_node& node, const std::string_view key)
        {
            for (const auto& property : node.properties)
            {
                if (property.key == key)
                {
                    return &property;
                }
            }

            return nullptr;
        }

        std::optional<std::string> path_of(const registry_node& node, const std::string_view plane)
        {
            const auto* placement = placement_of(node, plane);
            if (placement == nullptr)
            {
                return std::nullopt;
            }

            std::string tail{};
            for (const auto* walk = placement; walk->parent != NO_PARENT;)
            {
                tail = walk->name + (tail.empty() ? std::string{} : "/" + tail);
                walk = placement_of(registry().at(walk->parent), plane);
                if (walk == nullptr)
                {
                    return std::nullopt;
                }
            }

            return std::string{plane} + ":/" + tail;
        }

        std::optional<size_t> entry_for_path(const std::string_view path)
        {
            const auto separator = path.find(":/");
            if (separator == std::string_view::npos)
            {
                return std::nullopt;
            }

            const auto plane = path.substr(0, separator);
            auto tail = path.substr(separator + 2);

            std::optional<size_t> current{};
            const auto& nodes = registry();

            for (size_t i = 0; i < nodes.size(); ++i)
            {
                const auto* placement = placement_of(nodes[i], plane);
                if (placement != nullptr && placement->parent == NO_PARENT)
                {
                    current = i;
                    break;
                }
            }

            while (!tail.empty() && current.has_value())
            {
                const auto slash = tail.find('/');
                const auto step = tail.substr(0, slash);
                tail = slash == std::string_view::npos ? std::string_view{} : tail.substr(slash + 1);

                if (step.empty())
                {
                    continue;
                }

                std::optional<size_t> next{};
                for (size_t i = 0; i < nodes.size(); ++i)
                {
                    const auto* placement = placement_of(nodes[i], plane);
                    if (placement != nullptr && placement->parent == *current && placement->name == step)
                    {
                        next = i;
                        break;
                    }
                }

                current = next;
            }

            return current;
        }

        void append_u32le(std::vector<uint8_t>& out, const uint32_t value)
        {
            out.push_back(static_cast<uint8_t>(value & 0xFFu));
            out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
            out.push_back(static_cast<uint8_t>((value >> 16) & 0xFFu));
            out.push_back(static_cast<uint8_t>((value >> 24) & 0xFFu));
        }

        void append_token(std::vector<uint8_t>& out, const uint32_t type, const size_t length, const bool last)
        {
            append_u32le(out,
                         type | (last ? os_serialize::end_collection : 0u) | (static_cast<uint32_t>(length) & os_serialize::length_mask));
        }

        void append_padded(std::vector<uint8_t>& out, const std::string_view bytes)
        {
            out.insert(out.end(), bytes.begin(), bytes.end());
            out.resize(out.size() + ((4 - (bytes.size() % 4)) % 4), 0);
        }

        // A symbol's length counts the NUL and a string's does not; a data's is its byte count. The three
        // disagree and a decoder that gets one wrong reads the next token out of the payload.
        void append_symbol(std::vector<uint8_t>& out, const std::string_view key)
        {
            append_token(out, os_serialize::symbol, key.size() + 1, false);
            append_padded(out, std::string{key} + '\0');
        }

        void append_property_value(std::vector<uint8_t>& out, const registry_property& property, const bool last)
        {
            const auto type = property.kind == property_kind::string ? os_serialize::string : os_serialize::data;
            append_token(out, type, property.value.size(), last);
            append_padded(out, property.value);
        }

        std::vector<uint8_t> serialize_property(const registry_property& property)
        {
            std::vector<uint8_t> out{};
            append_u32le(out, os_serialize::signature);
            append_property_value(out, property, true);
            return out;
        }

        std::vector<uint8_t> serialize_properties(const registry_node& node)
        {
            std::vector<uint8_t> out{};
            append_u32le(out, os_serialize::signature);
            append_token(out, os_serialize::dictionary, node.properties.size(), true);

            for (size_t i = 0; i < node.properties.size(); ++i)
            {
                append_symbol(out, node.properties[i].key);
                append_property_value(out, node.properties[i], i + 1 == node.properties.size());
            }

            return out;
        }

        std::string base64(const std::string_view bytes)
        {
            static constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

            std::string out{};
            for (size_t i = 0; i < bytes.size(); i += 3)
            {
                const auto remaining = bytes.size() - i;
                const auto b0 = static_cast<uint8_t>(bytes[i]);
                const auto b1 = remaining > 1 ? static_cast<uint8_t>(bytes[i + 1]) : uint8_t{};
                const auto b2 = remaining > 2 ? static_cast<uint8_t>(bytes[i + 2]) : uint8_t{};

                out += alphabet[b0 >> 2];
                out += alphabet[((b0 & 0x03u) << 4) | (b1 >> 4)];
                out += remaining > 1 ? alphabet[((b1 & 0x0Fu) << 2) | (b2 >> 6)] : '=';
                out += remaining > 2 ? alphabet[b2 & 0x3Fu] : '=';
            }

            return out;
        }

        std::string xml_escaped(const std::string_view text)
        {
            std::string out{};
            for (const auto character : text)
            {
                switch (character)
                {
                case '&':
                    out += "&amp;";
                    break;
                case '<':
                    out += "&lt;";
                    break;
                case '>':
                    out += "&gt;";
                    break;
                default:
                    out += character;
                    break;
                }
            }

            return out;
        }

        // Routine 2805's payload is a bare, NUL-terminated XML fragment with no plist header, and the
        // ID attribute is part of the measured bytes: <data ID="0">TWFjMTUsMTEA</data>.
        std::vector<uint8_t> serialize_property_xml(const registry_property& property)
        {
            const auto text = property.kind == property_kind::string ? "<string ID=\"0\">" + xml_escaped(property.value) + "</string>"
                                                                     : "<data ID=\"0\">" + base64(property.value) + "</data>";

            std::vector<uint8_t> out(text.begin(), text.end());
            out.push_back(0);
            return out;
        }

        struct matching_criteria
        {
            bool parsed{};
            std::vector<std::pair<std::string, std::string>> entries{};
        };

        std::optional<std::string> read_serialized_text(const std::span<const uint8_t> bytes, size_t& offset, const uint32_t expected_type,
                                                        bool& last)
        {
            if (offset + sizeof(uint32_t) > bytes.size())
            {
                return std::nullopt;
            }

            const auto token = read_u32(bytes, offset);
            offset += sizeof(uint32_t);

            if ((token & os_serialize::type_mask) != expected_type)
            {
                return std::nullopt;
            }

            last = (token & os_serialize::end_collection) != 0;

            auto length = static_cast<size_t>(token & os_serialize::length_mask);
            const auto padded = length + ((4 - (length % 4)) % 4);
            if (offset + padded > bytes.size())
            {
                return std::nullopt;
            }

            if (expected_type == os_serialize::symbol && length > 0)
            {
                --length;
            }

            std::string text(reinterpret_cast<const char*>(bytes.data()) + offset, length);
            offset += padded;
            return text;
        }

        matching_criteria parse_matching(const std::span<const uint8_t> bytes)
        {
            matching_criteria criteria{};

            if (bytes.size() < 8 || read_u32(bytes, 0) != os_serialize::signature)
            {
                return criteria;
            }

            const auto header = read_u32(bytes, 4);
            if ((header & os_serialize::type_mask) != os_serialize::dictionary)
            {
                return criteria;
            }

            const auto count = header & os_serialize::length_mask;
            size_t offset = 8;

            for (uint32_t i = 0; i < count; ++i)
            {
                bool last = false;
                const auto key = read_serialized_text(bytes, offset, os_serialize::symbol, last);
                if (!key.has_value())
                {
                    return {};
                }

                const auto value = read_serialized_text(bytes, offset, os_serialize::string, last);
                if (!value.has_value())
                {
                    return {};
                }

                criteria.entries.emplace_back(*key, *value);
            }

            criteria.parsed = true;
            return criteria;
        }

        bool node_matches(macos_emulator& emu, const registry_node& node, const matching_criteria& criteria)
        {
            if (!criteria.parsed || criteria.entries.empty())
            {
                return false;
            }

            for (const auto& [key, value] : criteria.entries)
            {
                if (key == "IOProviderClass" || key == "IOClass")
                {
                    if (std::ranges::find(node.class_chain, value) == node.class_chain.end())
                    {
                        return false;
                    }

                    continue;
                }

                if (key == "IONameMatch")
                {
                    const auto named =
                        std::ranges::any_of(node.placements, [&](const plane_placement& placement) { return placement.name == value; });
                    if (!named)
                    {
                        return false;
                    }

                    continue;
                }

                static std::set<std::string> reported{};
                if (reported.insert(key).second)
                {
                    emu.log.warn("IOKit service matching on \"%s\" is not modelled; nothing in sogen's registry matches it\n", key.c_str());
                }

                return false;
            }

            return true;
        }

        std::span<const uint8_t> arguments_of(const mig_request& request)
        {
            const auto offset = request.effective_args_offset();
            return offset <= request.body.size() ? request.body.subspan(offset) : std::span<const uint8_t>{};
        }

        struct inline_string
        {
            bool valid{};
            std::string text{};
            size_t next{};
        };

        // MIG's variable c_string: a zero offset, a count that includes the NUL, then the characters
        // padded up to a 4-byte boundary.
        inline_string read_inline_string(const std::span<const uint8_t> arguments, const size_t offset)
        {
            if (offset + 2 * sizeof(uint32_t) > arguments.size())
            {
                return {};
            }

            const auto count = read_u32(arguments, offset + sizeof(uint32_t));
            const auto start = offset + 2 * sizeof(uint32_t);
            const auto padded = count + ((4 - (count % 4)) % 4);

            if (count == 0 || start + padded > arguments.size())
            {
                return {};
            }

            std::string text(reinterpret_cast<const char*>(arguments.data()) + start, count - 1);
            return {.valid = true, .text = std::move(text), .next = start + padded};
        }

        port_name_t mint_io_object(macos_emulator& emu, const uint64_t id)
        {
            return emu.mach.ports.allocate_receive_right({.kind = kernel_object_kind::io_object, .id = id});
        }

        std::vector<uint8_t> port_reply(macos_emulator& emu, const mig_request& request, const port_name_t name)
        {
            mig_reply_builder builder{request.call, emu.mach.ports};

            if (name == PORT_NULL)
            {
                builder.set_complex();
                std::array<uint8_t, PORT_DESCRIPTOR_SIZE> scratch{};
                write_port_descriptor(
                    scratch,
                    {.name = PORT_NULL, .disposition = received_disposition(disposition::make_send), .type = descriptor_type::port});
                builder.append_bytes(scratch);
                auto reply = builder.finish();
                write_u32(reply, MSG_HEADER_SIZE, 1);
                return reply;
            }

            builder.append_port_descriptor({.name = name, .disposition = disposition::make_send, .type = descriptor_type::port});
            return builder.finish();
        }

        std::vector<uint8_t> counted_string_reply(macos_emulator& emu, const mig_request& request, const std::string_view text)
        {
            mig_reply_builder builder{request.call, emu.mach.ports};
            builder.append_ndr();
            builder.append_u32(static_cast<uint32_t>(kr::success));
            builder.append_u32(0);
            builder.append_u32(static_cast<uint32_t>(text.size() + 1));

            std::vector<uint8_t> padded{};
            append_padded(padded, std::string{text} + '\0');
            builder.append_bytes(padded);

            return builder.finish();
        }

        std::vector<uint8_t> scalar_reply(macos_emulator& emu, const mig_request& request, const uint32_t value)
        {
            mig_reply_builder builder{request.call, emu.mach.ports};
            builder.append_ndr();
            builder.append_u32(static_cast<uint32_t>(kr::success));
            builder.append_u32(value);
            return builder.finish();
        }

        struct guest_allocation
        {
            uint64_t address{};
            size_t reserved{};
        };

        std::optional<guest_allocation> place_out_of_line(macos_emulator& emu, const std::span<const uint8_t> payload)
        {
            if (payload.empty())
            {
                return guest_allocation{};
            }

            const auto reserved = static_cast<size_t>(page_align_up(payload.size(), MACOS_PAGE_SIZE));
            const auto address = emu.memory.allocate_memory(reserved, memory_permission::read_write, MACOS_DEFAULT_MMAP_BASE);

            if (address == 0 || !emu.memory.try_write_memory(address, payload.data(), payload.size()))
            {
                if (address != 0)
                {
                    emu.memory.release_memory(address, reserved);
                }

                return std::nullopt;
            }

            return guest_allocation{.address = address, .reserved = reserved};
        }

        // mig_reply_builder only knows about port descriptors, so the out-of-line replies are assembled
        // here, in the same order the kernel sends them: body, descriptor, NDR, then the scalars.
        std::vector<uint8_t> ool_reply(const mig_request& request, const uint64_t address, const uint32_t size,
                                       const std::span<const uint8_t> scalars)
        {
            std::vector<uint8_t> reply(MSG_HEADER_SIZE + MSG_BODY_SIZE + OOL_DESCRIPTOR_SIZE + NDR_RECORD_SIZE + scalars.size(), 0);

            write_msg_header(reply, {
                                        .bits = reply_bits_for(request.call.header.bits, true),
                                        .size = static_cast<uint32_t>(reply.size()),
                                        .remote_port = PORT_NULL,
                                        .local_port = request.call.header.local_port,
                                        .voucher_port = PORT_NULL,
                                        .id = request.call.header.id + subsystem::reply_offset,
                                    });

            size_t offset = MSG_HEADER_SIZE;
            write_u32(reply, offset, 1);
            offset += MSG_BODY_SIZE;

            write_ool_descriptor(std::span{reply}.subspan(offset), {
                                                                       .address = address,
                                                                       .size = size,
                                                                       .deallocate = 0,
                                                                       .copy = copy_option::physical,
                                                                       .type = descriptor_type::ool,
                                                                   });
            offset += OOL_DESCRIPTOR_SIZE;

            std::ranges::copy(NDR_RECORD, reply.begin() + static_cast<ptrdiff_t>(offset));
            offset += NDR_RECORD_SIZE;

            std::ranges::copy(scalars, reply.begin() + static_cast<ptrdiff_t>(offset));
            return reply;
        }

        std::vector<uint8_t> serialized_ool_reply(macos_emulator& emu, const mig_request& request, const std::span<const uint8_t> payload)
        {
            const auto placed = place_out_of_line(emu, payload);
            if (!placed.has_value())
            {
                return make_mig_error_bytes(request, kr::resource_shortage);
            }

            std::array<uint8_t, sizeof(uint32_t)> scalars{};
            write_u32(scalars, 0, static_cast<uint32_t>(payload.size()));
            return ool_reply(request, placed->address, static_cast<uint32_t>(payload.size()), scalars);
        }

        // The _buf routines hand the kernel a buffer of the caller's own and take the answer inband when
        // it fits; only the overflow goes out of line. Both halves are one reply shape with the unused
        // half zeroed, which is why the count the caller reads is not always the same field.
        std::vector<uint8_t> serialized_buf_reply(macos_emulator& emu, const mig_request& request, const std::span<const uint8_t> payload,
                                                  const uint64_t buffer, const uint64_t buffer_size)
        {
            std::array<uint8_t, sizeof(uint64_t) + sizeof(uint32_t)> scalars{};

            if (buffer != 0 && payload.size() <= buffer_size && emu.memory.try_write_memory(buffer, payload.data(), payload.size()))
            {
                write_u64(scalars, 0, payload.size());
                return ool_reply(request, 0, 0, scalars);
            }

            const auto placed = place_out_of_line(emu, payload);
            if (!placed.has_value())
            {
                return make_mig_error_bytes(request, kr::resource_shortage);
            }

            write_u32(scalars, sizeof(uint64_t), static_cast<uint32_t>(payload.size()));
            return ool_reply(request, placed->address, static_cast<uint32_t>(payload.size()), scalars);
        }

        const registry_node* node_of_request(macos_emulator& emu, const mig_request& request)
        {
            const auto object = emu.mach.ports.object_of(request.call.header.remote_port);
            if (object.kind != kernel_object_kind::io_object)
            {
                return nullptr;
            }

            const auto index = node_index_for_entry_id(object.id);
            return index.has_value() ? &registry().at(*index) : nullptr;
        }

        std::string describe_criteria(const matching_criteria& criteria)
        {
            if (!criteria.parsed)
            {
                return "an unparseable matching dictionary";
            }

            std::string described{};
            for (const auto& [key, value] : criteria.entries)
            {
                described += (described.empty() ? "" : ", ") + key + "=\"" + value + "\"";
            }

            return described;
        }

        void report_no_match_once(macos_emulator& emu, const matching_criteria& criteria)
        {
            const auto described = describe_criteria(criteria);

            static std::set<std::string> reported{};
            if (reported.insert(described).second)
            {
                emu.log.warn("IOKit has no service matching %s; sogen's registry models the platform expert and nothing else\n",
                             described.c_str());
            }
        }

        void report_missing_property_once(macos_emulator& emu, const registry_node& node, const std::string& key)
        {
            static std::set<std::string> reported{};
            if (reported.insert(node.class_name + "." + key).second)
            {
                emu.log.warn("IOKit property \"%s\" is not modelled on %s\n", key.c_str(), node.class_name.c_str());
            }
        }

        void report_unsupported_once(macos_emulator& emu, const std::string_view name)
        {
            static std::set<std::string> reported{};
            if (reported.emplace(name).second)
            {
                emu.log.warn("IOKit routine %.*s is answered kIOReturnUnsupported, which is what the host kernel answers; a guest "
                             "that gets here has not seen io_server_version\n",
                             static_cast<int>(name.size()), name.data());
            }
        }

        std::vector<uint8_t> server_version_routine(macos_emulator& emu, const mig_request& request)
        {
            mig_reply_builder builder{request.call, emu.mach.ports};
            builder.append_ndr();
            builder.append_u32(static_cast<uint32_t>(kr::success));
            builder.append_u64(IOKIT_SERVER_VERSION);
            return builder.finish();
        }

        std::vector<uint8_t> registry_entry_from_path_routine(macos_emulator& emu, const mig_request& request)
        {
            const auto path = read_inline_string(arguments_of(request), 0);
            if (!path.valid)
            {
                return make_mig_error_bytes(request, mig_error::bad_arguments);
            }

            const auto index = entry_for_path(path.text);
            if (!index.has_value())
            {
                static std::set<std::string> reported{};
                if (reported.insert(path.text).second)
                {
                    emu.log.warn("IOKit registry path \"%s\" names nothing in sogen's registry\n", path.text.c_str());
                }

                return port_reply(emu, request, PORT_NULL);
            }

            return port_reply(emu, request, mint_io_object(emu, registry().at(*index).entry_id));
        }

        std::vector<uint8_t> registry_get_root_entry_routine(macos_emulator& emu, const mig_request& request)
        {
            return port_reply(emu, request, mint_io_object(emu, registry().front().entry_id));
        }

        std::vector<uint8_t> matching_service_bin_routine(macos_emulator& emu, const mig_request& request)
        {
            const auto arguments = arguments_of(request);
            const auto count = read_u32(arguments, 0);
            if (sizeof(uint32_t) + count > arguments.size())
            {
                return make_mig_error_bytes(request, mig_error::bad_arguments);
            }

            const auto criteria = parse_matching(arguments.subspan(sizeof(uint32_t), count));
            const auto& nodes = registry();

            for (const auto& node : nodes)
            {
                if (node_matches(emu, node, criteria))
                {
                    return port_reply(emu, request, mint_io_object(emu, node.entry_id));
                }
            }

            report_no_match_once(emu, criteria);
            return make_mig_error_bytes(request, io_return::not_found);
        }

        std::vector<uint8_t> matching_services_bin_routine(macos_emulator& emu, const mig_request& request)
        {
            const auto arguments = arguments_of(request);
            const auto count = read_u32(arguments, 0);
            if (sizeof(uint32_t) + count > arguments.size())
            {
                return make_mig_error_bytes(request, mig_error::bad_arguments);
            }

            const auto criteria = parse_matching(arguments.subspan(sizeof(uint32_t), count));
            const auto& nodes = registry();

            uint64_t remaining = 0;
            for (size_t i = 0; i < nodes.size(); ++i)
            {
                if (node_matches(emu, nodes[i], criteria))
                {
                    remaining |= uint64_t{1} << i;
                }
            }

            if (remaining == 0)
            {
                report_no_match_once(emu, criteria);
                return port_reply(emu, request, PORT_NULL);
            }

            return port_reply(emu, request, mint_io_object(emu, IOKIT_ITERATOR_TAG | remaining));
        }

        std::vector<uint8_t> matching_service_xml_routine(macos_emulator& emu, const mig_request& request)
        {
            report_unsupported_once(emu, "io_service_get_matching_service");
            return make_mig_error_bytes(request, io_return::unsupported);
        }

        std::vector<uint8_t> matching_services_xml_routine(macos_emulator& emu, const mig_request& request)
        {
            report_unsupported_once(emu, "io_service_get_matching_services");
            return make_mig_error_bytes(request, io_return::unsupported);
        }

        std::vector<uint8_t> iterator_next_routine(macos_emulator& emu, const mig_request& request)
        {
            auto* entry = emu.mach.ports.find(request.call.header.remote_port);
            if (entry == nullptr || !is_iterator(entry->object))
            {
                return make_mig_error_bytes(request, io_return::bad_argument);
            }

            const auto remaining = entry->object.id & ~IOKIT_ITERATOR_TAG;
            if (remaining == 0)
            {
                return make_mig_error_bytes(request, io_return::no_device);
            }

            const auto index = static_cast<size_t>(std::countr_zero(remaining));
            entry->object.id = IOKIT_ITERATOR_TAG | (remaining & ~(uint64_t{1} << index));

            return port_reply(emu, request, mint_io_object(emu, registry().at(index).entry_id));
        }

        std::vector<uint8_t> object_get_class_routine(macos_emulator& emu, const mig_request& request)
        {
            const auto object = emu.mach.ports.object_of(request.call.header.remote_port);
            if (is_iterator(object))
            {
                return counted_string_reply(emu, request, IOKIT_ITERATOR_CLASS);
            }

            if (is_user_client(object))
            {
                return counted_string_reply(emu, request, IO_SURFACE_USER_CLIENT_CLASS);
            }

            const auto* node = node_of_request(emu, request);
            if (node == nullptr)
            {
                return make_mig_error_bytes(request, io_return::bad_argument);
            }

            return counted_string_reply(emu, request, node->class_name);
        }

        std::vector<uint8_t> object_conforms_to_routine(macos_emulator& emu, const mig_request& request)
        {
            const auto wanted = read_inline_string(arguments_of(request), 0);
            if (!wanted.valid)
            {
                return make_mig_error_bytes(request, mig_error::bad_arguments);
            }

            const auto* node = node_of_request(emu, request);
            if (node == nullptr)
            {
                return make_mig_error_bytes(request, io_return::bad_argument);
            }

            const auto conforms = std::ranges::find(node->class_chain, wanted.text) != node->class_chain.end();
            return scalar_reply(emu, request, conforms ? 1u : 0u);
        }

        std::vector<uint8_t> object_get_retain_count_routine(macos_emulator& emu, const mig_request& request)
        {
            const auto* entry = emu.mach.ports.find(request.call.header.remote_port);
            if (entry == nullptr || entry->object.kind != kernel_object_kind::io_object)
            {
                return make_mig_error_bytes(request, io_return::bad_argument);
            }

            // sogen keeps no refcount of its own on a registry node, so the honest number is how many
            // send rights the guest is holding on this object right now.
            return scalar_reply(emu, request, std::max<uint32_t>(entry->send_urefs, 1));
        }

        std::vector<uint8_t> registry_entry_get_name_routine(macos_emulator& emu, const mig_request& request)
        {
            const auto* node = node_of_request(emu, request);
            if (node == nullptr)
            {
                return make_mig_error_bytes(request, io_return::bad_argument);
            }

            if (node->placements.empty())
            {
                return counted_string_reply(emu, request, "Root");
            }

            return counted_string_reply(emu, request, node->placements.front().name);
        }

        std::vector<uint8_t> registry_entry_get_name_in_plane_routine(macos_emulator& emu, const mig_request& request)
        {
            const auto plane = read_inline_string(arguments_of(request), 0);
            if (!plane.valid)
            {
                return make_mig_error_bytes(request, mig_error::bad_arguments);
            }

            const auto* node = node_of_request(emu, request);
            if (node == nullptr)
            {
                return make_mig_error_bytes(request, io_return::bad_argument);
            }

            const auto* placement = placement_of(*node, plane.text);
            if (placement == nullptr)
            {
                return registry_entry_get_name_routine(emu, request);
            }

            return counted_string_reply(emu, request, placement->name);
        }

        std::vector<uint8_t> registry_entry_get_path_routine(macos_emulator& emu, const mig_request& request)
        {
            const auto plane = read_inline_string(arguments_of(request), 0);
            if (!plane.valid)
            {
                return make_mig_error_bytes(request, mig_error::bad_arguments);
            }

            const auto* node = node_of_request(emu, request);
            if (node == nullptr)
            {
                return make_mig_error_bytes(request, io_return::bad_argument);
            }

            const auto path = path_of(*node, plane.text);
            if (!path.has_value())
            {
                return make_mig_error_bytes(request, io_return::bad_argument);
            }

            return counted_string_reply(emu, request, *path);
        }

        std::vector<uint8_t> registry_entry_in_plane_routine(macos_emulator& emu, const mig_request& request)
        {
            const auto plane = read_inline_string(arguments_of(request), 0);
            if (!plane.valid)
            {
                return make_mig_error_bytes(request, mig_error::bad_arguments);
            }

            const auto* node = node_of_request(emu, request);
            if (node == nullptr)
            {
                return make_mig_error_bytes(request, io_return::bad_argument);
            }

            return scalar_reply(emu, request, placement_of(*node, plane.text) != nullptr ? 1u : 0u);
        }

        std::vector<uint8_t> registry_entry_get_registry_entry_id_routine(macos_emulator& emu, const mig_request& request)
        {
            const auto* node = node_of_request(emu, request);
            if (node == nullptr)
            {
                return make_mig_error_bytes(request, io_return::bad_argument);
            }

            mig_reply_builder builder{request.call, emu.mach.ports};
            builder.append_ndr();
            builder.append_u32(static_cast<uint32_t>(kr::success));
            builder.append_u64(node->entry_id);
            return builder.finish();
        }

        std::vector<uint8_t> registry_entry_get_property_bytes_routine(macos_emulator& emu, const mig_request& request)
        {
            const auto arguments = arguments_of(request);
            const auto key = read_inline_string(arguments, 0);
            if (!key.valid || key.next + sizeof(uint32_t) > arguments.size())
            {
                return make_mig_error_bytes(request, mig_error::bad_arguments);
            }

            const auto* node = node_of_request(emu, request);
            if (node == nullptr)
            {
                return make_mig_error_bytes(request, io_return::bad_argument);
            }

            const auto* property = property_of(*node, key.text);
            if (property == nullptr)
            {
                report_missing_property_once(emu, *node, key.text);
                return make_mig_error_bytes(request, io_return::no_resources);
            }

            // An OSString comes back with its terminator and an OSData verbatim: measured 11 bytes for a
            // ten-character serial number against 9 for "Mac15,11\0".
            const auto bytes = property->kind == property_kind::string ? property->value + '\0' : property->value;

            if (bytes.size() > read_u32(arguments, key.next))
            {
                return make_mig_error_bytes(request, io_return::ipc_error);
            }

            mig_reply_builder builder{request.call, emu.mach.ports};
            builder.append_ndr();
            builder.append_u32(static_cast<uint32_t>(kr::success));
            builder.append_u32(static_cast<uint32_t>(bytes.size()));

            std::vector<uint8_t> padded{};
            append_padded(padded, bytes);
            builder.append_bytes(padded);

            return builder.finish();
        }

        std::vector<uint8_t> registry_entry_get_property_xml_routine(macos_emulator& emu, const mig_request& request)
        {
            const auto key = read_inline_string(arguments_of(request), 0);
            if (!key.valid)
            {
                return make_mig_error_bytes(request, mig_error::bad_arguments);
            }

            const auto* node = node_of_request(emu, request);
            if (node == nullptr)
            {
                return make_mig_error_bytes(request, io_return::bad_argument);
            }

            const auto* property = property_of(*node, key.text);
            if (property == nullptr)
            {
                report_missing_property_once(emu, *node, key.text);
                return make_mig_error_bytes(request, io_return::not_found);
            }

            return serialized_ool_reply(emu, request, serialize_property_xml(*property));
        }

        std::vector<uint8_t> registry_entry_get_properties_xml_routine(macos_emulator& emu, const mig_request& request)
        {
            report_unsupported_once(emu, "io_registry_entry_get_properties");
            return make_mig_error_bytes(request, io_return::unsupported);
        }

        struct property_bin_request
        {
            bool valid{};
            std::string key{};
            size_t next{};
        };

        property_bin_request read_property_bin_request(const mig_request& request)
        {
            const auto arguments = arguments_of(request);

            const auto plane = read_inline_string(arguments, 0);
            if (!plane.valid)
            {
                return {};
            }

            const auto key = read_inline_string(arguments, plane.next);
            if (!key.valid || key.next + sizeof(uint32_t) > arguments.size())
            {
                return {};
            }

            return {.valid = true, .key = key.text, .next = key.next + sizeof(uint32_t)};
        }

        std::vector<uint8_t> registry_entry_get_property_bin_routine(macos_emulator& emu, const mig_request& request)
        {
            const auto parsed = read_property_bin_request(request);
            if (!parsed.valid)
            {
                return make_mig_error_bytes(request, mig_error::bad_arguments);
            }

            const auto* node = node_of_request(emu, request);
            if (node == nullptr)
            {
                return make_mig_error_bytes(request, io_return::bad_argument);
            }

            const auto* property = property_of(*node, parsed.key);
            if (property == nullptr)
            {
                report_missing_property_once(emu, *node, parsed.key);
                return make_mig_error_bytes(request, io_return::not_found);
            }

            return serialized_ool_reply(emu, request, serialize_property(*property));
        }

        std::vector<uint8_t> registry_entry_get_property_bin_buf_routine(macos_emulator& emu, const mig_request& request)
        {
            const auto arguments = arguments_of(request);
            const auto parsed = read_property_bin_request(request);
            if (!parsed.valid || parsed.next + 2 * sizeof(uint64_t) > arguments.size())
            {
                return make_mig_error_bytes(request, mig_error::bad_arguments);
            }

            const auto* node = node_of_request(emu, request);
            if (node == nullptr)
            {
                return make_mig_error_bytes(request, io_return::bad_argument);
            }

            const auto* property = property_of(*node, parsed.key);
            if (property == nullptr)
            {
                report_missing_property_once(emu, *node, parsed.key);
                return make_mig_error_bytes(request, io_return::not_found);
            }

            return serialized_buf_reply(emu, request, serialize_property(*property), read_u64(arguments, parsed.next),
                                        read_u64(arguments, parsed.next + sizeof(uint64_t)));
        }

        std::vector<uint8_t> registry_entry_get_properties_bin_routine(macos_emulator& emu, const mig_request& request)
        {
            const auto* node = node_of_request(emu, request);
            if (node == nullptr)
            {
                return make_mig_error_bytes(request, io_return::bad_argument);
            }

            return serialized_ool_reply(emu, request, serialize_properties(*node));
        }

        std::vector<uint8_t> registry_entry_get_properties_bin_buf_routine(macos_emulator& emu, const mig_request& request)
        {
            const auto arguments = arguments_of(request);
            if (2 * sizeof(uint64_t) > arguments.size())
            {
                return make_mig_error_bytes(request, mig_error::bad_arguments);
            }

            const auto* node = node_of_request(emu, request);
            if (node == nullptr)
            {
                return make_mig_error_bytes(request, io_return::bad_argument);
            }

            return serialized_buf_reply(emu, request, serialize_properties(*node), read_u64(arguments, 0),
                                        read_u64(arguments, sizeof(uint64_t)));
        }

        // Only IOSurfaceRoot has a user client. Everything else in the registry is a leaf a guest reads
        // properties off, and answering an open on one would hand back a connection whose every method
        // is refused -- worse than the failure a client is written to handle.
        std::vector<uint8_t> service_open_extended_routine(macos_emulator& emu, const mig_request& request)
        {
            const auto* node = node_of_request(emu, request);
            if (node == nullptr)
            {
                return make_mig_error_bytes(request, io_return::bad_argument);
            }

            if (node->class_name != IO_SURFACE_ROOT_CLASS)
            {
                static std::set<std::string> reported{};
                if (reported.insert(node->class_name).second)
                {
                    emu.log.warn("IOServiceOpen on %s is refused; sogen runs a user client for IOSurfaceRoot and nothing else\n",
                                 node->class_name.c_str());
                }

                return make_mig_error_bytes(request, io_return::unsupported);
            }

            const auto arguments = arguments_of(request);
            const auto connect_type = arguments.size() >= sizeof(uint32_t) ? read_u32(arguments, 0) : 0;

            if (connect_type != 0)
            {
                static std::set<uint32_t> reported{};
                if (reported.insert(connect_type).second)
                {
                    emu.log.warn("IOServiceOpen asked IOSurfaceRoot for connection type %u; sogen runs only type 0\n", connect_type);
                }

                return make_mig_error_bytes(request, io_return::unsupported);
            }

            const auto name = mint_io_object(emu, IOKIT_CONNECT_TAG | emu.ui.surfaces.open_connection());

            mig_reply_builder builder{request.call, emu.mach.ports};
            builder.append_port_descriptor({.name = name, .disposition = disposition::make_send, .type = descriptor_type::port});
            builder.append_ndr();
            builder.append_u32(static_cast<uint32_t>(kr::success));
            return builder.finish();
        }

        struct connect_method_arguments
        {
            bool valid{};
            io_connect_method_call call{};
        };

        connect_method_arguments read_connect_method_arguments(const mig_request& request)
        {
            const auto arguments = arguments_of(request);

            size_t offset = 0;
            const auto take_u32 = [&](uint32_t& out) {
                if (offset + sizeof(uint32_t) > arguments.size())
                {
                    return false;
                }

                out = read_u32(arguments, offset);
                offset += sizeof(uint32_t);
                return true;
            };

            const auto take_u64 = [&](uint64_t& out) {
                if (offset + sizeof(uint64_t) > arguments.size())
                {
                    return false;
                }

                out = read_u64(arguments, offset);
                offset += sizeof(uint64_t);
                return true;
            };

            connect_method_arguments parsed{};
            auto& call = parsed.call;

            uint32_t scalar_count = 0;
            if (!take_u32(call.selector) || !take_u32(scalar_count) || scalar_count > MAX_CONNECT_SCALARS)
            {
                return {};
            }

            call.scalar_input.resize(scalar_count);
            for (auto& scalar : call.scalar_input)
            {
                if (!take_u64(scalar))
                {
                    return {};
                }
            }

            uint32_t inband_count = 0;
            if (!take_u32(inband_count) || inband_count > arguments.size() - offset)
            {
                return {};
            }

            call.inband_input = arguments.subspan(offset, inband_count);
            offset += inband_count + ((4 - (inband_count % 4)) % 4);

            if (!take_u64(call.ool_input) || !take_u64(call.ool_input_size) || !take_u32(call.inband_output_max) ||
                !take_u32(call.scalar_output_max) || !take_u64(call.ool_output) || !take_u64(call.ool_output_max))
            {
                return {};
            }

            if (call.scalar_output_max > MAX_CONNECT_SCALARS)
            {
                return {};
            }

            parsed.valid = true;
            return parsed;
        }

        std::vector<uint8_t> connect_method_routine(macos_emulator& emu, const mig_request& request)
        {
            const auto object = emu.mach.ports.object_of(request.call.header.remote_port);
            if (!is_user_client(object))
            {
                return make_mig_error_bytes(request, io_return::bad_argument);
            }

            const auto parsed = read_connect_method_arguments(request);
            if (!parsed.valid)
            {
                return make_mig_error_bytes(request, mig_error::bad_arguments);
            }

            auto result = io_surface_user_client_method(emu, parsed.call);
            if (result.code != kr::success)
            {
                return make_mig_error_bytes(request, result.code);
            }

            if (result.scalar_output.size() > parsed.call.scalar_output_max)
            {
                result.scalar_output.resize(parsed.call.scalar_output_max);
            }

            mig_reply_builder builder{request.call, emu.mach.ports};
            builder.append_ndr();
            builder.append_u32(static_cast<uint32_t>(kr::success));
            builder.append_u32(static_cast<uint32_t>(result.inband_output.size()));

            std::vector<uint8_t> padded{result.inband_output};
            padded.resize(padded.size() + ((4 - (padded.size() % 4)) % 4), 0);
            builder.append_bytes(padded);

            builder.append_u32(static_cast<uint32_t>(result.scalar_output.size()));
            for (const auto scalar : result.scalar_output)
            {
                builder.append_u64(scalar);
            }

            builder.append_u64(result.ool_output_size);
            return builder.finish();
        }

        struct object_routine
        {
            int32_t id{};
            mig_routine routine{};
            const char* name{};
        };
    }

    // Everything IOKit does is MIG to the master port until a routine hands back an io_object port, after
    // which the entry routines go to that. The object routines are registered for the master port too:
    // the kernel runs one subsystem for both and answers kIOReturnBadArgument when the port names no
    // io_object, which is what IOObjectGetClass(masterPort) returns on the host.
    void register_iokit_routines(mig_server_table& table)
    {
        table.register_routine(kernel_object_kind::io_master, 2877, server_version_routine, "io_server_version");
        table.register_routine(kernel_object_kind::io_master, 2809, registry_entry_from_path_routine, "io_registry_entry_from_path");
        table.register_routine(kernel_object_kind::io_master, 2827, registry_get_root_entry_routine, "io_registry_get_root_entry");
        table.register_routine(kernel_object_kind::io_master, 2804, matching_services_xml_routine, "io_service_get_matching_services");
        table.register_routine(kernel_object_kind::io_master, 2873, matching_service_xml_routine, "io_service_get_matching_service");
        table.register_routine(kernel_object_kind::io_master, 2880, matching_service_bin_routine, "io_service_get_matching_service_bin");
        table.register_routine(kernel_object_kind::io_master, 2881, matching_services_bin_routine, "io_service_get_matching_services_bin");

        const std::vector<object_routine> object_routines{
            {2800, object_get_class_routine, "io_object_get_class"},
            {2801, object_conforms_to_routine, "io_object_conforms_to"},
            {2802, iterator_next_routine, "io_iterator_next"},
            {2805, registry_entry_get_property_xml_routine, "io_registry_entry_get_property"},
            {2810, registry_entry_get_name_routine, "io_registry_entry_get_name"},
            {2811, registry_entry_get_properties_xml_routine, "io_registry_entry_get_properties"},
            {2812, registry_entry_get_property_bytes_routine, "io_registry_entry_get_property_bytes"},
            {2826, registry_entry_get_path_routine, "io_registry_entry_get_path"},
            {2829, registry_entry_in_plane_routine, "io_registry_entry_in_plane"},
            {2830, object_get_retain_count_routine, "io_object_get_retain_count"},
            {2843, registry_entry_get_name_in_plane_routine, "io_registry_entry_get_name_in_plane"},
            {2871, registry_entry_get_registry_entry_id_routine, "io_registry_entry_get_registry_entry_id"},
            {2878, registry_entry_get_properties_bin_routine, "io_registry_entry_get_properties_bin"},
            {2879, registry_entry_get_property_bin_routine, "io_registry_entry_get_property_bin"},
            {2888, registry_entry_get_properties_bin_buf_routine, "io_registry_entry_get_properties_bin_buf"},
            {2889, registry_entry_get_property_bin_buf_routine, "io_registry_entry_get_property_bin_buf"},
            {2862, service_open_extended_routine, "io_service_open_extended"},
            {2865, connect_method_routine, "io_connect_method"},
        };

        for (const auto& [id, routine, name] : object_routines)
        {
            table.register_routine(kernel_object_kind::io_object, id, routine, name);
            table.register_routine(kernel_object_kind::io_master, id, routine, name);
        }
    }
}
