#include "../std_include.hpp"
#include "dns_resolver.hpp"

#include "binary_writer.hpp"
#include "../windows_emulator.hpp"

#define DNS_TYPE_A     0x01
#define DNS_TYPE_CNAME 0x05
#define DNS_TYPE_AAAA  0x1C

#ifndef OS_WINDOWS
#define ERROR_SUCCESS              0x0
#define DNS_ERROR_RCODE_NAME_ERROR 0x232B
#endif

namespace
{
    constexpr DWORD k_dns_record_flags = 0x2009;
    constexpr DWORD k_dns_record_flags_without_name = 0x0009;
    constexpr DWORD k_default_ttl = 0x91;
    constexpr DWORD k_proc2_single_record_a_ttl = 0x5E;
    constexpr DWORD k_proc2_aaaa_direct_ttl = 0x70;
    constexpr DWORD k_proc3_default_ttl = 0x12C;
    constexpr DWORD k_native_record_reserved = 1;
    constexpr DWORD k_native_record_rank = 1;
    constexpr uint64_t k_native_pointer_referent = 0x20000;
    constexpr size_t k_dns_record_data_size = 16;

    struct resolved_dns_record
    {
        std::u16string name;
        WORD type{};
        WORD data_length{};
        DWORD flags{k_dns_record_flags};
        DWORD ttl{k_default_ttl};
        DWORD reserved{k_native_record_reserved};
        std::array<std::byte, k_dns_record_data_size> data{};
    };

    struct dns_query_request
    {
        std::u16string hostname;
        WORD type{};
        DWORD flags{};
        uint64_t rpc_query_options{};
    };

    enum class proc2_query_family
    {
        record_query,
        addrinfo_compatible_query,
        unsupported,
    };

    proc2_query_family classify_proc2_query_family(const dns_query_request& request)
    {
        switch (request.type)
        {
        case DNS_TYPE_A:
            return proc2_query_family::record_query;
        case DNS_TYPE_AAAA:
            return proc2_query_family::addrinfo_compatible_query;
        default:
            return proc2_query_family::unsupported;
        }
    }

    // R_ResolverQuery() returns a marshaled PDNS_RECORD chain. The semantic
    // structure comes from nt5src/.../resolver/idl/resrpc.h and the modern
    // dnsapi client later runs FixupNameOwnerPointers() over the decoded list.
    // We model that explicitly: the first record in a chain carries the owner
    // name, later records can omit it and rely on fixup semantics.
    struct rpc_dns_record_header
    {
        WORD type{};
        WORD data_length{};
        DWORD flags{};
        DWORD ttl{};
        DWORD reserved{};
        uint64_t rank{};
    };

    struct rpc_proc2_record_wire_layout
    {
        static constexpr size_t record_with_owner_name_size = 0x30;
        static constexpr size_t chained_record_size = 0x20;
        static constexpr size_t max_marshaled_size = record_with_owner_name_size;

        rpc_dns_record_header header{};
        std::array<std::byte, k_dns_record_data_size> data{};
    };

    struct rpc_proc3_record_wire_layout
    {
        static constexpr size_t marshaled_size = 0x28;

        rpc_dns_record_header header{};
        std::array<std::byte, k_dns_record_data_size> data{};
    };

    struct rpc_proc2_record_semantics
    {
        const resolved_dns_record* record{};
        bool is_owner_name_record{};
        bool uses_embedded_owner_name{};
    };

    struct rpc_proc2_response_shape
    {
        bool has_result_record_list{};
        bool has_alias_record_list{};
        bool has_owner_name_anchor{};
        size_t payload_size{};
    };

    template <typename Traits>
    void write_rpc_pointer(utils::aligned_binary_writer<Traits>& writer, const bool present)
    {
        writer.template write<typename Traits::PVOID>(present ? k_native_pointer_referent : 0);
    }

    template <typename Traits>
    void write_proc2_result_record_list_pointer(utils::aligned_binary_writer<Traits>& writer, const bool present)
    {
        write_rpc_pointer(writer, present);
    }

    template <typename Traits>
    void write_proc2_alias_record_list_pointer(utils::aligned_binary_writer<Traits>& writer, const bool present)
    {
        write_rpc_pointer(writer, present);
    }

    template <typename Traits>
    void write_proc2_name_owner_pointer(utils::aligned_binary_writer<Traits>& writer, const bool present)
    {
        write_rpc_pointer(writer, present);
    }

    std::vector<rpc_proc2_record_semantics> build_proc2_rpc_record_chain(const std::vector<resolved_dns_record>& records)
    {
        std::vector<rpc_proc2_record_semantics> chain;
        chain.reserve(records.size());

        // Mirrors the producer/client contract in the NT source:
        // Cache_PrepareRecordList() may null owner names on later records and
        // dnsapi later restores them with FixupNameOwnerPointers(). Native
        // dumps additionally show that the single-record proc2 case keeps the
        // owner name entirely out-of-line, while the multi-record case uses the
        // larger first-record layout.
        const bool has_multiple_records = records.size() > 1;
        for (size_t index = 0; index < records.size(); ++index)
        {
            chain.push_back(rpc_proc2_record_semantics{
                .record = &records[index],
                .is_owner_name_record = index == 0,
                .uses_embedded_owner_name = has_multiple_records && index == 0,
            });
        }

        return chain;
    }

    rpc_proc2_response_shape describe_proc2_response_shape(const std::vector<rpc_proc2_record_semantics>& record_chain, const DWORD status)
    {
        if (status != ERROR_SUCCESS || record_chain.empty())
        {
            return rpc_proc2_response_shape{
                .has_result_record_list = false,
                .has_alias_record_list = false,
                .has_owner_name_anchor = false,
                .payload_size = static_cast<size_t>(0x18),
            };
        }

        const bool has_multiple_records = record_chain.size() > 1;
        return rpc_proc2_response_shape{
            .has_result_record_list = true,
            .has_alias_record_list = has_multiple_records,
            .has_owner_name_anchor = true,
            .payload_size = static_cast<size_t>(has_multiple_records ? 0x100 : 0xD0),
        };
    }

    rpc_dns_record_header build_rpc_dns_record_header(const resolved_dns_record& record, const DWORD flags, const uint64_t rank,
                                                      const DWORD reserved)
    {
        return rpc_dns_record_header{
            .type = record.type,
            .data_length = record.data_length,
            .flags = flags,
            .ttl = record.ttl,
            .reserved = reserved,
            .rank = rank,
        };
    }

    void copy_rpc_record_to_bytes(std::array<std::byte, rpc_proc2_record_wire_layout::max_marshaled_size>& buffer,
                                  const rpc_proc2_record_wire_layout& wire)
    {
        memcpy(buffer.data() + 0x00, &wire.header.type, sizeof(wire.header.type));
        memcpy(buffer.data() + 0x02, &wire.header.data_length, sizeof(wire.header.data_length));
        memcpy(buffer.data() + 0x04, &wire.header.flags, sizeof(wire.header.flags));
        memcpy(buffer.data() + 0x08, &wire.header.ttl, sizeof(wire.header.ttl));
        memcpy(buffer.data() + 0x0C, &wire.header.reserved, sizeof(wire.header.reserved));
        memcpy(buffer.data() + 0x10, &wire.header.rank, sizeof(wire.header.rank));
        memcpy(buffer.data() + 0x18, wire.data.data(), wire.header.data_length);
    }

    void copy_rpc_record_to_bytes(std::array<std::byte, rpc_proc3_record_wire_layout::marshaled_size>& buffer,
                                  const rpc_proc3_record_wire_layout& wire)
    {
        memcpy(buffer.data() + 0x00, &wire.header.type, sizeof(wire.header.type));
        memcpy(buffer.data() + 0x02, &wire.header.data_length, sizeof(wire.header.data_length));
        memcpy(buffer.data() + 0x04, &wire.header.flags, sizeof(wire.header.flags));
        memcpy(buffer.data() + 0x08, &wire.header.ttl, sizeof(wire.header.ttl));
        memcpy(buffer.data() + 0x0C, &wire.header.reserved, sizeof(wire.header.reserved));
        memcpy(buffer.data() + 0x10, &wire.header.rank, sizeof(wire.header.rank));
        memcpy(buffer.data() + 0x18, wire.data.data(), wire.header.data_length);
    }

    template <typename Traits>
    void write_proc2_marshaled_record(utils::aligned_binary_writer<Traits>& writer, const rpc_proc2_record_semantics& semantic_record)
    {
        const auto& record = *semantic_record.record;

        rpc_proc2_record_wire_layout wire{};
        wire.header =
            build_rpc_dns_record_header(record, semantic_record.is_owner_name_record ? record.flags : k_dns_record_flags_without_name,
                                        k_native_record_rank, record.reserved);
        wire.data = record.data;

        std::array<std::byte, rpc_proc2_record_wire_layout::max_marshaled_size> buffer{};
        copy_rpc_record_to_bytes(buffer, wire);

        const auto wire_size = semantic_record.uses_embedded_owner_name ? rpc_proc2_record_wire_layout::record_with_owner_name_size
                                                                        : rpc_proc2_record_wire_layout::chained_record_size;
        writer.write(buffer.data(), wire_size);
    }

    template <typename Traits>
    void write_proc2_record_chain(utils::aligned_binary_writer<Traits>& writer, const std::vector<rpc_proc2_record_semantics>& record_chain)
    {
        for (const auto& semantic_record : record_chain)
        {
            write_proc2_marshaled_record(writer, semantic_record);
        }
    }

    template <typename Traits>
    void write_proc2_query_response(utils::aligned_binary_writer<Traits>& writer, const dns_query_request& request,
                                    const std::vector<resolved_dns_record>& records, const DWORD status)
    {
        const auto start = writer.offset();
        const auto record_chain = build_proc2_rpc_record_chain(records);
        const auto shape = describe_proc2_response_shape(record_chain, status);

        writer.write(request.rpc_query_options);

        write_proc2_result_record_list_pointer(writer, shape.has_result_record_list);
        write_proc2_alias_record_list_pointer(writer, shape.has_alias_record_list);
        write_proc2_name_owner_pointer(writer, shape.has_owner_name_anchor);

        if (shape.has_result_record_list)
        {
            write_proc2_record_chain(writer, record_chain);
            writer.write_ndr_u16string(records.front().name);
        }

        if (writer.offset() < start + shape.payload_size)
        {
            writer.pad(static_cast<size_t>(start + shape.payload_size - writer.offset()));
        }

        (void)request;
    }

    template <typename Traits>
    void write_proc3_record(utils::aligned_binary_writer<Traits>& writer, const resolved_dns_record& record, const DWORD flags,
                            const DWORD reserved)
    {
        rpc_proc3_record_wire_layout wire{};
        wire.header = build_rpc_dns_record_header(record, flags, static_cast<uint64_t>(record.type), reserved);
        wire.data = record.data;

        std::array<std::byte, rpc_proc3_record_wire_layout::marshaled_size> buffer{};
        copy_rpc_record_to_bytes(buffer, wire);
        writer.write(buffer);
    }

    template <typename Traits>
    void write_proc3_response(utils::aligned_binary_writer<Traits>& writer, const dns_query_request& request,
                              const std::vector<resolved_dns_record>& direct_records,
                              const std::vector<resolved_dns_record>& mapped_records, const DWORD status)
    {
        writer.write(request.rpc_query_options);

        if (status != ERROR_SUCCESS || (direct_records.empty() && mapped_records.empty()))
        {
            write_rpc_pointer(writer, false);
            write_rpc_pointer(writer, false);
            write_rpc_pointer(writer, false);
            return;
        }

        write_rpc_pointer(writer, true);
        write_rpc_pointer(writer, true);
        write_rpc_pointer(writer, true);

        if (!direct_records.empty())
        {
            write_proc3_record(writer, direct_records[0], k_dns_record_flags, 1);
        }

        if (direct_records.size() > 1)
        {
            write_rpc_pointer(writer, true);
            write_rpc_pointer(writer, false);
            write_proc3_record(writer, direct_records[1], k_dns_record_flags_without_name, 1);
        }

        if (!mapped_records.empty())
        {
            write_rpc_pointer(writer, true);
            write_rpc_pointer(writer, true);
            write_proc3_record(writer, mapped_records[0], k_dns_record_flags, 0);
        }

        if (mapped_records.size() > 1)
        {
            write_rpc_pointer(writer, false);
            write_rpc_pointer(writer, true);
            write_proc3_record(writer, mapped_records[1], k_dns_record_flags, 0);
        }

        const auto& owner_name = direct_records.empty() ? mapped_records.front().name : direct_records.front().name;
        writer.write_ndr_u16string(owner_name);

        if (!mapped_records.empty())
        {
            writer.write_ndr_u16string(mapped_records[0].name);
        }

        if (mapped_records.size() > 1)
        {
            writer.write_ndr_u16string(mapped_records[1].name);
        }

        constexpr size_t k_native_proc3_reply_payload_size = 0x1E0;
        if (writer.offset() < k_native_proc3_reply_payload_size)
        {
            writer.pad(static_cast<size_t>(k_native_proc3_reply_payload_size - writer.offset()));
        }
    }

    bool parse_dns_query_request(windows_emulator& win_emu, const lpc_request_context& c, dns_query_request& request)
    {
        constexpr ULONG hostname_header_offset = 0x08;
        constexpr ULONG hostname_offset = 0x20;
        constexpr ULONG fixed_trailer_size = 8; // query type + padding + flags
        auto& emu = win_emu.emu();

        if (c.send_buffer_length < hostname_offset + sizeof(char16_t) + fixed_trailer_size)
        {
            return false;
        }

        const auto hostname_length = static_cast<size_t>(emu.read_memory<uint64_t>(c.send_buffer + hostname_header_offset));
        const auto hostname_actual_length = static_cast<size_t>(emu.read_memory<uint64_t>(c.send_buffer + 0x18));
        if (hostname_length == 0 || hostname_actual_length != hostname_length)
        {
            return false;
        }

        const auto hostname_bytes = hostname_length * sizeof(char16_t);
        if (hostname_offset + hostname_bytes + fixed_trailer_size > c.send_buffer_length)
        {
            return false;
        }

        std::u16string encoded_hostname(hostname_length, u'\0');
        emu.read_memory(c.send_buffer + hostname_offset, encoded_hostname.data(), hostname_bytes);
        if (encoded_hostname.back() != u'\0')
        {
            return false;
        }

        request.hostname.assign(encoded_hostname.begin(), encoded_hostname.end() - 1);
        const auto query_type_offset = c.send_buffer + hostname_offset + hostname_bytes;
        request.type = emu.read_memory<uint16_t>(query_type_offset);
        request.flags = emu.read_memory<uint32_t>(query_type_offset + sizeof(uint32_t));
        request.rpc_query_options = emu.read_memory<uint64_t>(c.send_buffer + c.send_buffer_length - sizeof(uint64_t));
        return true;
    }

    std::vector<resolved_dns_record> resolve_host_addresses(windows_emulator& win_emu, const std::u16string& host, const WORD dns_type)
    {
        const auto family = dns_type == DNS_TYPE_A ? AF_INET : AF_INET6;
        const auto results = win_emu.dns_lookup().resolve_host(u16_to_u8(host), family);

        std::vector<resolved_dns_record> records;
        for (const auto& current : results)
        {
            resolved_dns_record record{};
            record.name = host;

            if (current.is_ipv4() && dns_type == DNS_TYPE_A)
            {
                const auto& ipv4 = current.get_in_addr();
                record.type = DNS_TYPE_A;
                record.data_length = sizeof(ipv4.sin_addr);
                memcpy(record.data.data(), &ipv4.sin_addr, sizeof(ipv4.sin_addr));
            }
            else if (current.is_ipv6() && dns_type == DNS_TYPE_AAAA)
            {
                const auto& ipv6 = current.get_in6_addr();
                record.type = DNS_TYPE_AAAA;
                record.data_length = sizeof(ipv6.sin6_addr);
                memcpy(record.data.data(), &ipv6.sin6_addr, sizeof(ipv6.sin6_addr));
            }
            else
            {
                continue;
            }

            const auto is_duplicate = std::ranges::any_of(records, [&record](const resolved_dns_record& existing) {
                return existing.type == record.type && existing.data == record.data;
            });
            if (!is_duplicate)
            {
                records.push_back(std::move(record));
            }
        }

        std::ranges::sort(records, [](const resolved_dns_record& a, const resolved_dns_record& b) {
            return std::ranges::lexicographical_compare(b.data, a.data);
        });

        return records;
    }

    std::vector<resolved_dns_record> build_proc3_mapped_records(windows_emulator& win_emu, const std::u16string& host)
    {
        const auto ipv4_records = resolve_host_addresses(win_emu, host, DNS_TYPE_A);
        std::vector<resolved_dns_record> mapped_records;
        mapped_records.reserve(ipv4_records.size());

        for (const auto& ipv4_record : ipv4_records)
        {
            resolved_dns_record mapped{};
            mapped.name = host;
            mapped.type = DNS_TYPE_AAAA;
            mapped.data_length = 16;
            mapped.ttl = k_proc3_default_ttl;
            mapped.reserved = 0;
            mapped.data.fill(std::byte{0});
            mapped.data[10] = std::byte{0xFF};
            mapped.data[11] = std::byte{0xFF};
            memcpy(mapped.data.data() + 12, ipv4_record.data.data(), 4);
            mapped_records.push_back(mapped);
        }

        std::ranges::sort(mapped_records, [](const resolved_dns_record& a, const resolved_dns_record& b) {
            return std::ranges::lexicographical_compare(a.data, b.data);
        });

        return mapped_records;
    }

    struct dns_resolver : rpc_port
    {
        NTSTATUS handle_rpc(windows_emulator& win_emu, const uint32_t procedure_id, const lpc_request_context& c) override
        {
            utils::aligned_binary_writer<EmulatorTraits<Emu64>> writer(win_emu.emu(), c.recv_buffer);

            switch (procedure_id)
            {
            case 2:
                return handle_dns_query(win_emu, c, writer);
            case 3:
                return handle_dns_addrinfo_query(win_emu, c, writer);
            default:
                win_emu.log.print(color::gray, "Unexpected DNSResolver procedure: %u\n", procedure_id);
                return STATUS_NOT_SUPPORTED;
            }
        }

        template <typename Traits>
        static NTSTATUS handle_dns_query(windows_emulator& win_emu, const lpc_request_context& c,
                                         utils::aligned_binary_writer<Traits>& writer)
        {
            dns_query_request request{};
            if (!parse_dns_query_request(win_emu, c, request))
            {
                return STATUS_INVALID_PARAMETER;
            }

            const auto hostname = u16_to_u8(request.hostname);
            win_emu.callbacks.on_generic_activity("DNS query: " + hostname);

            switch (classify_proc2_query_family(request))
            {
            case proc2_query_family::record_query: {
                auto records = resolve_host_addresses(win_emu, request.hostname, DNS_TYPE_A);
                if (records.size() > 2)
                {
                    records.resize(2);
                }

                if (records.size() == 1)
                {
                    records[0].ttl = k_proc2_single_record_a_ttl;
                }

                const DWORD status = records.empty() ? DNS_ERROR_RCODE_NAME_ERROR : ERROR_SUCCESS;
                write_proc2_query_response(writer, request, records, status);
                break;
            }
            case proc2_query_family::addrinfo_compatible_query: {
                auto direct_records = resolve_host_addresses(win_emu, request.hostname, DNS_TYPE_AAAA);
                auto mapped_records = build_proc3_mapped_records(win_emu, request.hostname);

                for (auto& record : direct_records)
                {
                    record.ttl = k_proc2_aaaa_direct_ttl;
                    record.reserved = 1;
                }

                if (direct_records.size() > 2)
                {
                    direct_records.resize(2);
                }
                if (mapped_records.size() > 2)
                {
                    mapped_records.resize(2);
                }

                const DWORD status = direct_records.empty() && mapped_records.empty() ? DNS_ERROR_RCODE_NAME_ERROR : ERROR_SUCCESS;
                write_proc3_response(writer, request, direct_records, mapped_records, status);
                break;
            }
            case proc2_query_family::unsupported:
                write_proc2_query_response(writer, request, {}, STATUS_INVALID_PARAMETER);
                break;
            }
            c.recv_buffer_length = static_cast<ULONG>(writer.offset());
            return STATUS_SUCCESS;
        }

        template <typename Traits>
        static NTSTATUS handle_dns_addrinfo_query(windows_emulator& win_emu, const lpc_request_context& c,
                                                  utils::aligned_binary_writer<Traits>& writer)
        {
            dns_query_request request{};
            if (!parse_dns_query_request(win_emu, c, request))
            {
                return STATUS_INVALID_PARAMETER;
            }

            const auto hostname = u16_to_u8(request.hostname);
            win_emu.callbacks.on_generic_activity("DNS addrinfo query: " + hostname);

            auto direct_records = resolve_host_addresses(win_emu, request.hostname, DNS_TYPE_AAAA);
            auto mapped_records = build_proc3_mapped_records(win_emu, request.hostname);

            std::ranges::sort(direct_records, [](const resolved_dns_record& a, const resolved_dns_record& b) {
                return std::ranges::lexicographical_compare(a.data, b.data);
            });

            for (auto& record : direct_records)
            {
                record.ttl = k_proc3_default_ttl;
                record.reserved = 1;
            }

            if (direct_records.size() > 2)
            {
                direct_records.resize(2);
            }
            if (mapped_records.size() > 2)
            {
                mapped_records.resize(2);
            }

            const DWORD status = direct_records.empty() && mapped_records.empty() ? DNS_ERROR_RCODE_NAME_ERROR : ERROR_SUCCESS;
            write_proc3_response(writer, request, direct_records, mapped_records, status);
            c.recv_buffer_length = static_cast<ULONG>(writer.offset());
            return STATUS_SUCCESS;
        }
    };
}

std::unique_ptr<port> create_dns_resolver()
{
    return std::make_unique<dns_resolver>();
}
