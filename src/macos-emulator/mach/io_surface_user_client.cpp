#include "../std_include.hpp"
#include "io_surface_user_client.hpp"

#include "../macos_emulator.hpp"
#include "mach_traps.hpp"

#include <address_utils.hpp>

#include <algorithm>
#include <set>

namespace sogen::mach
{
    namespace
    {
        namespace io_return
        {
            constexpr kern_return_t no_memory = static_cast<kern_return_t>(0xE00002BDu);
            constexpr kern_return_t bad_argument = static_cast<kern_return_t>(0xE00002C2u);
            constexpr kern_return_t unsupported = static_cast<kern_return_t>(0xE00002C7u);
        }

        namespace os_serialize
        {
            constexpr uint32_t signature = 0x000000D3u;
            constexpr uint32_t dictionary = 0x01000000u;
            constexpr uint32_t array = 0x02000000u;
            constexpr uint32_t set = 0x03000000u;
            constexpr uint32_t number = 0x04000000u;
            constexpr uint32_t symbol = 0x08000000u;
            constexpr uint32_t string = 0x09000000u;
            constexpr uint32_t data = 0x0A000000u;
            constexpr uint32_t boolean = 0x0B000000u;
            constexpr uint32_t object = 0x0C000000u;
            constexpr uint32_t end_collection = 0x80000000u;
            constexpr uint32_t type_mask = 0x7F000000u;
            constexpr uint32_t length_mask = 0x00FFFFFFu;
        }

        // IOSurfaceRootUserClient's external methods, by the selector each client entry point sends.
        // Measured on 25G76 by breaking on mach_msg2_internal for routine 2865 and reading the selector
 // together with the caller:.
        namespace selector
        {
            constexpr uint32_t create_surface = 0;
            constexpr uint32_t set_value = 9;
            constexpr uint32_t copy_all_values = 10;
            constexpr uint32_t get_limits = 13;
            constexpr uint32_t set_purgeable = 20;
            constexpr uint32_t create_mach_port = 35;
        }

        // Trap indices on the same user client, read out of the w1 every IOConnectTrapN call site in
        // IOSurface loads. 6..12 belong to shared events, bulk attachments and the transaction list.
        namespace trap_index
        {
            constexpr uint32_t increment_use_count = 0;
            constexpr uint32_t decrement_use_count = 1;
            constexpr uint32_t lock = 2;
            constexpr uint32_t unlock = 3;
            constexpr uint32_t release = 4;
            constexpr uint32_t retain = 5;
        }

        // The 40 bytes selector 13 answers on 25G76, verbatim: five 64-bit limits that
        // _iosConnectInitalize caches and IOSurfaceClientGetPropertyMaximum then reads back as the
        // BytesPerRow, Width and Height maxima. The first and last words have no exported accessor, so
        // they are carried rather than named.
        constexpr std::array<uint64_t, 5> SURFACE_LIMITS{0x0000007F00003FFFull, 0x0000000000008000ull, 0x0000000000004000ull,
                                                         0x0000000000004000ull, 0x0000000000010000ull};

        constexpr uint64_t MAX_BYTES_PER_ROW = SURFACE_LIMITS[1];
        constexpr uint64_t MAX_SURFACE_DIMENSION = SURFACE_LIMITS[2];

        // The limits above allow a 16384-square surface at a 32768-byte stride, which is half a gigabyte
        // of guest memory in one allocation. sogen refuses past this and says so rather than mapping it.
        constexpr uint64_t MAX_ALLOC_SIZE = 256ull * 1024 * 1024;

        // IOSurfaceLock's options. A reader's unlock leaves the seed alone; only a writer's bumps it,
        // which is what tells every other holder its copy of the pixels is stale.
        constexpr uint64_t IO_SURFACE_LOCK_READ_ONLY = 1;

        // A surface's mach port shares the io_object id space with the registry nodes and the user-client
        // connections, so it carries a tag of its own rather than a bare surface id.
        constexpr uint64_t IOKIT_SURFACE_PORT_TAG = 0x2000000000000000ull;

        // Offsets into the record selector 0 answers with. The client copies the whole thing into its
        // IOSurfaceClient at +0x70 and reads it with fixed loads, so each of these is the offset of an
        // IOSurfaceClientGet* accessor minus 0x70.
        namespace record
        {
            constexpr size_t base = 0x00;
            constexpr size_t info = 0x08;
            constexpr size_t timestamps = 0x10;
            constexpr size_t id = 0x18;
            constexpr size_t alloc_size = 0x20;
            constexpr size_t width = 0x28;
            constexpr size_t height = 0x30;
            constexpr size_t bytes_per_row = 0x38;
            constexpr size_t base_offset = 0x40;
            constexpr size_t pixel_format = 0x48;
            constexpr size_t total_size = 0x50;
            constexpr size_t plane_count = 0x58;
            constexpr size_t bytes_per_element = 0x60;
            constexpr size_t element_width = 0x62;
            constexpr size_t element_height = 0x63;
            constexpr size_t cache_mode = 0x64;
            constexpr size_t parent_id = 0x70;
        }

        // The per-surface record the client maps read-only and polls without a round trip.
        // IOSurfaceClientGetSeed reads +0x0c and IOSurfaceClientIsInUse reads +0x18.
        namespace info_record
        {
            constexpr size_t seed = 0x0C;
            constexpr size_t use_count = 0x18;
            constexpr size_t size = 0xD0;
        }

        void report_unmodelled_trap(macos_emulator& emu, const uint32_t index, const std::string_view target)
        {
            static std::set<uint32_t> reported{};
            if (reported.insert(index).second)
            {
                emu.log.warn("iokit_user_client_trap index %u on %.*s is not modelled\n", index, static_cast<int>(target.size()),
                             target.data());
            }
        }

        void report_once(macos_emulator& emu, const std::string& key, const std::string& message)
        {
            static std::set<std::string> reported{};
            if (reported.insert(key).second)
            {
                emu.log.warn("%s\n", message.c_str());
            }
        }

        size_t padded_length(const size_t length)
        {
            return length + ((4 - (length % 4)) % 4);
        }

        struct binary_parser
        {
            std::span<const uint8_t> bytes{};
            size_t offset{};
            std::vector<os_value> objects{};
            bool failed{};

            bool take_token(uint32_t& token)
            {
                if (this->failed || this->offset + sizeof(uint32_t) > this->bytes.size())
                {
                    this->failed = true;
                    return false;
                }

                token = read_u32(this->bytes, this->offset);
                this->offset += sizeof(uint32_t);
                return true;
            }

            bool take_payload(const size_t length, std::span<const uint8_t>& payload)
            {
                const auto padded = padded_length(length);
                if (this->offset + padded > this->bytes.size())
                {
                    this->failed = true;
                    return false;
                }

                payload = this->bytes.subspan(this->offset, length);
                this->offset += padded;
                return true;
            }

            // xnu registers every object it unserialises, the enclosing collection before its children,
            // and kOSSerializeObject is a zero-based index into that order. Skipping the registration
            // for keys -- or for the collection itself -- makes every back-reference resolve one slot
            // off, which reads a key as a value rather than failing.
            size_t parse_object()
            {
                uint32_t token = 0;
                if (!this->take_token(token))
                {
                    return 0;
                }

                const auto type = token & os_serialize::type_mask;
                const auto length = static_cast<size_t>(token & os_serialize::length_mask);

                if (type == os_serialize::object)
                {
                    if (length >= this->objects.size())
                    {
                        this->failed = true;
                        return 0;
                    }

                    return length;
                }

                const auto index = this->objects.size();
                this->objects.push_back({.type = type, .length = static_cast<uint32_t>(length)});

                std::span<const uint8_t> payload{};

                switch (type)
                {
                case os_serialize::number:
                    if (!this->take_payload(sizeof(uint64_t), payload))
                    {
                        return 0;
                    }

                    this->objects[index].number = read_u64(payload, 0);
                    return index;

                case os_serialize::symbol:
                    if (length == 0 || !this->take_payload(length, payload))
                    {
                        this->failed = true;
                        return 0;
                    }

                    this->objects[index].text.assign(reinterpret_cast<const char*>(payload.data()), length - 1);
                    return index;

                case os_serialize::string:
                    if (!this->take_payload(length, payload))
                    {
                        return 0;
                    }

                    this->objects[index].text.assign(reinterpret_cast<const char*>(payload.data()), length);
                    return index;

                case os_serialize::data:
                    if (!this->take_payload(length, payload))
                    {
                        return 0;
                    }

                    this->objects[index].data.assign(payload.begin(), payload.end());
                    return index;

                case os_serialize::boolean:
                    this->objects[index].number = length != 0 ? 1 : 0;
                    return index;

                case os_serialize::dictionary:
                    for (size_t i = 0; i < length && !this->failed; ++i)
                    {
                        this->parse_object();
                        this->parse_object();
                    }

                    return index;

                case os_serialize::array:
                case os_serialize::set:
                    for (size_t i = 0; i < length && !this->failed; ++i)
                    {
                        this->parse_object();
                    }

                    return index;

                default:
                    this->failed = true;
                    return 0;
                }
            }
        };

        std::optional<binary_parser> parse_binary(const std::span<const uint8_t> bytes)
        {
            if (bytes.size() < 8 || read_u32(bytes, 0) != os_serialize::signature)
            {
                return std::nullopt;
            }

            binary_parser parser{.bytes = bytes, .offset = sizeof(uint32_t)};
            parser.parse_object();

            if (parser.failed || parser.objects.empty())
            {
                return std::nullopt;
            }

            return parser;
        }

        void append_u32le(std::vector<uint8_t>& out, const uint32_t value)
        {
            std::array<uint8_t, sizeof(uint32_t)> scratch{};
            write_u32(scratch, 0, value);
            out.insert(out.end(), scratch.begin(), scratch.end());
        }

        void append_token(std::vector<uint8_t>& out, const uint32_t type, const size_t length, const bool last)
        {
            append_u32le(out,
                         type | (last ? os_serialize::end_collection : 0u) | (static_cast<uint32_t>(length) & os_serialize::length_mask));
        }

        void append_padded(std::vector<uint8_t>& out, const std::span<const uint8_t> payload)
        {
            out.insert(out.end(), payload.begin(), payload.end());
            out.resize(out.size() + (padded_length(payload.size()) - payload.size()), 0);
        }

        void append_value(std::vector<uint8_t>& out, const os_value& value, const bool last)
        {
            switch (value.type)
            {
            case os_serialize::number: {
                append_token(out, os_serialize::number, value.length, last);
                std::array<uint8_t, sizeof(uint64_t)> scratch{};
                write_u64(scratch, 0, value.number);
                out.insert(out.end(), scratch.begin(), scratch.end());
                return;
            }

            case os_serialize::boolean:
                append_token(out, os_serialize::boolean, value.number != 0 ? 1 : 0, last);
                return;

            case os_serialize::data:
                append_token(out, os_serialize::data, value.data.size(), last);
                append_padded(out, value.data);
                return;

            case os_serialize::symbol: {
                append_token(out, os_serialize::symbol, value.text.size() + 1, last);
                const auto terminated = value.text + '\0';
                append_padded(out, {reinterpret_cast<const uint8_t*>(terminated.data()), terminated.size()});
                return;
            }

            default:
                append_token(out, os_serialize::string, value.text.size(), last);
                append_padded(out, {reinterpret_cast<const uint8_t*>(value.text.data()), value.text.size()});
                return;
            }
        }

        bool value_is_scalar(const os_value& value)
        {
            return value.type == os_serialize::number || value.type == os_serialize::boolean || value.type == os_serialize::data ||
                   value.type == os_serialize::string || value.type == os_serialize::symbol;
        }

        const os_value* entry_of(const std::vector<os_dictionary_entry>& entries, const std::string_view key)
        {
            for (const auto& entry : entries)
            {
                if (entry.key == key)
                {
                    return &entry.value;
                }
            }

            return nullptr;
        }

        uint64_t number_or(const std::vector<os_dictionary_entry>& entries, const std::string_view key, const uint64_t fallback)
        {
            const auto* value = entry_of(entries, key);
            return value != nullptr && value->type == os_serialize::number ? value->number : fallback;
        }

        uint64_t divide_up(const uint64_t value, const uint64_t divisor)
        {
            return divisor == 0 ? value : (value + divisor - 1) / divisor;
        }
    }

    std::optional<std::vector<os_dictionary_entry>> parse_binary_dictionary(const std::span<const uint8_t> bytes)
    {
        if (bytes.size() < 2 * sizeof(uint32_t) || read_u32(bytes, 0) != os_serialize::signature)
        {
            return std::nullopt;
        }

        binary_parser walker{.bytes = bytes, .offset = sizeof(uint32_t)};

        uint32_t header = 0;
        if (!walker.take_token(header) || (header & os_serialize::type_mask) != os_serialize::dictionary)
        {
            return std::nullopt;
        }

        const auto count = static_cast<size_t>(header & os_serialize::length_mask);
        walker.objects.push_back({.type = os_serialize::dictionary, .length = static_cast<uint32_t>(count)});

        std::vector<os_dictionary_entry> entries{};
        entries.reserve(count);

        for (size_t i = 0; i < count; ++i)
        {
            const auto key = walker.parse_object();
            const auto value = walker.parse_object();

            if (walker.failed || key >= walker.objects.size() || value >= walker.objects.size())
            {
                return std::nullopt;
            }

            entries.push_back({.key = walker.objects[key].text, .value = walker.objects[value]});
        }

        return entries;
    }

    std::vector<uint8_t> serialize_binary_dictionary(const std::span<const os_dictionary_entry> entries)
    {
        std::vector<uint8_t> out{};
        append_u32le(out, os_serialize::signature);
        append_token(out, os_serialize::dictionary, entries.size(), true);

        for (size_t i = 0; i < entries.size(); ++i)
        {
            os_value key{.type = os_serialize::symbol, .text = entries[i].key};
            append_value(out, key, false);
            append_value(out, entries[i].value, i + 1 == entries.size());
        }

        return out;
    }

    io_surface* io_surface_store::find(const uint32_t id)
    {
        const auto match = this->surfaces_.find(id);
        return match == this->surfaces_.end() ? nullptr : &match->second;
    }

    bool io_surface_store::release(macos_emulator& emu, const uint32_t id)
    {
        auto* surface = this->find(id);
        if (surface == nullptr)
        {
            return false;
        }

        if (surface->references > 1)
        {
            --surface->references;
            return true;
        }

        if (surface->base != 0)
        {
            emu.memory.release_memory(surface->base, surface->base_reserved);
        }

        if (surface->info != 0)
        {
            emu.memory.release_memory(surface->info, surface->info_reserved);
        }

        this->surfaces_.erase(id);
        return true;
    }

    // Everything a surface is made of comes out of the property dictionary the guest serialised, and the
    // fields it leaves out are derived the way the kernel derives them -- a dictionary carrying only a
    // width and a height is a valid IOSurfaceCreate and has to produce a mapped buffer.
    io_surface* io_surface_store::create(macos_emulator& emu, const std::span<const uint8_t> properties)
    {
        const auto entries = parse_binary_dictionary(properties);
        if (!entries.has_value())
        {
            report_once(emu, "iosurface-create-properties",
                        "IOSurfaceCreate was handed a properties dictionary sogen cannot unserialise; no surface is made");
            return nullptr;
        }

        io_surface surface{};
        surface.width = number_or(*entries, "IOSurfaceWidth", 0);
        surface.height = number_or(*entries, "IOSurfaceHeight", 0);
        surface.pixel_format = static_cast<uint32_t>(number_or(*entries, "IOSurfacePixelFormat", 0));
        surface.bytes_per_element = static_cast<uint16_t>(number_or(*entries, "IOSurfaceBytesPerElement", 1));
        surface.element_width = static_cast<uint8_t>(number_or(*entries, "IOSurfaceElementWidth", 1));
        surface.element_height = static_cast<uint8_t>(number_or(*entries, "IOSurfaceElementHeight", 1));
        surface.cache_mode = static_cast<uint32_t>(number_or(*entries, "IOSurfaceCacheMode", 0));

        if (surface.bytes_per_element == 0)
        {
            surface.bytes_per_element = 1;
        }

        if (surface.element_width == 0)
        {
            surface.element_width = 1;
        }

        if (surface.element_height == 0)
        {
            surface.element_height = 1;
        }

        surface.bytes_per_row =
            number_or(*entries, "IOSurfaceBytesPerRow", divide_up(surface.width, surface.element_width) * surface.bytes_per_element);
        surface.alloc_size =
            number_or(*entries, "IOSurfaceAllocSize", surface.bytes_per_row * divide_up(surface.height, surface.element_height));

        if (const auto* planes = entry_of(*entries, "IOSurfacePlaneInfo"); planes != nullptr)
        {
            report_once(emu, "iosurface-planes",
                        "IOSurfaceCreate asked for a planar surface; sogen models single-plane surfaces only and the plane "
                        "descriptors are dropped");
        }

        for (const auto& entry : *entries)
        {
            if (!value_is_scalar(entry.value))
            {
                report_once(emu, "iosurface-property:" + entry.key,
                            "IOSurface property \"" + entry.key +
                                "\" is a collection; sogen stores scalar properties only and this one is dropped");
                continue;
            }

            surface.values.push_back(entry);
        }

        if (surface.width == 0 || surface.height == 0 || surface.alloc_size == 0)
        {
            report_once(emu, "iosurface-create-empty",
                        "IOSurfaceCreate asked for a surface with no pixels; sogen refuses it rather than mapping nothing");
            return nullptr;
        }

        if (surface.width > MAX_SURFACE_DIMENSION || surface.height > MAX_SURFACE_DIMENSION || surface.bytes_per_row > MAX_BYTES_PER_ROW ||
            surface.alloc_size > MAX_ALLOC_SIZE)
        {
            emu.log.warn("IOSurfaceCreate asked for %" PRIu64 "x%" PRIu64 " at %" PRIu64 " bytes per row (%" PRIu64
                         " bytes), past what sogen will map\n",
                         surface.width, surface.height, surface.bytes_per_row, surface.alloc_size);
            return nullptr;
        }

        surface.base_reserved = page_align_up(surface.alloc_size, MACOS_PAGE_SIZE);
        surface.base =
            emu.memory.allocate_memory(static_cast<size_t>(surface.base_reserved), memory_permission::read_write, MACOS_DEFAULT_MMAP_BASE);

        if (surface.base == 0)
        {
            report_once(emu, "iosurface-create-memory", "IOSurfaceCreate could not map a backing buffer for the surface");
            return nullptr;
        }

        // Read-only because that is how the host maps it: the seed and the use count are the kernel's to
        // move, and the client polls them without a round trip.
        surface.info_reserved = MACOS_PAGE_SIZE;
        surface.info =
            emu.memory.allocate_memory(static_cast<size_t>(surface.info_reserved), memory_permission::read, MACOS_DEFAULT_MMAP_BASE);

        if (surface.info == 0)
        {
            emu.memory.release_memory(surface.base, surface.base_reserved);
            report_once(emu, "iosurface-create-info", "IOSurfaceCreate could not map the shared record for the surface");
            return nullptr;
        }

        surface.id = this->next_id_++;
        ++this->created_;

        emu.log.info("IOSurface %u: %" PRIu64 "x%" PRIu64 ", %" PRIu64 " bytes per row, format %08x, %" PRIu64 " bytes at 0x%" PRIx64 "\n",
                     surface.id, surface.width, surface.height, surface.bytes_per_row, surface.pixel_format, surface.alloc_size,
                     surface.base);

        auto& stored = this->surfaces_[surface.id];
        stored = std::move(surface);
        return &stored;
    }

    namespace
    {
        void write_info_record(macos_emulator& emu, const io_surface& surface)
        {
            std::array<uint8_t, info_record::size> record{};
            write_u32(record, info_record::seed, surface.seed);
            write_u64(record, info_record::use_count, static_cast<uint64_t>(std::max<int64_t>(surface.use_count, 0)));
            emu.memory.try_write_memory(surface.info, record.data(), record.size());
        }

        std::vector<uint8_t> build_surface_record(const io_surface& surface)
        {
            std::vector<uint8_t> out(IO_SURFACE_RECORD_SIZE, 0);

            write_u64(out, record::base, surface.base);
            write_u64(out, record::info, surface.info);
            write_u64(out, record::timestamps, 0);
            write_u32(out, record::id, surface.id);
            write_u64(out, record::alloc_size, surface.alloc_size);
            write_u64(out, record::width, surface.width);
            write_u64(out, record::height, surface.height);
            write_u64(out, record::bytes_per_row, surface.bytes_per_row);
            write_u64(out, record::base_offset, 0);
            write_u32(out, record::pixel_format, surface.pixel_format);
            write_u64(out, record::total_size, surface.alloc_size);
            write_u64(out, record::plane_count, 0);
            out[record::bytes_per_element] = static_cast<uint8_t>(surface.bytes_per_element & 0xFFu);
            out[record::bytes_per_element + 1] = static_cast<uint8_t>((surface.bytes_per_element >> 8) & 0xFFu);
            out[record::element_width] = surface.element_width;
            out[record::element_height] = surface.element_height;
            write_u32(out, record::cache_mode, surface.cache_mode);
            write_u32(out, record::parent_id, 0);

            return out;
        }

        io_surface* surface_of_scalar(macos_emulator& emu, const io_connect_method_call& call, const size_t index)
        {
            if (index >= call.scalar_input.size())
            {
                return nullptr;
            }

            return emu.ui.surfaces.find(static_cast<uint32_t>(call.scalar_input[index]));
        }

        // Every selector that names a surface inband does it the same way: a leading uint32 id, then
        // twelve bytes of header before the serialised payload starts.
        constexpr size_t INBAND_SURFACE_HEADER = 12;

        io_connect_method_result create_surface_method(macos_emulator& emu, const io_connect_method_call& call)
        {
            auto* surface = emu.ui.surfaces.create(emu, call.inband_input);
            if (surface == nullptr)
            {
                return {.code = io_return::no_memory};
            }

            write_info_record(emu, *surface);

            auto record = build_surface_record(*surface);
            if (call.inband_output_max < record.size())
            {
                report_once(emu, "iosurface-record-size",
                            "IOSurfaceCreate asked for a surface record smaller than the " + std::to_string(IO_SURFACE_RECORD_SIZE) +
                                " bytes sogen writes; this IOSurface build reads a different structure");
                record.resize(call.inband_output_max);
            }

            return {.code = kr::success, .inband_output = std::move(record)};
        }

        // IOSurfaceSetValue serialises an array of exactly two elements, value first and key second,
        // behind the twelve-byte header. Reading them the other way round stores the value under itself.
        io_connect_method_result set_value_method(macos_emulator& emu, const io_connect_method_call& call)
        {
            if (call.inband_input.size() <= INBAND_SURFACE_HEADER)
            {
                return {.code = io_return::bad_argument};
            }

            auto* surface = emu.ui.surfaces.find(read_u32(call.inband_input, 0));
            if (surface == nullptr)
            {
                return {.code = io_return::bad_argument};
            }

            const auto parsed = parse_binary(call.inband_input.subspan(INBAND_SURFACE_HEADER));
            if (!parsed.has_value() || parsed->objects.front().type != os_serialize::array || parsed->objects.size() < 3)
            {
                report_once(emu, "iosurface-setvalue-shape",
                            "IOSurfaceSetValue sent something other than a two-element [value, key] array; the value is dropped");
                return {.code = io_return::bad_argument};
            }

            const auto& value = parsed->objects[1];
            const auto& key = parsed->objects[2];

            if (key.text.empty() || !value_is_scalar(value))
            {
                report_once(emu, "iosurface-setvalue-kind",
                            "IOSurfaceSetValue was given a key or a value sogen does not store; only scalar properties are kept");
                return {.code = io_return::unsupported};
            }

            const auto existing =
                std::ranges::find_if(surface->values, [&](const os_dictionary_entry& entry) { return entry.key == key.text; });
            if (existing != surface->values.end())
            {
                existing->value = value;
            }
            else
            {
                surface->values.push_back({.key = key.text, .value = value});
            }

            std::vector<uint8_t> inband(sizeof(uint32_t), 0);
            write_u32(inband, 0, static_cast<uint32_t>(surface->values.size()));
            return {.code = kr::success, .inband_output = std::move(inband)};
        }

        io_connect_method_result copy_all_values_method(macos_emulator& emu, const io_connect_method_call& call)
        {
            if (call.inband_input.size() < sizeof(uint32_t))
            {
                return {.code = io_return::bad_argument};
            }

            const auto* surface = emu.ui.surfaces.find(read_u32(call.inband_input, 0));
            if (surface == nullptr)
            {
                return {.code = io_return::bad_argument};
            }

            const auto payload = serialize_binary_dictionary(surface->values);

            if (call.ool_output == 0 || payload.size() > call.ool_output_max)
            {
                report_once(emu, "iosurface-copyvalues-buffer",
                            "IOSurfaceCopyAllValues has no out-of-line buffer big enough for the surface's properties");
                return {.code = io_return::no_memory};
            }

            if (!emu.memory.try_write_memory(call.ool_output, payload.data(), payload.size()))
            {
                return {.code = io_return::bad_argument};
            }

            return {.code = kr::success, .ool_output_size = payload.size()};
        }

        io_connect_method_result get_limits_method(const io_connect_method_call& call)
        {
            std::vector<uint8_t> inband(SURFACE_LIMITS.size() * sizeof(uint64_t), 0);
            for (size_t i = 0; i < SURFACE_LIMITS.size(); ++i)
            {
                write_u64(inband, i * sizeof(uint64_t), SURFACE_LIMITS[i]);
            }

            if (call.inband_output_max < inband.size())
            {
                inband.resize(call.inband_output_max);
            }

            return {.code = kr::success, .inband_output = std::move(inband)};
        }

        io_connect_method_result set_purgeable_method(macos_emulator& emu, const io_connect_method_call& call)
        {
            auto* surface = surface_of_scalar(emu, call, 0);
            if (surface == nullptr || call.scalar_input.size() < 2)
            {
                return {.code = io_return::bad_argument};
            }

            const auto previous = surface->purgeable_state;
            surface->purgeable_state = call.scalar_input[1];

            // sogen never reclaims a volatile surface: the emulated machine is not under memory pressure,
            // and a surface whose pages went away between a lock and an unlock would lose the picture.
            return {.code = kr::success, .scalar_output = {previous}};
        }

        // The port is what a surface is passed to another process by. sogen runs one process, so the
        // right names the surface and nothing receives it; a guest that sends it somewhere gets a port
        // whose messages are refused, which is what every other unserviced port here does.
        io_connect_method_result create_mach_port_method(macos_emulator& emu, const io_connect_method_call& call)
        {
            auto* surface = surface_of_scalar(emu, call, 0);
            if (surface == nullptr)
            {
                return {.code = io_return::bad_argument};
            }

            const auto name =
                emu.mach.ports.allocate_receive_right({.kind = kernel_object_kind::io_object, .id = IOKIT_SURFACE_PORT_TAG | surface->id});
            emu.mach.ports.insert_send_right(name);

            return {.code = kr::success, .scalar_output = {name}};
        }
    }

    io_connect_method_result io_surface_user_client_method(macos_emulator& emu, const io_connect_method_call& call)
    {
        switch (call.selector)
        {
        case selector::create_surface:
            return create_surface_method(emu, call);

        case selector::set_value:
            return set_value_method(emu, call);

        case selector::copy_all_values:
            return copy_all_values_method(emu, call);

        case selector::get_limits:
            return get_limits_method(call);

        case selector::set_purgeable:
            return set_purgeable_method(emu, call);

        case selector::create_mach_port:
            return create_mach_port_method(emu, call);

        default:
            break;
        }

        report_once(emu, "iosurface-selector:" + std::to_string(call.selector),
                    "IOSurfaceRootUserClient selector " + std::to_string(call.selector) + " is not modelled (" +
                        std::to_string(call.scalar_input.size()) + " scalars in, " + std::to_string(call.inband_input.size()) +
                        " inband bytes in, " + std::to_string(call.scalar_output_max) + " scalars out, " +
                        std::to_string(call.inband_output_max) + " inband bytes out)");

        return {.code = io_return::unsupported};
    }
}

namespace sogen::mach_traps
{
    void trap_iokit_user_client(const macos_syscall_context& c)
    {
        auto& emu = c.emu_ref;

        const auto connect = static_cast<mach::port_name_t>(get_macos_syscall_argument(c, 0));
        const auto index = static_cast<uint32_t>(get_macos_syscall_argument(c, 1));
        const auto first = get_macos_syscall_argument(c, 2);
        const auto second = get_macos_syscall_argument(c, 3);
        const auto third = get_macos_syscall_argument(c, 4);

        const auto object = emu.mach.ports.object_of(connect);
        if (object.kind != mach::kernel_object_kind::io_object || (object.id & mach::IOKIT_CONNECT_TAG) == 0)
        {
            mach::report_unmodelled_trap(emu, index, "a port that is not an IOKit user client");
            write_mach_result(c, mach::kr::invalid_argument);
            return;
        }

        auto* surface = emu.ui.surfaces.find(static_cast<uint32_t>(first));

        switch (index)
        {
        case mach::trap_index::increment_use_count:
        case mach::trap_index::decrement_use_count: {
            if (surface == nullptr)
            {
                write_mach_result(c, mach::io_return::bad_argument);
                return;
            }

            surface->use_count += index == mach::trap_index::increment_use_count ? 1 : -1;
            mach::write_info_record(emu, *surface);
            write_mach_result(c, mach::kr::success);
            return;
        }

        case mach::trap_index::lock:
        case mach::trap_index::unlock: {
            if (surface == nullptr)
            {
                write_mach_result(c, mach::io_return::bad_argument);
                return;
            }

            if (index == mach::trap_index::lock)
            {
                ++surface->lock_depth;
            }
            else if (surface->lock_depth > 0)
            {
                --surface->lock_depth;

                if ((second & mach::IO_SURFACE_LOCK_READ_ONLY) == 0)
                {
                    ++surface->seed;
                }
            }

            mach::write_info_record(emu, *surface);

            if (third != 0)
            {
                const auto seed = surface->seed;
                if (!emu.memory.try_write_memory(third, &seed, sizeof(seed)))
                {
                    write_mach_result(c, mach::kr::invalid_address);
                    return;
                }
            }

            write_mach_result(c, mach::kr::success);
            return;
        }

        case mach::trap_index::retain: {
            if (surface == nullptr)
            {
                write_mach_result(c, mach::io_return::bad_argument);
                return;
            }

            ++surface->references;
            write_mach_result(c, mach::kr::success);
            return;
        }

        case mach::trap_index::release: {
            if (!emu.ui.surfaces.release(emu, static_cast<uint32_t>(first)))
            {
                write_mach_result(c, mach::io_return::bad_argument);
                return;
            }

            write_mach_result(c, mach::kr::success);
            return;
        }

        default:
            break;
        }

        mach::report_unmodelled_trap(emu, index, "the IOSurfaceRoot user client");
        write_mach_result(c, mach::io_return::unsupported);
    }
}
