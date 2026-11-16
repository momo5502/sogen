#pragma once
#include "process_session.hpp"
#include "process_protocol_generated.hxx"
#include <list>
#include <unordered_map>
#include <unordered_set>

namespace sogen
{
    class process_session_registry
    {
      public:
        static constexpr auto reservation_lifetime = std::chrono::minutes(2);

        explicit process_session_registry(process_session::clock_source clock);
        ProcessProtocol::SessionResponseT handle(const ProcessProtocol::SessionRequestT& request);

      private:
        struct registry_record
        {
            ProcessProtocol::SessionProcessT process{};
            bool published{};
            bool discarded{};
            std::chrono::steady_clock::time_point reserved_until{};
            std::list<uint32_t>::iterator reservation{};
        };

        process_session::clock_source now;
        std::list<uint32_t> reservations{};
        std::mutex mutex{};
        std::unordered_map<uint32_t, registry_record> processes{};
        std::unordered_set<uint32_t> allocated_ids{};
        uint64_t next_process_id{8};
        void expire_reservations();
    };
}
