#pragma once
#include <queue>
#include <string>

namespace gdb_stub
{
    class stream_processor
    {
      public:
        bool has_packet() const;
        std::string get_next_packet();
        void push_stream_data(const std::string& data);

      private:
        std::string stream_{};
        std::queue<std::string> packets_{};

        void process_data_stream();
        void enqueue_packet(std::string packet);
    };
}
