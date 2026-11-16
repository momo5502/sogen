#pragma once
#include "process_protocol.hpp"
#include <network/tcp_client_socket.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <optional>
#include <random>
#include <string>

namespace sogen::process_protocol
{
    constexpr uint32_t maximum_payload_size = 64U << 20;
    constexpr auto response_timeout = std::chrono::seconds(5);

    template <typename T>
    bool send_message(network::tcp_client_socket& socket, T payload,
                      const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max(),
                      const std::atomic_bool* stopping = nullptr)
    {
        ProcessProtocol::MessageT message{};
        message.payload.Set(std::move(payload));

        flatbuffers::FlatBufferBuilder builder{};
        ProcessProtocol::FinishSizePrefixedMessageBuffer(builder, ProcessProtocol::Message::Pack(builder, &message));
        if (builder.GetSize() > maximum_payload_size + sizeof(flatbuffers::uoffset_t))
        {
            return false;
        }
        return socket.send(std::string_view{reinterpret_cast<const char*>(builder.GetBufferPointer()), builder.GetSize()}, deadline,
                           stopping);
    }

    inline std::optional<std::string> receive_exact(network::tcp_client_socket& socket, const size_t size)
    {
        std::string data{};
        data.reserve(size);
        while (data.size() < size)
        {
            auto part = socket.receive(size - data.size());
            if (!part)
            {
                return std::nullopt;
            }
            data.append(*part);
        }
        return data;
    }

    inline std::optional<std::string> receive_exact_until(network::tcp_client_socket& socket, const size_t size,
                                                          const std::chrono::steady_clock::time_point deadline,
                                                          const std::atomic_bool* stopping = nullptr)
    {
        std::string data{};
        data.reserve(size);
        while (data.size() < size)
        {
            if ((stopping && *stopping) || std::chrono::steady_clock::now() >= deadline)
            {
                return std::nullopt;
            }
            if (!socket.is_ready(true))
            {
                socket.sleep(std::chrono::milliseconds(10), true);
                continue;
            }

            auto part = socket.receive(size - data.size());
            if (!part)
            {
                return std::nullopt;
            }
            data.append(*part);
        }
        return data;
    }

    inline std::optional<ProcessProtocol::MessageT> decode_message(std::string data)
    {
        flatbuffers::Verifier verifier{reinterpret_cast<const uint8_t*>(data.data()), data.size()};
        if (!ProcessProtocol::VerifySizePrefixedMessageBuffer(verifier))
        {
            return std::nullopt;
        }

        ProcessProtocol::MessageT message{};
        ProcessProtocol::GetSizePrefixedMessage(data.data())->UnPackTo(&message);
        return message;
    }

    inline std::optional<ProcessProtocol::MessageT> receive_message(network::tcp_client_socket& socket)
    {
        auto size_data = receive_exact(socket, sizeof(flatbuffers::uoffset_t));
        if (!size_data)
        {
            return std::nullopt;
        }

        const auto size = flatbuffers::ReadScalar<flatbuffers::uoffset_t>(size_data->data());
        if (size > maximum_payload_size)
        {
            return std::nullopt;
        }

        auto data = receive_exact(socket, size);
        if (!data)
        {
            return std::nullopt;
        }
        size_data->append(*data);
        return decode_message(std::move(*size_data));
    }

    inline std::optional<ProcessProtocol::MessageT> receive_message_until(network::tcp_client_socket& socket,
                                                                          const std::chrono::steady_clock::time_point deadline,
                                                                          const std::atomic_bool* stopping = nullptr)
    {
        auto size_data = receive_exact_until(socket, sizeof(flatbuffers::uoffset_t), deadline, stopping);
        if (!size_data)
        {
            return std::nullopt;
        }

        const auto size = flatbuffers::ReadScalar<flatbuffers::uoffset_t>(size_data->data());
        if (size > maximum_payload_size)
        {
            return std::nullopt;
        }

        auto data = receive_exact_until(socket, size, deadline, stopping);
        if (!data)
        {
            return std::nullopt;
        }
        size_data->append(*data);
        return decode_message(std::move(*size_data));
    }

    inline std::string make_token()
    {
        std::random_device random{};
        constexpr std::array digits{'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
        std::string token{};
        token.reserve(32);
        for (size_t i = 0; i < 4; ++i)
        {
            auto value = random();
            for (size_t digit = 0; digit < 8; ++digit)
            {
                token.push_back(digits[value & 0xF]);
                value >>= 4;
            }
        }
        return token;
    }

}
