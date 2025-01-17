#include "stream_processor.hpp"
#include "checksum.hpp"

#include <cassert>
#include <stdexcept>

namespace gdb_stub
{
    namespace
    {
        void trim_data_stream_start(std::string& stream)
        {
            while (!stream.empty() && stream.front() != '$')
            {
                stream.erase(stream.begin());
            }
        }
    }

    bool stream_processor::has_packet() const
    {
        return !this->packets_.empty();
    }

    std::string stream_processor::get_next_packet()
    {
        if (this->packets_.empty())
        {
            throw std::runtime_error("No packet available");
        }

        auto packet = std::move(this->packets_.front());
        this->packets_.pop();

        return packet;
    }

    void stream_processor::push_stream_data(const std::string& data)
    {
        this->stream_.append(data);
        this->process_data_stream();
    }

    void stream_processor::process_data_stream()
    {
        while (true)
        {
            trim_data_stream_start(this->stream_);

            const auto end = this->stream_.find_first_of('#');
            if (end == std::string::npos)
            {
                break;
            }

            const auto packet_size = end + CHECKSUM_SIZE + 1;

            if (packet_size > this->stream_.size())
            {
                break;
            }

            auto packet = this->stream_.substr(0, packet_size);
            this->stream_.erase(0, packet_size);

            this->enqueue_packet(std::move(packet));
        }
    }

    void stream_processor::enqueue_packet(std::string packet)
    {
        constexpr auto END_BYTES = CHECKSUM_SIZE + 1;

        if (packet.size() < (END_BYTES + 1) //
            || packet.front() != '$'        //
            || packet[packet.size() - END_BYTES] != '#')
        {
            return;
        }

        const auto checksum = strtoul(packet.c_str() + packet.size() - CHECKSUM_SIZE, nullptr, 16);
        assert((checksum & 0xFF) == checksum);

        packet.erase(packet.begin());
        packet.erase(packet.size() - END_BYTES, END_BYTES);

        const auto computed_checksum = compute_checksum(packet);

        if (computed_checksum == checksum)
        {
            this->packets_.push(std::move(packet));
        }
    }
}
