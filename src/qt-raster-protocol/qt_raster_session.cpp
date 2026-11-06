#include "qt_raster_session.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>

namespace sogen::qt_raster
{
    namespace
    {
        uint64_t next_identity()
        {
            static std::atomic<uint64_t> next{1};
            auto value = next.load();
            while (value != std::numeric_limits<uint64_t>::max())
            {
                if (next.compare_exchange_weak(value, value + 1))
                {
                    return value;
                }
            }
            return 0;
        }

        bool valid_text(std::span<const uint8_t> text)
        {
            size_t i{};
            while (i < text.size())
            {
                const auto first = text[i++];
                if (first < 0x80)
                {
                    continue;
                }
                size_t count = 3;
                if (first <= 0xDF)
                {
                    count = 1;
                }
                else if (first <= 0xEF)
                {
                    count = 2;
                }
                if (first < 0xC2 || first > 0xF4 || count > text.size() - i)
                {
                    return false;
                }
                if ((first == 0xE0 && text[i] < 0xA0) || (first == 0xED && text[i] >= 0xA0) || (first == 0xF0 && text[i] < 0x90) ||
                    (first == 0xF4 && text[i] >= 0x90))
                {
                    return false;
                }
                for (size_t n = 0; n < count; ++n)
                {
                    if ((text[i++] & 0xC0) != 0x80)
                    {
                        return false;
                    }
                }
            }
            return true;
        }
    }

    result session::exchange(const std::span<const uint8_t> request, const size_t capacity, const reply_writer& write_reply)
    {
        if (request.size() < sizeof(Packet))
        {
            return result::invalid_parameter;
        }
        Packet packet{};
        std::memcpy(&packet, request.data(), sizeof(packet));
        if (!valid(packet, request.size()))
        {
            return result::invalid_parameter;
        }
        return this->exchange(packet, {request.begin() + sizeof(Packet), request.end()}, capacity, write_reply);
    }

    result session::exchange(const Packet& packet, std::vector<uint8_t> storage, const size_t capacity, const reply_writer& write_reply)
    {
        if (!valid(packet, sizeof(Packet) + storage.size()) || packet.seq == 0)
        {
            return result::invalid_parameter;
        }
        const std::span<const uint8_t> payload{storage};
        const bool hello = packet.op == Hello;
        if (hello)
        {
            if (packet.session != 0 || packet.seq != 1 || packet.id != 0 || !payload.empty() || !this->windows_.empty() ||
                !this->input_.empty())
            {
                return result::invalid_parameter;
            }
        }
        else if (!this->active() || packet.session != this->identity_ || packet.seq <= this->sequence_)
        {
            return result::invalid_parameter;
        }
        if (packet.op != Create && packet.op != Title && packet.op != Frame && !payload.empty())
        {
            return result::invalid_parameter;
        }
        if (capacity < sizeof(Packet))
        {
            return result::buffer_too_small;
        }

        Packet reply{};
        reply.op = Ack;
        reply.seq = packet.seq;
        reply.session = this->identity_;
        std::span<const uint8_t> reply_payload{};
        std::function<void()> commit = [] {};
        const auto entry = this->windows_.find(packet.id);
        if (packet.op != Hello && packet.op != Poll && packet.op != End && entry == this->windows_.end() && packet.op != Create)
        {
            return result::invalid_parameter;
        }

        switch (packet.op)
        {
        case Hello: {
            const auto identity = next_identity();
            if (identity == 0)
            {
                return result::resource_limit;
            }
            reply.op = Hello;
            reply.session = identity;
            reply.a[0] = 1280;
            reply.a[1] = 900;
            reply.a[2] = 1000;
            commit = [this, identity] {
                this->identity_ = identity;
                this->latest_frame_.reset();
                this->frame_count_ = 0;
            };
            break;
        }
        case Create: {
            const auto parent = static_cast<uint32_t>(packet.a[6]);
            if (packet.id == 0 || entry != this->windows_.end() || !valid_geometry(packet) || packet.a[4] < 0 || packet.a[5] < 0 ||
                packet.a[5] > 2 || packet.a[7] < 500 || packet.a[7] > 4000 || payload.size() > 4096 || !valid_text(payload) ||
                (parent != 0 && !this->windows_.contains(parent)))
            {
                return result::invalid_parameter;
            }
            if (this->windows_.size() >= max_windows)
            {
                return result::resource_limit;
            }
            window value{};
            std::copy_n(packet.a.begin(), value.geometry.size(), value.geometry.begin());
            value.parent = parent;
            value.type = packet.a[4];
            value.modality = packet.a[5];
            value.title.assign(payload.begin(), payload.end());
            std::map<uint32_t, window> pending;
            pending.emplace(packet.id, std::move(value));
            commit = [this, pending = std::move(pending)]() mutable { this->windows_.merge(pending); };
            break;
        }
        case Geometry:
            if (!valid_geometry(packet))
            {
                return result::invalid_parameter;
            }
            commit = [entry, packet] { std::copy_n(packet.a.begin(), entry->second.geometry.size(), entry->second.geometry.begin()); };
            break;
        case Visible:
            if ((packet.a[0] != 0 && packet.a[0] != 1) || packet.a[1] < 0 || packet.a[1] > 2)
            {
                return result::invalid_parameter;
            }
            commit = [entry, packet] {
                entry->second.visible = packet.a[0] != 0;
                entry->second.modality = packet.a[1];
                entry->second.visibility_order = packet.seq;
                if (entry->second.visible)
                {
                    entry->second.raised = true;
                    entry->second.stacking_order = packet.seq;
                }
            };
            break;
        case Title:
            if (payload.size() > 4096 || !valid_text(payload))
            {
                return result::invalid_parameter;
            }
            commit = [entry, title = std::string(payload.begin(), payload.end())]() mutable { entry->second.title.swap(title); };
            break;
        case Raise:
            if (packet.a[0] != 0 && packet.a[0] != 1)
            {
                return result::invalid_parameter;
            }
            commit = [entry, packet] {
                entry->second.raised = packet.a[0] != 0;
                entry->second.stacking_order = packet.seq;
            };
            break;
        case Focus:
            if (packet.a[0] != 0 && packet.a[0] != 1)
            {
                return result::invalid_parameter;
            }
            commit = [entry, packet] {
                entry->second.focused = packet.a[0] != 0;
                entry->second.focus_order = packet.seq;
            };
            break;
        case Cursor:
            if (packet.a[0] < 0 || packet.a[0] > 24)
            {
                return result::invalid_parameter;
            }
            commit = [entry, packet] { entry->second.cursor = packet.a[0]; };
            break;
        case Destroy:
            if (std::ranges::any_of(this->windows_, [id = packet.id](const auto& item) { return item.second.parent == id; }))
            {
                return result::invalid_parameter;
            }
            commit = [this, entry, id = packet.id] {
                if (entry->second.surface)
                {
                    this->frame_bytes_ -= entry->second.surface->pixels->size();
                }
                this->windows_.erase(entry);
                std::erase_if(this->input_, [id](const auto& input) { return input.packet.id == id; });
            };
            break;
        case Frame: {
            if (!valid_frame(packet))
            {
                return result::invalid_parameter;
            }
            const auto old_size = entry->second.surface ? entry->second.surface->pixels->size() : 0;
            if (payload.size() > max_payload - (this->frame_bytes_ - old_size) ||
                this->frame_count_ == std::numeric_limits<uint64_t>::max())
            {
                return result::resource_limit;
            }
            frame value{.window = packet.id,
                        .width = packet.a[0],
                        .height = packet.a[1],
                        .stride = packet.a[2],
                        .pixels = std::make_shared<const std::vector<uint8_t>>(std::move(storage))};
            commit = [this, entry, old_size, value = std::move(value)] {
                this->frame_bytes_ = this->frame_bytes_ - old_size + value.pixels->size();
                entry->second.surface = value;
                this->latest_frame_ = value;
                ++this->frame_count_;
            };
            break;
        }
        case Poll:
            if (packet.id != 0)
            {
                return result::invalid_parameter;
            }
            reply.op = Op{};
            if (!this->input_.empty())
            {
                reply = this->input_.front().packet;
                reply.seq = packet.seq;
                reply.session = this->identity_;
                const auto& bytes = this->input_.front().payload;
                if (sizeof(reply) + bytes.size() > capacity)
                {
                    return result::buffer_too_small;
                }
                reply_payload = bytes;
                commit = [this] { this->input_.pop_front(); };
            }
            break;
        case End:
            if (packet.id != 0 || !this->windows_.empty() || !this->input_.empty())
            {
                return result::invalid_parameter;
            }
            commit = [this] { this->identity_ = 0; };
            break;
        default:
            return result::invalid_parameter;
        }

        std::vector<uint8_t> response(sizeof(reply) + reply_payload.size());
        std::memcpy(response.data(), &reply, sizeof(reply));
        if (!reply_payload.empty())
        {
            std::memcpy(response.data() + sizeof(reply), reply_payload.data(), reply_payload.size());
        }
        if (!write_reply(response))
        {
            return result::access_violation;
        }
        commit();
        this->sequence_ = this->active() ? packet.seq : 0;
        return result::success;
    }

    bool session::enqueue_input(const Packet& event, const std::span<const uint8_t> payload)
    {
        if (!this->active() || event.session != this->identity_ || payload.size() > max_input_payload || event.size != payload.size() ||
            !valid(event, sizeof(Packet) + payload.size()) || this->input_.size() >= max_events)
        {
            return false;
        }
        if (event.op == Screen)
        {
            if (event.id != 0 || event.a[0] <= 0 || event.a[1] <= 0 || event.a[0] > 16384 || event.a[1] > 16384 || event.a[2] < 500 ||
                event.a[2] > 4000)
            {
                return false;
            }
        }
        else if (event.op < Mouse || event.op > Wheel || !this->windows_.contains(event.id))
        {
            return false;
        }
        if (event.op == Key)
        {
            if ((event.a[0] != 6 && event.a[0] != 7) || !valid_text(payload))
            {
                return false;
            }
        }
        else if (!payload.empty())
        {
            return false;
        }
        this->input_.push_back({.packet = event, .payload = {payload.begin(), payload.end()}});
        return true;
    }

    void session::reset()
    {
        this->identity_ = 0;
        this->sequence_ = 0;
        this->windows_.clear();
        this->input_.clear();
        this->frame_bytes_ = 0;
        this->latest_frame_.reset();
        this->frame_count_ = 0;
    }

    bool session::active() const
    {
        return this->identity_ != 0;
    }

    uint64_t session::identity() const
    {
        return this->identity_;
    }

    const std::map<uint32_t, window>& session::windows() const
    {
        return this->windows_;
    }

    const std::optional<frame>& session::latest_frame() const
    {
        return this->latest_frame_;
    }

    uint64_t session::frame_count() const
    {
        return this->frame_count_;
    }
}
