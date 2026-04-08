#include "connection_handler.hpp"
#include "checksum.hpp"
#include <utils/string.hpp>

#include <thread>

using namespace std::literals;

namespace gdb_stub
{
    namespace
    {
        bool read_from_socket(stream_processor& processor, network::tcp_client_socket& client)
        {
            const auto data = client.receive();
            if (!data)
            {
                return false;
            }

            processor.push_stream_data(*data);
            return true;
        }
    }

    connection_handler::connection_handler(network::tcp_client_socket& client, utils::optional_function<bool()> should_stop)
        : should_stop_(std::move(should_stop)),
          client_(client)
    {
        this->client_.set_blocking(false);

        this->stop_ = false;

        this->output_thread_ = std::thread([this] {
            this->transmission_loop(); //
        });
    }

    connection_handler::~connection_handler()
    {
        this->stop_ = true;
        this->condition_variable_.notify_all();

        if (this->output_thread_.joinable())
        {
            this->output_thread_.join();
        }
    }

    std::optional<std::string> connection_handler::get_packet()
    {
        while (this->client_.is_valid() && !this->processor_.has_packet() && !this->should_stop_())
        {
            if (!read_from_socket(this->processor_, this->client_))
            {
                (void)this->client_.sleep(100ms, true);
            }
        }

        if (this->processor_.has_packet())
        {
            return this->processor_.get_next_packet();
        }

        return std::nullopt;
    }

    static constexpr std::string compress(const std::string_view data)
    {
        std::string result;
        result.reserve(data.size());

        auto compress_run = [&result](char symbol, int repeat_count) {
            if (repeat_count >= 3)
            {
                result.push_back('*');
                if (repeat_count == 6 || repeat_count == 7)
                {
                    result.push_back(static_cast<char>(5 + 29));
                    result.push_back(symbol);
                    if (repeat_count == 7)
                    {
                        result.push_back(symbol);
                    }
                }
                else
                {
                    result.push_back(static_cast<char>(repeat_count + 29));
                }
            }
            else if (repeat_count >= 1)
            {
                result.push_back(symbol);
                if (repeat_count == 2)
                {
                    result.push_back(symbol);
                }
            }
        };

        bool start = true;
        char prev_ch = 0;
        int counter = 0;

        for (auto ch : data)
        {
            if (start || prev_ch != ch || counter == 97)
            {
                compress_run(prev_ch, counter);
                result.push_back(ch);
                prev_ch = ch;
                counter = 0;
                start = false;
            }
            else
            {
                counter++;
            }
        }

        if (counter)
        {
            compress_run(prev_ch, counter);
        }

        return result;
    }

    void connection_handler::send_reply(const std::string_view data)
    {
        const auto compressed = compress(data);
        const auto checksum = utils::string::to_hex_string(compute_checksum(compressed));
        this->send_raw_data("$" + compressed + "#" + checksum);
    }

    void connection_handler::send_raw_data(const std::string_view data)
    {
        {
            std::scoped_lock _{this->mutex_};
            this->output_stream_.append(data);
        }

        this->condition_variable_.notify_one();
    }

    bool connection_handler::should_stop() const
    {
        return this->stop_ || !this->client_.is_valid();
    }

    void connection_handler::close() const
    {
        this->client_.close();
    }

    void connection_handler::await_transmission(const std::function<void()>& handler)
    {
        std::unique_lock lock{this->mutex_};

        const auto can_run = [this] {
            return this->should_stop() //
                   || !this->output_stream_.empty();
        };

        const auto run = this->condition_variable_.wait_for(lock, 100ms, can_run);

        if (run && !this->should_stop())
        {
            handler();
        }
    }

    std::string connection_handler::get_next_data_to_transmit()
    {
        std::string transmit_data{};

        this->await_transmission([&] {
            transmit_data = std::move(this->output_stream_);
            this->output_stream_ = {};
        });

        return transmit_data;
    }

    void connection_handler::transmission_loop()
    {
        while (!this->should_stop())
        {
            const auto data = this->get_next_data_to_transmit();
            (void)this->client_.send(data);
        }
    }
}
