#include "event_handler.hpp"
#include "message_transmitter.hpp"
#include "windows_emulator.hpp"
#include "memory_utils.hpp"
#include "debug_session.hpp"

#include <base64.hpp>

#include <utils/string.hpp>

#include <cstring>
#include <optional>
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

        // --- generic debugger command channel (see ARCHITECTURE.md) ---
        //
        // Request `payload` carries packed little-endian args (documented per
        // kind); response `payload` is UTF-8 JSON. This keeps a JSON serializer
        // (already present) on the hot path and avoids a JSON *parser* in C++.

        enum class debug_command : uint32_t
        {
            get_registers = 0,
            disassemble = 1,
            get_modules = 2,
            get_threads = 3,
            get_callstack = 4,
            set_breakpoint = 5,
            clear_breakpoint = 6,
            list_breakpoints = 7,
            step_into = 8,
            step_over = 9,
            step_out = 10,
            run_to = 11,
            continue_execution = 12,
        };

        template <typename T>
        bool read_le(const std::vector<uint8_t>& buf, size_t offset, T& out)
        {
            if (offset + sizeof(T) > buf.size())
            {
                return false;
            }
            std::memcpy(&out, buf.data() + offset, sizeof(T));
            return true;
        }

        debug_session& get_debug_session(windows_emulator& win_emu)
        {
            static windows_emulator* bound = nullptr;
            static std::optional<debug_session> session{};
            if (bound != &win_emu)
            {
                session.emplace(win_emu);
                bound = &win_emu;
            }
            return *session;
        }

        // Shared break/step controller. The break loop blocks here; debug
        // commands (and RunRequest) release it with a requested motion that is
        // then enforced by the persistent control hook in debug_session.
        struct break_controller
        {
            bool resume{false};
            step_request request{step_request::none};
            uint64_t run_to_address{0};

            int mode{0}; // 0: free, 1: single-step, 2: run-to-target
            uint64_t origin{0};
            uint64_t target{0};
        };

        break_controller& controller()
        {
            static break_controller instance{};
            return instance;
        }

        std::string build_registers_json(const std::vector<register_value>& regs)
        {
            std::string json = R"({"registers":[)";
            bool first = true;
            for (const auto& r : regs)
            {
                if (!first)
                {
                    json += ',';
                }
                first = false;
                json += R"({"name":)";
                append_json_string(json, r.name);
                json += R"(,"value":"0x)";
                json += utils::string::to_hex_number(r.value);
                json += R"(","size":)";
                json += std::to_string(r.size);
                json += '}';
            }
            json += "]}";
            return json;
        }

        std::string build_disassembly_json(const std::vector<disassembled_instruction>& insns)
        {
            std::string json = R"({"instructions":[)";
            bool first = true;
            for (const auto& i : insns)
            {
                if (!first)
                {
                    json += ',';
                }
                first = false;
                json += R"({"address":"0x)";
                json += utils::string::to_hex_number(i.address);
                json += R"(","mnemonic":)";
                append_json_string(json, i.mnemonic);
                json += R"(,"operands":)";
                append_json_string(json, i.operands);
                json += R"(,"symbol":)";
                append_json_string(json, i.symbol);
                json += R"(,"size":)";
                json += std::to_string(i.bytes.size());
                json += R"(,"isCall":)";
                json += i.is_call ? "true" : "false";
                json += R"(,"isJump":)";
                json += i.is_jump ? "true" : "false";
                json += R"(,"isReturn":)";
                json += i.is_return ? "true" : "false";
                if (i.branch)
                {
                    json += R"(,"branch":"0x)";
                    json += utils::string::to_hex_number(*i.branch);
                    json += '"';
                }
                json += '}';
            }
            json += "]}";
            return json;
        }

        std::string build_modules_json(const std::vector<module_info>& mods)
        {
            std::string json = R"({"modules":[)";
            bool first = true;
            for (const auto& m : mods)
            {
                if (!first)
                {
                    json += ',';
                }
                first = false;
                json += R"({"name":)";
                append_json_string(json, m.name);
                json += R"(,"base":"0x)";
                json += utils::string::to_hex_number(m.base);
                json += R"(","size":)";
                json += std::to_string(m.size);
                json += R"(,"entry":"0x)";
                json += utils::string::to_hex_number(m.entry_point);
                json += R"("})";
            }
            json += "]}";
            return json;
        }

        std::string build_threads_json(const std::vector<thread_info>& threads)
        {
            std::string json = R"({"threads":[)";
            bool first = true;
            for (const auto& t : threads)
            {
                if (!first)
                {
                    json += ',';
                }
                first = false;
                json += R"({"id":)";
                json += std::to_string(t.id);
                json += R"(,"ip":"0x)";
                json += utils::string::to_hex_number(t.instruction_pointer);
                json += R"(","active":)";
                json += t.active ? "true" : "false";
                json += '}';
            }
            json += "]}";
            return json;
        }

        std::string build_callstack_json(const std::vector<stack_frame>& frames)
        {
            std::string json = R"({"frames":[)";
            bool first = true;
            for (const auto& f : frames)
            {
                if (!first)
                {
                    json += ',';
                }
                first = false;
                json += R"({"ip":"0x)";
                json += utils::string::to_hex_number(f.instruction_pointer);
                json += R"(","sp":"0x)";
                json += utils::string::to_hex_number(f.stack_pointer);
                json += R"(","module":)";
                append_json_string(json, f.module);
                json += '}';
            }
            json += "]}";
            return json;
        }

        std::string build_breakpoints_json(const std::vector<breakpoint>& bps)
        {
            std::string json = R"({"breakpoints":[)";
            bool first = true;
            for (const auto& b : bps)
            {
                if (!first)
                {
                    json += ',';
                }
                first = false;
                json += R"({"address":"0x)";
                json += utils::string::to_hex_number(b.address);
                json += R"(","type":)";
                json += std::to_string(static_cast<uint32_t>(b.type));
                json += R"(,"enabled":)";
                json += b.enabled ? "true" : "false";
                json += '}';
            }
            json += "]}";
            return json;
        }

        void handle_debug_command(const event_context& c, const Debugger::DebugCommandRequestT& request)
        {
            auto& session = get_debug_session(c.win_emu);
            const auto& in = request.payload;

            Debugger::DebugCommandResponseT response{};
            response.id = request.id;
            response.ok = true;

            std::string json{};

            switch (static_cast<debug_command>(request.kind))
            {
            case debug_command::get_registers:
                json = build_registers_json(session.registers());
                break;

            case debug_command::disassemble: {
                uint64_t address = 0;
                uint32_t count = 0;
                if (!read_le(in, 0, address) || !read_le(in, sizeof(uint64_t), count) || count == 0)
                {
                    response.ok = false;
                    break;
                }
                json = build_disassembly_json(session.disassemble(address, count));
                break;
            }

            case debug_command::get_modules:
                json = build_modules_json(session.modules());
                break;

            case debug_command::get_threads:
                json = build_threads_json(session.threads());
                break;

            case debug_command::get_callstack:
                json = build_callstack_json(session.call_stack());
                break;

            case debug_command::set_breakpoint: {
                uint64_t address = 0;
                uint8_t type = 0;
                if (!read_le(in, 0, address))
                {
                    response.ok = false;
                    break;
                }
                (void)read_le(in, sizeof(uint64_t), type);
                response.ok = session.add_breakpoint(address, static_cast<breakpoint_type>(type));
                json = build_breakpoints_json(session.list_breakpoints());
                break;
            }

            case debug_command::clear_breakpoint: {
                uint64_t address = 0;
                if (!read_le(in, 0, address))
                {
                    response.ok = false;
                    break;
                }
                response.ok = session.remove_breakpoint(address);
                json = build_breakpoints_json(session.list_breakpoints());
                break;
            }

            case debug_command::list_breakpoints:
                json = build_breakpoints_json(session.list_breakpoints());
                break;

            case debug_command::step_into:
                debugger::request_resume(step_request::into);
                json = "{}";
                break;

            case debug_command::step_over:
                debugger::request_resume(step_request::over);
                json = "{}";
                break;

            case debug_command::step_out:
                debugger::request_resume(step_request::step_out);
                json = "{}";
                break;

            case debug_command::run_to: {
                uint64_t address = 0;
                if (!read_le(in, 0, address))
                {
                    response.ok = false;
                    break;
                }
                debugger::request_resume(step_request::cont, address);
                json = "{}";
                break;
            }

            case debug_command::continue_execution:
                debugger::request_resume(step_request::cont);
                json = "{}";
                break;

            default:
                response.ok = false;
                json = R"({"error":"unsupported"})";
                break;
            }

            response.payload.assign(json.begin(), json.end());
            send_event(std::move(response));
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
                debugger::request_resume(step_request::cont);
                break;

            case Debugger::Event_GetStateRequest:
                handle_get_state(c);
                break;

            case Debugger::Event_GetMemoryRegionsRequest:
                handle_get_memory_regions(c);
                break;

            case Debugger::Event_DebugCommandRequest:
                handle_debug_command(c, *e.event.AsDebugCommandRequest());
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

    void request_resume(const step_request request, const uint64_t run_to_address)
    {
        auto& ctrl = controller();
        ctrl.request = request;
        ctrl.run_to_address = run_to_address;
        ctrl.resume = true;
    }

    // Consulted by the persistent control hook on every instruction. Returns
    // true exactly once when the armed step motion completes; breakpoints are
    // matched separately by debug_session.
    bool step_should_break(const uint64_t address)
    {
        auto& ctrl = controller();
        if (ctrl.mode == 1) // single-step: stop on the next distinct instruction
        {
            if (address != ctrl.origin)
            {
                ctrl.mode = 0;
                return true;
            }
            return false;
        }
        if (ctrl.mode == 2) // run-to-target
        {
            if (address == ctrl.target)
            {
                ctrl.mode = 0;
                return true;
            }
            return false;
        }
        return false;
    }

    void enter_breakpoint(windows_emulator& win_emu, const uint64_t address)
    {
        // Stop: tell the UI, then block here draining debug commands using the
        // exact same primitive PauseRequest uses, until a resume/step arrives.
        {
            Debugger::GetStateResponseT stopped{};
            stopped.state = Debugger::State_Paused;
            send_event(stopped);
        }
        update_emulation_status(win_emu);

        auto& ctrl = controller();
        ctrl.resume = false;

        event_context lc{.win_emu = win_emu, .state = emulation_state::paused};
        while (!ctrl.resume)
        {
            handle_events_once(lc);
            if (ctrl.resume)
            {
                break;
            }
            suspend_execution(2ms);
        }
        ctrl.resume = false;

        // Resolve the requested motion into the control-hook plan.
        auto& session = get_debug_session(win_emu);
        ctrl.mode = 0;
        ctrl.origin = address;
        ctrl.target = 0;

        switch (ctrl.request)
        {
        case step_request::into:
            ctrl.mode = 1;
            break;
        case step_request::over: {
            const auto insns = session.disassemble(address, 1);
            if (!insns.empty() && insns[0].is_call)
            {
                ctrl.mode = 2;
                ctrl.target = address + insns[0].bytes.size();
            }
            else
            {
                ctrl.mode = 1;
            }
            break;
        }
        case step_request::step_out: {
            const auto frames = session.call_stack();
            if (frames.size() >= 2)
            {
                ctrl.mode = 2;
                ctrl.target = frames[1].instruction_pointer;
            }
            else
            {
                ctrl.mode = 1;
            }
            break;
        }
        case step_request::cont:
            if (ctrl.run_to_address != 0)
            {
                ctrl.mode = 2;
                ctrl.target = ctrl.run_to_address;
            }
            break;
        case step_request::none:
            break;
        }

        ctrl.request = step_request::none;
        ctrl.run_to_address = 0;

        Debugger::GetStateResponseT running{};
        running.state = Debugger::State_Running;
        send_event(running);
    }
}
