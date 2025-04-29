#include "event_handler.hpp"
#include "message_transmitter.hpp"

#include <base64.hpp>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4244)
#endif

#include "events_generated.hxx"

#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace debugger
{
    namespace
    {
        std::optional<Debugger::DebugEventT> receive_event()
        {
            const auto message = receive_message();
            if (message.empty())
            {
                return std::nullopt;
            }

            const auto data = base64::from_base64(message);

            flatbuffers::Verifier verifier(reinterpret_cast<const uint8_t*>(data.data()), data.size());
            if (!Debugger::VerifyDebugEventBuffer(verifier))
            {
                return std::nullopt;
            }

            Debugger::DebugEventT e{};
            Debugger::GetDebugEvent(data.data())->UnPackTo(&e);

            return {std::move(e)};
        }

        void send_event(const Debugger::DebugEventT& event)
        {
            flatbuffers::FlatBufferBuilder fbb{};
            fbb.Finish(Debugger::DebugEvent::Pack(fbb, &event));

            const std::string_view buffer(reinterpret_cast<const char*>(fbb.GetBufferPointer()), fbb.GetSize());
            const auto message = base64::to_base64(buffer);

            send_message(message);
        }

        template <typename T>
            requires(!std::is_same_v<std::remove_cvref_t<T>, Debugger::DebugEventT>)
        void send_event(T event)
        {
            Debugger::DebugEventT e{};
            e.event.Set(std::move(event));
            send_event(e);
        }

        Debugger::State translate_state(const emulation_state state)
        {
            switch (state)
            {
            case emulation_state::paused:
                return Debugger::State_Paused;

            case emulation_state::none:
            case emulation_state::running:
                return Debugger::State_Running;

            default:
                return Debugger::State_None;
            }
        }

        void handle_get_state(const event_context& c)
        {
            Debugger::GetStateResponseT response{};
            response.state = translate_state(c.state);

            send_event(response);
        }

        void handle_read_memory(const event_context& c, const Debugger::ReadMemoryRequestT& request)
        {
            std::vector<uint8_t> buffer{};
            buffer.resize(request.size);
            const auto res = c.win_emu.memory.try_read_memory(request.address, buffer.data(), buffer.size());

            Debugger::ReadMemoryResponseT response{};
            response.address = request.address;

            if (res)
            {
                response.data = std::move(buffer);
            }

            send_event(std::move(response));
        }

        void handle_write_memory(const event_context& c, const Debugger::WriteMemoryRequestT& request)
        {
            bool success{};

            try
            {
                c.win_emu.memory.write_memory(request.address, request.data.data(), request.data.size());
                success = true;
            }
            catch (...)
            {
                success = false;
            }

            Debugger::WriteMemoryResponseT response{};
            response.address = request.address;
            response.size = static_cast<uint32_t>(request.data.size());
            response.success = success;

            send_event(response);
        }

        void handle_read_register(const event_context& c, const Debugger::ReadRegisterRequestT& request)
        {
            uint8_t buffer[512]{};
            const auto res =
                c.win_emu.emu().read_register(static_cast<x86_register>(request.register_), buffer, sizeof(buffer));

            const auto size = std::min(sizeof(buffer), res);

            Debugger::ReadRegisterResponseT response{};
            response.register_ = request.register_;
            response.data.assign(buffer, buffer + size);

            send_event(std::move(response));
        }

        void handle_write_register(const event_context& c, const Debugger::WriteRegisterRequestT& request)
        {
            bool success{};
            size_t size = request.data.size();

            try
            {
                size = c.win_emu.emu().write_register(static_cast<x86_register>(request.register_), request.data.data(),
                                                      request.data.size());
                success = true;
            }
            catch (...)
            {
                success = false;
            }

            Debugger::WriteRegisterResponseT response{};
            response.register_ = request.register_;
            response.size = static_cast<uint32_t>(size);
            response.success = success;

            send_event(response);
        }

        void handle_event(event_context& c, const Debugger::DebugEventT& e)
        {
            switch (e.event.type)
            {
            case Debugger::Event_PauseRequest:
                c.state = emulation_state::paused;
                break;

            case Debugger::Event_RunRequest:
                c.state = emulation_state::running;
                break;

            case Debugger::Event_GetStateRequest:
                handle_get_state(c);
                break;

            case Debugger::Event_ReadMemoryRequest:
                handle_read_memory(c, *e.event.AsReadMemoryRequest());
                break;

            case Debugger::Event_WriteMemoryRequest:
                handle_write_memory(c, *e.event.AsWriteMemoryRequest());
                break;

            case Debugger::Event_ReadRegisterRequest:
                handle_read_register(c, *e.event.AsReadRegisterRequest());
                break;

            case Debugger::Event_WriteRegisterRequest:
                handle_write_register(c, *e.event.AsWriteRegisterRequest());
                break;

            default:
                break;
            }
        }
    }

    void handle_events_once(event_context& c)
    {
        while (true)
        {
            suspend_execution(0ms);

            const auto e = receive_event();
            if (!e.has_value())
            {
                break;
            }

            handle_event(c, *e);
        }
    }

    void handle_events(event_context& c)
    {
        while (true)
        {
            handle_events_once(c);

            if (c.state != emulation_state::paused)
            {
                break;
            }

            suspend_execution(2ms);
        }
    }
}