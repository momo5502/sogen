#ifndef NOMINMAX
#define NOMINMAX
#endif

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windns.h>

#include <array>
#include <cstdio>
#include <stdexcept>

namespace
{
    class wsa_session
    {
      public:
        wsa_session()
        {
            WSADATA data{};
            if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
            {
                throw std::runtime_error("WSAStartup failed");
            }
        }

        ~wsa_session()
        {
            WSACleanup();
        }
    };

    int print_dnsquery_results(const char* hostname)
    {
        PDNS_RECORDA records = nullptr;
        const DNS_STATUS status = DnsQuery_A(hostname, DNS_TYPE_A, DNS_QUERY_STANDARD, nullptr, &records, nullptr);
        if (status != ERROR_SUCCESS)
        {
            std::printf("DnsQuery_A failed: %lu\n", status);
            return 20;
        }

        int count = 0;
        std::array<char, INET_ADDRSTRLEN> buffer{};
        for (auto* current = records; current != nullptr; current = current->pNext)
        {
            if (current->wType != DNS_TYPE_A)
            {
                continue;
            }

            in_addr address{};
            address.S_un.S_addr = current->Data.A.IpAddress;
            if (InetNtopA(AF_INET, &address, buffer.data(), static_cast<DWORD>(buffer.size())) == nullptr)
            {
                std::printf("InetNtopA failed: %d\n", WSAGetLastError());
                DnsRecordListFree(records, DnsFreeRecordList);
                return 21;
            }

            std::printf("dnsquery[%d]=%s\n", count, buffer.data());
            ++count;
        }

        DnsRecordListFree(records, DnsFreeRecordList);

        if (count == 0)
        {
            std::puts("DnsQuery_A returned no A records");
            return 22;
        }

        return 0;
    }

    int print_resolution_results(const char* hostname)
    {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        addrinfo* results = nullptr;
        const int status = getaddrinfo(hostname, "80", &hints, &results);
        if (status != 0)
        {
            std::printf("getaddrinfo failed: %d\n", status);
            return 1;
        }

        int count = 0;
        std::array<char, INET6_ADDRSTRLEN> buffer{};

        for (auto* current = results; current != nullptr; current = current->ai_next)
        {
            const void* address = nullptr;
            if (current->ai_family == AF_INET)
            {
                address = &reinterpret_cast<const sockaddr_in*>(current->ai_addr)->sin_addr;
            }
            else if (current->ai_family == AF_INET6)
            {
                address = &reinterpret_cast<const sockaddr_in6*>(current->ai_addr)->sin6_addr;
            }
            else
            {
                continue;
            }

            if (InetNtopA(current->ai_family, const_cast<void*>(address), buffer.data(), static_cast<DWORD>(buffer.size())) == nullptr)
            {
                std::printf("InetNtopA failed: %d\n", WSAGetLastError());
                freeaddrinfo(results);
                return 2;
            }

            std::printf("resolved[%d]=%s family=%d\n", count, buffer.data(), current->ai_family);
            ++count;
        }

        freeaddrinfo(results);

        if (count == 0)
        {
            std::puts("getaddrinfo returned no usable addresses");
            return 3;
        }

        return 0;
    }
}

int main()
{
    try
    {
        wsa_session session{};
        constexpr auto hostname = "example.com";
        std::printf("resolving %s\n", hostname);

        const int dnsquery_status = print_dnsquery_results(hostname);
        if (dnsquery_status != 0)
        {
            return dnsquery_status;
        }

        return print_resolution_results(hostname);
    }
    catch (const std::exception& e)
    {
        std::printf("fatal: %s\n", e.what());
        return 10;
    }
}
