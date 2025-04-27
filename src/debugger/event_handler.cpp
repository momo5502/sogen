#include "event_handler.hpp"
#include "events.hpp"

#include "message_transmitter.hpp"

namespace debugger
{
    namespace
    {
        void handle_event(event_context& c, const event& e, const nlohmann::json& obj)
        {
            switch (e.type)
            {
            case event_type::pause:
                c.win_emu.emu().stop();
                break;

            case event_type::run:
                c.resume = true;
                break;

            default:
                break;
            }
        }

        void handle_object(event_context& c, const nlohmann::json& obj)
        {
            try
            {
                const auto e = obj.get<event>();
                handle_event(c, e, obj);
            }
            catch (const std::exception& e)
            {
                puts(e.what());
            }
        }
    }

    void handle_events(event_context& c)
    {
        while (true)
        {
            suspend_execution(0ms);

            const auto obj = receive_object();
            if (obj.is_null())
            {
                break;
            }

            handle_object(c, obj);
        }
    }
}