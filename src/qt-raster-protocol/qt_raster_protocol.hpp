#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace sogen::qt_raster
{
    constexpr uint32_t magic = 0x51425231;
    constexpr uint32_t version = 2;
    constexpr uint32_t ioctl = 0x222000;
    constexpr uint32_t max_payload = 64 * 1024 * 1024;
    constexpr uint32_t max_input_payload = 4096 - 64;
    constexpr uint32_t max_windows = 64;
    constexpr uint32_t max_events = 256;

    enum class Op : uint32_t
    {
        Empty = 0,
        Hello = 1,
        Create,
        Geometry,
        Visible,
        Destroy,
        Frame,
        Title,
        Raise,
        Poll,
        End,
        Mouse = 20,
        Key,
        Focus,
        Close,
        Resize,
        Wheel,
        Screen,
        Ack,
        Cursor,
    };

    using enum Op;

    struct Packet
    {
        uint32_t magic_value = magic;
        uint32_t protocol_version = version;
        Op op{};
        uint32_t id{};
        uint32_t seq{};
        uint32_t size{};
        std::array<int32_t, 8> a{};
        uint64_t session{};
    };

    static_assert(sizeof(Packet) == 64 && offsetof(Packet, session) == 56);
    static_assert(std::endian::native == std::endian::little);

    inline bool valid(const Packet& packet, const size_t bytes)
    {
        return packet.magic_value == magic && packet.protocol_version == version && packet.size <= max_payload &&
               bytes == sizeof(Packet) + static_cast<size_t>(packet.size);
    }

    inline bool valid_geometry(const Packet& packet)
    {
        return packet.a[2] > 0 && packet.a[2] <= 4096 && packet.a[3] > 0 && packet.a[3] <= 4096 &&
               int64_t{packet.a[0]} + packet.a[2] <= INT32_MAX && int64_t{packet.a[1]} + packet.a[3] <= INT32_MAX;
    }

    inline bool valid_frame(const Packet& packet)
    {
        return packet.a[0] > 0 && packet.a[0] <= 4096 && packet.a[1] > 0 && packet.a[1] <= 4096 && packet.a[2] == packet.a[0] * 4 &&
               uint64_t{static_cast<uint32_t>(packet.a[2])} * static_cast<uint32_t>(packet.a[1]) == packet.size && packet.a[3] >= 500 &&
               packet.a[3] <= 4000 && packet.a[4] >= 0 && packet.a[5] >= 0 && packet.a[6] >= 0 && packet.a[7] >= 0 &&
               int64_t{packet.a[4]} + packet.a[6] <= packet.a[0] && int64_t{packet.a[5]} + packet.a[7] <= packet.a[1];
    }
}
