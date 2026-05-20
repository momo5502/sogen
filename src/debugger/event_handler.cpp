#include "event_handler.hpp"
#include "message_transmitter.hpp"
#include "windows_emulator.hpp"
#include "memory_utils.hpp"

#include <base64.hpp>

#include <utils/string.hpp>

#include <string>
#include <string_view>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4244)
#endif

#include "events_generated.hxx"

#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace sogen::debugger
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

        const char* region_kind_string(const memory_region_kind kind)
        {
            switch (kind)
            {
            case memory_region_kind::free:
                return "free";
            case memory_region_kind::private_allocation:
                return "private";
            case memory_region_kind::file_section_view:
                return "file";
            case memory_region_kind::pagefile_section_view:
                return "pagefile";
            case memory_region_kind::section_image:
                return "image";
            case memory_region_kind::mmio:
                return "mmio";
            default:
                return "unknown";
            }
        }

        void append_json_string(std::string& out, const std::string_view value)
        {
            constexpr std::string_view digits = "0123456789abcdef";

            out += '"';
            for (const char ch : value)
            {
                switch (ch)
                {
                case '"':
                    out += R"(\")";
                    break;
                case '\\':
                    out += R"(\\)";
                    break;
                case '\n':
                    out += R"(\n)";
                    break;
                case '\r':
                    out += R"(\r)";
                    break;
                case '\t':
                    out += R"(\t)";
                    break;
                default: {
                    const auto byte = static_cast<unsigned char>(ch);
                    if (byte < 0x20)
                    {
                        out += R"(\u00)";
                        out += digits[(byte >> 4) & 0xF];
                        out += digits[byte & 0xF];
                    }
                    else
                    {
                        out += ch;
                    }
                    break;
                }
                }
            }
            out += '"';
        }

        void append_region(std::string& out, bool& first, const uint64_t base, const uint64_t size, const std::string_view protection,
                           const char* state, const char* kind, const std::string_view module)
        {
            if (!first)
            {
                out += ',';
            }
            first = false;

            out += R"({"base":"0x)";
            out += utils::string::to_hex_number(base);
            out += R"(","size":)";
            out += std::to_string(size);
            out += R"(,"protection":)";
            append_json_string(out, protection);
            out += R"(,"state":")";
            out += state;
            out += R"(","kind":")";
            out += kind;
            out += R"(","module":)";
            if (module.empty())
            {
                out += "null";
            }
            else
            {
                append_json_string(out, module);
            }
            out += '}';
        }

        // Read-only enumeration of the allocated virtual address space:
        // reserved ranges and their committed sub-ranges with real protection
        // flags, including non-readable / guard pages. Free (unallocated) space
        // is intentionally omitted. Snapshotted from the paused state and
        // serialized as JSON so the payload stays trivially forward/backward
        // compatible. Reading the maps never mutates state.
        void handle_get_memory_regions(const event_context& c)
        {
            auto& memory = c.win_emu.memory;
            auto& modules = c.win_emu.mod_manager;
            const auto& reserved_regions = memory.get_reserved_regions();

            std::string json{};
            json.reserve(reserved_regions.size() * 192 + 2);
            json += '[';

            bool first = true;

            for (const auto& [base, region] : reserved_regions)
            {
                const auto* module_name = modules.find_name(base);
                const std::string_view module =
                    (!module_name || std::string_view(module_name) == "<N/A>") ? std::string_view{} : module_name;

                append_region(json, first, base, region.length, get_permission_string(region.initial_permission), "reserve",
                              region_kind_string(region.kind), module);

                for (const auto& [committed_base, committed] : region.committed_regions)
                {
                    auto protection = get_permission_string(committed.permissions.common);
                    if (committed.permissions.is_guarded())
                    {
                        protection += 'g';
                    }

                    append_region(json, first, committed_base, committed.length, protection, "commit", region_kind_string(region.kind),
                                  module);
                }
            }

            json += ']';

            Debugger::GetMemoryRegionsResponseT response{};
            response.regions.assign(json.begin(), json.end());

            send_event(std::move(response));
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
            std::array<uint8_t, 512> buffer{};
            const auto res = c.win_emu.emu().read_register(static_cast<x86_register>(request.register_), buffer.data(), buffer.size());

            const auto size = std::min(buffer.size(), res);

            Debugger::ReadRegisterResponseT response{};
            response.register_ = request.register_;
            response.data.assign(buffer.data(), buffer.data() + size);

            send_event(std::move(response));
        }

        void handle_write_register(const event_context& c, const Debugger::WriteRegisterRequestT& request)
        {
            bool success{};
            size_t size = request.data.size();

            try
            {
                size =
                    c.win_emu.emu().write_register(static_cast<x86_register>(request.register_), request.data.data(), request.data.size());
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

            case Debugger::Event_GetMemoryRegionsRequest:
                handle_get_memory_regions(c);
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
        update_emulation_status(c.win_emu);

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

    void update_emulation_status(const windows_emulator& win_emu)
    {
        const auto memory_status = win_emu.memory.compute_memory_stats();

        Debugger::EmulationStatusT status{};
        status.reserved_memory = memory_status.reserved_memory;
        status.committed_memory = memory_status.committed_memory;
        status.executed_instructions = win_emu.get_executed_instructions();
        status.active_threads = static_cast<uint32_t>(win_emu.process.get_live_thread_count());
        send_event(status);
    }

    void handle_exit(const windows_emulator& win_emu, std::optional<NTSTATUS> exit_status)
    {
        update_emulation_status(win_emu);

        Debugger::ApplicationExitT response{};
        response.exit_status = exit_status;
        send_event(response);
    }
} // namespace sogen::debugger
