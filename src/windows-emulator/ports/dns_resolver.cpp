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

    template <typename Traits>
    void write_marshaled_dns_record(utils::aligned_binary_writer<Traits>& writer, const resolved_dns_record& record,
                                    const bool include_name)
    {
        constexpr size_t first_record_wire_size = 0x30;
        constexpr size_t chained_record_wire_size = 0x20;

        std::array<std::byte, first_record_wire_size> buffer{};
        memcpy(buffer.data() + 0x00, &record.type, sizeof(record.type));
        memcpy(buffer.data() + 0x02, &record.data_length, sizeof(record.data_length));

        const auto wire_flags = include_name ? record.flags : k_dns_record_flags_without_name;
        memcpy(buffer.data() + 0x04, &wire_flags, sizeof(wire_flags));
        memcpy(buffer.data() + 0x08, &record.ttl, sizeof(record.ttl));
        memcpy(buffer.data() + 0x0C, &record.reserved, sizeof(record.reserved));

        const auto rank = k_native_record_rank;
        memcpy(buffer.data() + 0x10, &rank, sizeof(rank));
        memcpy(buffer.data() + 0x18, record.data.data(), record.data_length);

        const auto wire_size = include_name ? first_record_wire_size : chained_record_wire_size;
        writer.write(buffer.data(), wire_size);
    }

    template <typename Traits>
    void write_dns_query_response(utils::aligned_binary_writer<Traits>& writer, const dns_query_request& request,
                                  const std::vector<resolved_dns_record>& records, const DWORD status)
    {
        const auto start = writer.offset();

        writer.write(request.rpc_query_options);

        if (status == ERROR_SUCCESS && !records.empty())
        {
            writer.template write<typename Traits::PVOID>(k_native_pointer_referent);
            writer.template write<typename Traits::PVOID>(k_native_pointer_referent);
            writer.template write<typename Traits::PVOID>(k_native_pointer_referent);

            for (size_t index = 0; index < records.size(); ++index)
            {
                write_marshaled_dns_record(writer, records[index], index == 0);
            }

            writer.write_ndr_u16string(records.front().name);
        }
        else
        {
            writer.template write<typename Traits::PVOID>(0);
            writer.template write<typename Traits::PVOID>(0);
            writer.template write<typename Traits::PVOID>(0);
        }

        constexpr size_t k_native_query_reply_payload_size = 0x118;
        if (writer.offset() < start + k_native_query_reply_payload_size)
        {
            writer.pad(start + k_native_query_reply_payload_size - writer.offset());
        }

        (void)start;
        (void)request;
        (void)status;
    }

    template <typename Traits>
    void write_proc3_record(utils::aligned_binary_writer<Traits>& writer, const resolved_dns_record& record, const DWORD flags,
                            const DWORD reserved)
    {
        std::array<std::byte, 0x28> buffer{};
        memcpy(buffer.data() + 0x00, &record.type, sizeof(record.type));
        memcpy(buffer.data() + 0x02, &record.data_length, sizeof(record.data_length));
        memcpy(buffer.data() + 0x04, &flags, sizeof(flags));
        memcpy(buffer.data() + 0x08, &record.ttl, sizeof(record.ttl));
        memcpy(buffer.data() + 0x0C, &reserved, sizeof(reserved));

        const auto rank = static_cast<uint64_t>(record.type);
        memcpy(buffer.data() + 0x10, &rank, sizeof(rank));
        memcpy(buffer.data() + 0x18, record.data.data(), record.data_length);
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
            writer.template write<typename Traits::PVOID>(0);
            writer.template write<typename Traits::PVOID>(0);
            writer.template write<typename Traits::PVOID>(0);
            return;
        }

        writer.template write<typename Traits::PVOID>(k_native_pointer_referent);
        writer.template write<typename Traits::PVOID>(k_native_pointer_referent);
        writer.template write<typename Traits::PVOID>(k_native_pointer_referent);

        if (!direct_records.empty())
        {
            write_proc3_record(writer, direct_records[0], k_dns_record_flags, 1);
        }

        if (direct_records.size() > 1)
        {
            writer.template write<typename Traits::PVOID>(k_native_pointer_referent);
            writer.template write<typename Traits::PVOID>(0);
            write_proc3_record(writer, direct_records[1], k_dns_record_flags_without_name, 1);
        }

        if (!mapped_records.empty())
        {
            writer.template write<typename Traits::PVOID>(k_native_pointer_referent);
            writer.template write<typename Traits::PVOID>(k_native_pointer_referent);
            write_proc3_record(writer, mapped_records[0], k_dns_record_flags, 0);
        }

        if (mapped_records.size() > 1)
        {
            writer.template write<typename Traits::PVOID>(0);
            writer.template write<typename Traits::PVOID>(k_native_pointer_referent);
            write_proc3_record(writer, mapped_records[1], k_dns_record_flags, 0);
        }

        writer.write_ndr_u16string(direct_records.empty() ? mapped_records.front().name : direct_records.front().name);
        writer.template write<typename Traits::SIZE_T>(12);

        constexpr size_t k_native_proc3_reply_payload_size = 0x128;
        if (writer.offset() < k_native_proc3_reply_payload_size)
        {
            writer.pad(k_native_proc3_reply_payload_size - writer.offset());
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

    std::vector<resolved_dns_record> resolve_host_addresses(const std::u16string& host, const WORD dns_type)
    {
        addrinfo hints{};
        hints.ai_family = dns_type == DNS_TYPE_A ? AF_INET : AF_INET6;

        addrinfo* results = nullptr;
        const auto status = getaddrinfo(u16_to_u8(host).c_str(), nullptr, &hints, &results);
        if (status != 0 || !results)
        {
            return {};
        }

        std::vector<resolved_dns_record> records;
        for (const auto* current = results; current != nullptr; current = current->ai_next)
        {
            resolved_dns_record record{};
            record.name = host;

            if (current->ai_family == AF_INET && dns_type == DNS_TYPE_A)
            {
                const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(current->ai_addr);
                record.type = DNS_TYPE_A;
                record.data_length = sizeof(ipv4->sin_addr);
                memcpy(record.data.data(), &ipv4->sin_addr, sizeof(ipv4->sin_addr));
            }
            else if (current->ai_family == AF_INET6 && dns_type == DNS_TYPE_AAAA)
            {
                const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(current->ai_addr);
                record.type = DNS_TYPE_AAAA;
                record.data_length = sizeof(ipv6->sin6_addr);
                memcpy(record.data.data(), &ipv6->sin6_addr, sizeof(ipv6->sin6_addr));
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

        freeaddrinfo(results);

        std::ranges::sort(records, [](const resolved_dns_record& a, const resolved_dns_record& b)
        {
            return std::lexicographical_compare(b.data.begin(), b.data.end(), a.data.begin(), a.data.end());
        });

        return records;
    }

    std::vector<resolved_dns_record> build_proc3_mapped_records(const std::u16string& host)
    {
        const auto ipv4_records = resolve_host_addresses(host, DNS_TYPE_A);
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

        std::ranges::sort(mapped_records, [](const resolved_dns_record& a, const resolved_dns_record& b)
        {
            return std::lexicographical_compare(b.data.begin(), b.data.end(), a.data.begin(), a.data.end());
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
                win_emu.log.warn("[dns] failed to parse DNS query request (len=0x%X)\n", c.send_buffer_length);
                return STATUS_INVALID_PARAMETER;
            }

            const auto hostname = u16_to_u8(request.hostname);
            win_emu.callbacks.on_generic_activity("DNS query: " + hostname);

            DWORD status = ERROR_SUCCESS;
            std::vector<resolved_dns_record> records;
            switch (request.type)
            {
            case DNS_TYPE_A:
            case DNS_TYPE_AAAA:
                records = resolve_host_addresses(request.hostname, request.type);
                if (records.size() > 2)
                {
                    records.resize(2);
                }
                status = records.empty() ? DNS_ERROR_RCODE_NAME_ERROR : ERROR_SUCCESS;
                break;
            default:
                status = ERROR_INVALID_PARAMETER;
                break;
            }

            write_dns_query_response(writer, request, records, status);
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
                win_emu.log.warn("[dns] failed to parse proc-3 DNS query request (len=0x%X)\n", c.send_buffer_length);
                return STATUS_INVALID_PARAMETER;
            }

            const auto hostname = u16_to_u8(request.hostname);
            win_emu.callbacks.on_generic_activity("DNS addrinfo query: " + hostname);

            auto direct_records = resolve_host_addresses(request.hostname, DNS_TYPE_AAAA);
            auto mapped_records = build_proc3_mapped_records(request.hostname);

            std::ranges::sort(direct_records, [](const resolved_dns_record& a, const resolved_dns_record& b)
            {
                return std::lexicographical_compare(a.data.begin(), a.data.end(), b.data.begin(), b.data.end());
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
