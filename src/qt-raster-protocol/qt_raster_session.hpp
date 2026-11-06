#pragma once

#include "qt_raster_protocol.hpp"

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace sogen::qt_raster
{
    enum class result
    {
        success,
        invalid_parameter,
        buffer_too_small,
        access_violation,
        resource_limit,
    };

    struct frame
    {
        uint32_t window{};
        int32_t width{};
        int32_t height{};
        int32_t stride{};
        std::shared_ptr<const std::vector<uint8_t>> pixels{};
    };

    struct window
    {
        std::array<int32_t, 4> geometry{};
        uint32_t parent{};
        int32_t type{};
        int32_t modality{};
        bool visible{};
        bool raised{};
        bool focused{};
        int32_t cursor{};
        uint32_t visibility_order{};
        uint32_t stacking_order{};
        uint32_t focus_order{};
        std::string title{};
        std::optional<frame> surface{};
    };

    class session
    {
      public:
        using reply_writer = std::function<bool(std::span<const uint8_t>)>;

        session() = default;
        session(const session&) = delete;
        session& operator=(const session&) = delete;
        session(session&&) = delete;
        session& operator=(session&&) = delete;

        void reset();

        result exchange(std::span<const uint8_t> request, size_t capacity, const reply_writer& write_reply);
        result exchange(const Packet& packet, std::vector<uint8_t> storage, size_t capacity, const reply_writer& write_reply);
        bool enqueue_input(const Packet& event, std::span<const uint8_t> payload = {});

        bool active() const;
        uint64_t identity() const;
        const std::map<uint32_t, window>& windows() const;
        const std::optional<frame>& latest_frame() const;
        uint64_t frame_count() const;

      private:
        struct input
        {
            Packet packet{};
            std::vector<uint8_t> payload{};
        };

        uint64_t identity_{};
        uint32_t sequence_{};
        std::map<uint32_t, window> windows_{};
        std::deque<input> input_{};
        size_t frame_bytes_{};
        std::optional<frame> latest_frame_{};
        uint64_t frame_count_{};
    };
}
