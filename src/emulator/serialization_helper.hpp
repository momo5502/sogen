#pragma once

#include "serialization.hpp"

#include <chrono>
#include <filesystem>
#include <utils/path_key.hpp>

namespace utils
{
    inline void serialize(buffer_serializer& buffer, const std::chrono::steady_clock::time_point& tp)
    {
        buffer.write(tp.time_since_epoch().count());
    }

    inline void deserialize(buffer_deserializer& buffer, std::chrono::steady_clock::time_point& tp)
    {
        using time_point = std::chrono::steady_clock::time_point;
        using duration = time_point::duration;

        const auto count = buffer.read<duration::rep>();
        tp = time_point{duration{count}};
    }

    inline void serialize(buffer_serializer& buffer, const std::chrono::system_clock::time_point& tp)
    {
        buffer.write(tp.time_since_epoch().count());
    }

    inline void deserialize(buffer_deserializer& buffer, std::chrono::system_clock::time_point& tp)
    {
        using time_point = std::chrono::system_clock::time_point;
        using duration = time_point::duration;

        const auto count = buffer.read<duration::rep>();
        tp = time_point{duration{count}};
    }

    inline void serialize(buffer_serializer& buffer, const std::filesystem::path& path)
    {
        buffer.write_string<char16_t>(path.u16string());
    }

    inline void deserialize(buffer_deserializer& buffer, std::filesystem::path& path)
    {
        path = buffer.read_string<char16_t>();
    }

    inline void serialize(buffer_serializer& buffer, const path_key& path)
    {
        buffer.write(path.get());
    }

    inline void deserialize(buffer_deserializer& buffer, path_key& path)
    {
        path = buffer.read<std::filesystem::path>();
    }
}
