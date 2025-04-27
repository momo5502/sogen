#pragma once

namespace debugger
{
    enum class event_type
    {
        invalid = 0,
        pause = 1,
        run = 2,
        register_request = 3,
        register_response = 4,
        write_memory_request = 5,
        write_memory_response = 6,
        read_memory_request = 7,
        read_memory_response = 8,
    };

    struct event
    {
        event_type type{event_type::invalid};
    };

    template <event_type Type>
    struct typed_event : event
    {
        typed_event()
            : event{
                  .type = Type,
              }
        {
        }
    };

    using pause_event = typed_event<event_type::pause>;
    using run_event = typed_event<event_type::run>;

    NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(event, type);
}
