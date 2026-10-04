// The browser front-end. The page seeds the guest filesystem through Emscripten's FS, then calls main()
// with the options the user picked; everything this process learns comes back as one JSON object per
// line on stdout, which the page parses into its trace, module and memory views.
//
// One line per event rather than a single document at the end: a run can execute millions of
// instructions, and the page has to be able to render the trace as it arrives instead of waiting for a
// result that may never come. The composed desktop travels the other way, as a transferred pixel buffer
// rather than a JSON line, and input travels back in: a GUI process never exits, so a frame handed over
// at the end would be a frame nobody ever sees.

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <span>
#include <string_view>
#include <vector>

#include <emscripten/emscripten.h>
#include <emscripten/em_js.h>

#include <backend_selection.hpp>
#include <macos_emulator.hpp>
#include <screenshot_ui_backend.hpp>
#include <host_range_reader.hpp>
#include <macos_memory_report.hpp>
#include <stop_reason.hpp>
#include <module/macho_mapping.hpp>
#include <trace/macos_syscall_trace.hpp>
#include <utils/io.hpp>

namespace
{
    std::string json_escape(const std::string_view text)
    {
        std::string out{};
        out.reserve(text.size() + 8);

        for (const auto raw : text)
        {
            const auto c = static_cast<unsigned char>(raw);

            switch (c)
            {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                // Guest data reaches here verbatim, so anything a JSON string may not carry literally is
                // escaped rather than trusted. \u00XX is valid for a lone byte because the page reads
                // these as text, not as a faithful byte stream.
                if (c < 0x20 || c == 0x7F)
                {
                    char buffer[7]{};
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                    out += buffer;
                }
                else
                {
                    out += static_cast<char>(c);
                }
                break;
            }
        }

        return out;
    }

    void emit_line(const std::string& payload)
    {
        std::printf("%s\n", payload.c_str());
        std::fflush(stdout);
    }

    void emit_event(const std::string_view type, const std::string& fields)
    {
        std::string line = "{\"t\":\"";
        line += type;
        line += "\"";

        if (!fields.empty())
        {
            line += ",";
            line += fields;
        }

        line += "}";
        emit_line(line);
    }

    std::string field(const std::string_view name, const std::string_view value)
    {
        return "\"" + std::string(name) + "\":\"" + json_escape(value) + "\"";
    }

    std::string field(const std::string_view name, const uint64_t value)
    {
        return "\"" + std::string(name) + "\":" + std::to_string(value);
    }

    std::string hex(const uint64_t value)
    {
        char buffer[32]{};
        std::snprintf(buffer, sizeof(buffer), "0x%" PRIx64, value);
        return buffer;
    }

    // The colour a line was printed in is the only level the logger carries: red is an error, yellow a
    // warning, cyan an informational line, green a success, pink a force-print from outside the emulator.
    std::string_view log_level_name(const sogen::color c)
    {
        using enum sogen::color;

        switch (c)
        {
        case red:
            return "error";
        case yellow:
            return "warn";
        case cyan:
            return "info";
        case green:
            return "success";
        case pink:
            return "print";
        default:
            return "log";
        }
    }

    struct web_options
    {
        std::string executable{"/macho_trace_arm64"};
        std::string root{"/root"};
        std::vector<std::string> argv{};
        std::vector<std::string> envp{};
        std::set<std::string, std::less<>> ignored{};
        uint64_t max_instructions{0};
        size_t string_limit{256};
        size_t buffer_limit{32};
        bool decode_arguments{true};
        bool trace_syscalls{true};
        bool trace_modules{true};
        bool memory_report{true};
        bool emulator_log{false};
        bool gui{false};
        int32_t desktop_width{1024};
        int32_t desktop_height{640};
        double frame_interval_ms{100.0};
    };

    void split_into(std::set<std::string, std::less<>>& target, const std::string_view values)
    {
        size_t start = 0;
        while (start <= values.size())
        {
            const auto comma = values.find(',', start);
            const auto piece = values.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start);

            if (!piece.empty())
            {
                target.emplace(piece);
            }

            if (comma == std::string_view::npos)
            {
                return;
            }

            start = comma + 1;
        }
    }

    bool matches(const std::string_view argument, const std::string_view name, std::string_view& value)
    {
        if (!argument.starts_with(name) || argument.size() <= name.size() || argument[name.size()] != '=')
        {
            return false;
        }

        value = argument.substr(name.size() + 1);
        return true;
    }

    // Reads a range through the host range reader and reports what came back, so the page can check that
    // the JS bridge is actually reachable. The build cannot catch a missing one: EM_JS compiles happily
    // and the failure is a ReferenceError at the first read, long after anything is watching.
    int probe_range(const std::string& path, const uint64_t offset, const size_t size)
    {
        auto& reader = sogen::default_host_range_reader();

        const auto total = reader.file_size(path);

        std::vector<std::byte> data(size);
        const auto read_bytes = reader.read(path, offset, data);
        data.resize(read_bytes);

        std::string hex_prefix{};
        for (size_t i = 0; i < data.size() && i < 16; ++i)
        {
            char byte[3]{};
            std::snprintf(byte, sizeof(byte), "%02x", static_cast<unsigned>(std::to_integer<uint8_t>(data[i])));
            hex_prefix += byte;
        }

        std::string fields = field("path", path);
        fields += "," + field("size", total);
        fields += "," + field("offset", offset);
        fields += "," + field("requested", static_cast<uint64_t>(size));
        fields += "," + field("read", static_cast<uint64_t>(read_bytes));
        fields += "," + field("head", hex_prefix);
        emit_event("range", fields);

        return read_bytes == 0 && size != 0 ? 1 : 0;
    }

    web_options parse_options(const int argc, char** argv)
    {
        web_options options{};
        std::string_view value{};

        for (int i = 1; i < argc; ++i)
        {
            const std::string_view argument{argv[i]};

            if (matches(argument, "--exe", value))
            {
                options.executable = std::string{value};
            }
            else if (matches(argument, "--root", value))
            {
                options.root = std::string{value};
            }
            else if (matches(argument, "--arg", value))
            {
                options.argv.emplace_back(value);
            }
            else if (matches(argument, "--env", value))
            {
                options.envp.emplace_back(value);
            }
            else if (matches(argument, "--ignore", value))
            {
                split_into(options.ignored, value);
            }
            else if (matches(argument, "--max-instructions", value))
            {
                options.max_instructions = std::strtoull(std::string{value}.c_str(), nullptr, 10);
            }
            else if (matches(argument, "--string-limit", value))
            {
                options.string_limit = static_cast<size_t>(std::strtoull(std::string{value}.c_str(), nullptr, 10));
            }
            else if (matches(argument, "--buffer-limit", value))
            {
                options.buffer_limit = static_cast<size_t>(std::strtoull(std::string{value}.c_str(), nullptr, 10));
            }
            else if (argument == "--no-decode")
            {
                options.decode_arguments = false;
            }
            else if (argument == "--no-syscalls")
            {
                options.trace_syscalls = false;
            }
            else if (argument == "--no-modules")
            {
                options.trace_modules = false;
            }
            else if (argument == "--no-memory-report")
            {
                options.memory_report = false;
            }
            else if (argument == "--log")
            {
                options.emulator_log = true;
            }
            else if (argument == "--gui")
            {
                options.gui = true;
            }
            else if (matches(argument, "--frame-interval", value))
            {
                options.frame_interval_ms = std::strtod(std::string{value}.c_str(), nullptr);
            }
            else if (matches(argument, "--desktop-size", value))
            {
                const auto separator = value.find_first_of("xX");
                if (separator != std::string_view::npos)
                {
                    options.desktop_width = static_cast<int32_t>(std::strtol(std::string{value.substr(0, separator)}.c_str(), nullptr, 10));
                    options.desktop_height =
                        static_cast<int32_t>(std::strtol(std::string{value.substr(separator + 1)}.c_str(), nullptr, 10));
                }
            }
        }

        if (options.argv.empty())
        {
            options.argv.push_back(options.executable);
        }

        return options;
    }

    // Reported before the run so the page can explain a refusal in terms the user can act on. A browser
    // cannot supply the 5.4 GB shared cache a dynamically linked Mach-O needs, so such an image is
    // described rather than attempted.
    void describe_image(const std::filesystem::path& host_path)
    {
        std::vector<std::byte> data{};
        if (!sogen::utils::io::read_file(host_path, &data))
        {
            emit_event("image", field("readable", uint64_t{0}));
            return;
        }

        try
        {
            const auto slice = sogen::select_macho_slice(data, host_path);
            const auto metadata = sogen::read_macho_module_metadata(data, host_path, slice, 0);

            std::string fields = field("readable", uint64_t{1});
            fields += "," + field("bytes", static_cast<uint64_t>(data.size()));
            fields += "," + field("arm64e", static_cast<uint64_t>(metadata.is_arm64e() ? 1 : 0));
            fields += "," + field("dylinker", metadata.dylinker_path);
            fields += "," + field("entry", hex(metadata.entry_point));
            emit_event("image", fields);
        }
        catch (const std::exception& e)
        {
            emit_event("image", field("readable", uint64_t{1}) + "," + field("error", e.what()));
        }
    }

    double pointer_to_double(const void* pointer)
    {
        return static_cast<double>(reinterpret_cast<uintptr_t>(pointer));
    }

    // clang-format off
    // The bodies below are JavaScript, not C++. clang-format parses them as C++ and rewrites !== into
    // != = and === into == =, which compiles fine and emits glue that fails to parse at load time -- a
    // break the build cannot see and only a run in a JS engine reports.
    EM_JS_DEPS(sogen_web_stream, "$UTF8ToString");

    // -1 means no page is listening, which is a different answer from "listening but behind": the module
    // also loads on the main thread purely to read its embedded demo binaries out, and postMessage there
    // does not take a transfer list.
    EM_JS(double, sogen_web_frame_credit, (), {
        const credit = globalThis.__sogenFrameCredit;
        return typeof credit === "number" ? credit : -1;
    });

    EM_JS(void, sogen_web_frame, (double pixels, double byte_count, double width, double height, double sequence, double presents,
                                  double windows_json), {
        const buffer = HEAPU8.slice(Number(pixels), Number(pixels) + Number(byte_count));

        let windows = [];
        try
        {
            windows = JSON.parse(UTF8ToString(Number(windows_json)));
        }
        catch (error)
        {
            windows = [];
        }

        globalThis.__sogenFrameCredit = globalThis.__sogenFrameCredit - 1;

        postMessage({
            t: "frame",
            width: Number(width),
            height: Number(height),
            sequence: Number(sequence),
            presents: Number(presents),
            windows: windows,
            pixels: buffer,
        }, [buffer.buffer]);
    });

    EM_JS(double, sogen_web_take_input, (double destination, double capacity), {
        const queue = globalThis.__sogenInputQueue;
        if (!Array.isArray(queue) || queue.length === 0)
        {
            return 0;
        }

        const count = Math.min(queue.length, Number(capacity));
        let index = Number(destination) / 8;

        for (let i = 0; i < count; ++i)
        {
            const event = queue[i];
            HEAPF64[index++] = Number(event.window) || 0;
            HEAPF64[index++] = Number(event.message) || 0;
            HEAPF64[index++] = Number(event.wParam) || 0;
            HEAPF64[index++] = Number(event.lParam) || 0;
        }

        queue.splice(0, count);
        return count;
    });

    // The page cannot learn the exit code from callMain: at the first yield ASYNCIFY unwinds the stack
    // and callMain returns 0 -- not undefined, which is what makes it dangerous -- with the run still
    // going. Reading that as "finished" tore a live run down.
    EM_JS(void, sogen_web_finished, (double code), {
        globalThis.__sogenFinished = true;
        if (typeof globalThis.__sogenFrameCredit === "number")
        {
            postMessage({ t: "done", code: Number(code) });
        }
    });
    // clang-format on

    std::string window_list_json(const sogen::macos_window_server& server)
    {
        std::string windows{};

        for (const auto& window : server.windows())
        {
            if (!windows.empty())
            {
                windows += ",";
            }

            windows += "{\"id\":" + std::to_string(window.id) + ",\"x\":" + std::to_string(window.x) +
                       ",\"y\":" + std::to_string(window.y) + ",\"w\":" + std::to_string(window.width) +
                       ",\"h\":" + std::to_string(window.height) + ",\"level\":" + std::to_string(window.level) +
                       ",\"visible\":" + (window.ordered_in ? "true" : "false") + "}";
        }

        return "[" + windows + "]";
    }

    std::string ui_message_name(const uint32_t message)
    {
        switch (message)
        {
        case WM_MOUSEMOVE:
            return "WM_MOUSEMOVE";
        case WM_LBUTTONDOWN:
            return "WM_LBUTTONDOWN";
        case WM_LBUTTONUP:
            return "WM_LBUTTONUP";
        case WM_RBUTTONDOWN:
            return "WM_RBUTTONDOWN";
        case WM_RBUTTONUP:
            return "WM_RBUTTONUP";
        case WM_MBUTTONDOWN:
            return "WM_MBUTTONDOWN";
        case WM_MBUTTONUP:
            return "WM_MBUTTONUP";
        case WM_MOUSEWHEEL:
            return "WM_MOUSEWHEEL";
        case WM_MOUSEHWHEEL:
            return "WM_MOUSEHWHEEL";
        case WM_KEYDOWN:
            return "WM_KEYDOWN";
        case WM_KEYUP:
            return "WM_KEYUP";
        case WM_SYSKEYDOWN:
            return "WM_SYSKEYDOWN";
        case WM_SYSKEYUP:
            return "WM_SYSKEYUP";
        case WM_CHAR:
            return "WM_CHAR";
        case WM_CLOSE:
            return "WM_CLOSE";
        default:
            return hex(message);
        }
    }

    // The composed desktop as raw RGBA, transferred rather than encoded. A frame is already the size of
    // its pixels once the PNG encoder emits stored deflate blocks, and the base64 the page needed to put
    // one in an <img> costs another third on top plus a decode per frame; a transferred ArrayBuffer costs
    // one copy out of linear memory and nothing after that.
    class web_frame_stream
    {
      public:
        web_frame_stream(sogen::macos_emulator& emu, sogen::screenshot_ui_backend& screen, const double interval_ms)
            : emu_(&emu),
              screen_(&screen),
              interval_(interval_ms),
              attached_(sogen_web_frame_credit() >= 0)
        {
        }

        bool attached() const
        {
            return this->attached_;
        }

        void tick()
        {
            const auto now = emscripten_get_now();
            if (now - this->last_tick_ < this->interval_)
            {
                return;
            }

            this->last_tick_ = now;

            // The worker's message loop only runs while the module is unwound, and this is the only place
            // in a run where that happens: every frame ack and every input event the page sent is
            // delivered during this call. Same shape as web_ui_backend::pump_events().
            emscripten_sleep(0);

            this->deliver_input();
            this->send_frame(false);

            if (now - this->last_status_ >= STATUS_INTERVAL_MS)
            {
                this->last_status_ = now;
                this->send_status();
            }
        }

        // A guest parked on its run loop reaches no syscall, so tick() is never called again and both the
        // page's input and its frames stop arriving. The emulator polls this from its idle hook instead.
        // Input is taken every poll rather than at the frame cadence: a park has nothing else to spend
        // the time on, and the cadence is what would otherwise make a click late.
        void idle()
        {
            emscripten_sleep(0);
            this->deliver_input();
            this->tick();
        }

        void count_syscall()
        {
            ++this->syscalls_;
        }

        void finish()
        {
            this->send_frame(true);
            this->send_status();
        }

      private:
        static constexpr size_t INPUT_BATCH = 64;
        static constexpr double STATUS_INTERVAL_MS = 500.0;

        void deliver_input()
        {
            std::array<double, INPUT_BATCH * 4> batch{};
            const auto taken = static_cast<size_t>(sogen_web_take_input(pointer_to_double(batch.data()), static_cast<double>(INPUT_BATCH)));

            if (taken == 0)
            {
                return;
            }

            for (size_t i = 0; i < taken; ++i)
            {
                const auto* record = batch.data() + (i * 4);
                this->screen_->post_event(sogen::ui_event{
                    .window = static_cast<sogen::hwnd>(static_cast<uint64_t>(record[0])),
                    .message = static_cast<uint32_t>(record[1]),
                    .wParam = static_cast<uint64_t>(static_cast<int64_t>(record[2])),
                    .lParam = static_cast<uint64_t>(static_cast<int64_t>(record[3])),
                });
            }

            this->delivered_ += taken;
            this->screen_->pump_events();
        }

        void send_frame(const bool forced)
        {
            if (!this->attached_)
            {
                return;
            }

            const auto presents = static_cast<uint64_t>(this->emu_->ui.present_count());
            auto windows = window_list_json(this->emu_->ui.server);

            if (!forced && this->frames_ > 0 && presents == this->last_presents_ && windows == this->last_windows_)
            {
                return;
            }

            const auto credit = static_cast<int64_t>(sogen_web_frame_credit());
            this->credit_floor_ = std::min(this->credit_floor_, credit);

            if (!forced && credit < 1)
            {
                ++this->dropped_;
                return;
            }

            const auto image = this->screen_->compose();
            if (image.rgba.empty())
            {
                return;
            }

            ++this->frames_;
            this->last_presents_ = presents;
            this->last_windows_ = std::move(windows);

            sogen_web_frame(pointer_to_double(image.rgba.data()), static_cast<double>(image.rgba.size()), static_cast<double>(image.width),
                            static_cast<double>(image.height), static_cast<double>(this->frames_), static_cast<double>(presents),
                            pointer_to_double(this->last_windows_.c_str()));
        }

        void send_status()
        {
            std::string fields = field("frames", this->frames_);
            fields += "," + field("syscalls", this->syscalls_);
            fields += "," + field("dropped", this->dropped_);
            fields += "," + field("presents", static_cast<uint64_t>(this->emu_->ui.present_count()));
            fields += "," + field("windows", static_cast<uint64_t>(this->emu_->ui.server.windows().size()));
            fields += "," + field("threads", static_cast<uint64_t>(this->emu_->process.threads.size()));
            fields += "," + field("input", this->delivered_);
            if (this->credit_floor_ != std::numeric_limits<int64_t>::max())
            {
                fields += ",\"credit\":" + std::to_string(this->credit_floor_);
            }

            emit_event("status", fields);
        }

        sogen::macos_emulator* emu_;
        sogen::screenshot_ui_backend* screen_;
        double interval_;
        bool attached_;

        double last_tick_{};
        double last_status_{};
        uint64_t frames_{};
        uint64_t dropped_{};
        uint64_t delivered_{};
        uint64_t syscalls_{};
        uint64_t last_presents_{};

        // The lowest credit the page ever left the module at. Without it a run with no refused frames is
        // ambiguous: it reads the same whether the page kept up or the desktop simply never changed.
        int64_t credit_floor_{std::numeric_limits<int64_t>::max()};
        std::string last_windows_{};
    };

    int run(const web_options& options)
    {
        auto emu = std::make_unique<sogen::macos_emulator>(sogen::create_arm64_emulator(), std::filesystem::path{options.root});

        // The emulator's own logger writes terminal colour markup, which the page would show literally.
        // Its output therefore stays off and the sink below carries the same lines instead, as events the
        // page can render and filter. A sink runs before either check in logger::print_message, so nothing
        // is lost by muting both.
        //
        // set_silent as well as disable_output because logger::error() force-prints past disable_output_,
        // and one such line would put a bare <span class="terminal-red">...</span> -- with no newline of
        // its own -- into the middle of the NDJSON stream. The worker only parses a line beginning with
        // '{', so everything after it would degrade to plain text rows. src/macos-emulator has no
        // log.error() call today; this is what stops the first one from being a stream corruption.
        emu->log.disable_output(true);
        emu->log.set_silent(true);

        // Off unless asked for, because this is the emulator's verbose channel and nothing else: a native
        // run only sees it under --verbose either, and it is dense enough (measured at ~2,000 lines/s on
        // an AppKit guest, 90% of them one mach_msg2 trace) that streaming it into a page unasked would
        // cost a long run more than it tells anyone.
        if (options.emulator_log)
        {
            emu->log.set_sink([](const sogen::color c, std::string_view message) {
                while (!message.empty() && (message.back() == '\n' || message.back() == '\r'))
                {
                    message.remove_suffix(1);
                }

                if (message.empty())
                {
                    return;
                }

                emit_event("log", field("level", log_level_name(c)) + "," + field("text", message));
            });
        }

        emu->trace.decode_arguments = options.decode_arguments;
        emu->trace.string_limit = options.string_limit;
        emu->trace.buffer_preview_limit = options.buffer_limit;

        sogen::screenshot_ui_backend* screen = nullptr;
        std::optional<web_frame_stream> stream{};

        if (options.gui)
        {
            auto backend = sogen::create_screenshot_ui_backend();
            screen = static_cast<sogen::screenshot_ui_backend*>(backend.get());
            screen->set_desktop_size(options.desktop_width, options.desktop_height);
            emu->set_ui_backend(std::move(backend));

            emu->ui.enabled = true;
            emu->ui.desktop_width = options.desktop_width;
            emu->ui.desktop_height = options.desktop_height;

            // The macOS side owns the backend's event sink, so this observes rather than replaces it: an
            // event that reached the guest and one the window path dropped look identical from the page
            // otherwise, and that difference is the whole value of the counter.
            auto* const emulator = emu.get();
            emulator->ui.set_input_observer([](const sogen::ui_event& event, const bool delivered) {
                std::string fields = field("message", ui_message_name(event.message));
                fields += "," + field("code", static_cast<uint64_t>(event.message));
                fields += "," + field("wparam", event.wParam);
                fields += "," + field("lparam", event.lParam);
                fields += "," + field("window", event.window);
                fields += "," + field("delivered", delivered ? uint64_t{1} : uint64_t{0});
                emit_event("input", fields);
            });

            stream.emplace(*emu, *screen, options.frame_interval_ms);

            if (!stream->attached())
            {
                emit_event("stream", field("attached", uint64_t{0}) + "," +
                                         field("reason", "no frame host installed on this page; the desktop is composed but never sent"));
            }
            else
            {
                // A page behind the frame host is a person who can still click, which is what tells the
                // emulator that a guest with nothing runnable left is waiting rather than deadlocked.
                // Without a host there is nobody to wait for, and the run has to end at idle the way the
                // analyzer's does.
                screen->set_input_source(true);

                emu->on_host_idle = [&stream, announced = false]() mutable {
                    if (!announced)
                    {
                        announced = true;
                        emit_event("idle", field("reason", "the guest has run out of work and is waiting for input"));
                    }

                    stream->idle();
                };

                emit_event("stream",
                           field("attached", uint64_t{1}) + "," + field("interval", static_cast<uint64_t>(options.frame_interval_ms)));
            }
        }

        emu->callbacks.on_gui_routines_bound = [](const size_t bound, const size_t registered, const size_t unbound) {
            emit_event("gui", field("bound", static_cast<uint64_t>(bound)) + "," + field("registered", static_cast<uint64_t>(registered)) +
                                  "," + field("unbound", static_cast<uint64_t>(unbound)));
        };

        emu->callbacks.on_shared_cache_mapped = [](const uint32_t mappings, const uint64_t rebased) {
            emit_event("cache", field("mappings", static_cast<uint64_t>(mappings)) + "," + field("rebased", rebased));
        };

        emu->callbacks.on_stdout = [](const std::string_view data) { emit_event("stdout", field("text", data)); };
        emu->callbacks.on_stderr = [](const std::string_view data) { emit_event("stderr", field("text", data)); };

        if (options.trace_modules)
        {
            emu->callbacks.on_module_load.add([](const sogen::macos_mapped_module& mod) {
                std::string fields = field("name", mod.name);
                fields += "," + field("base", hex(mod.image_base));
                fields += "," + field("start", hex(mod.image_start));
                fields += "," + field("size", static_cast<uint64_t>(mod.size_of_image));
                emit_event("module", fields);
            });
        }

        bool suppress_details = false;

        // Installed whether or not the trace is wanted: on_syscall runs for bsd calls and mach traps
        // alike, and it is the only recurring point the browser build has. The basic-block hook that
        // would otherwise pace this cannot be registered in wasm, so a guest between two syscalls is a
        // guest the page hears nothing from.
        emu->callbacks.on_syscall = [&](const uint64_t id, const std::string_view name) {
            if (stream)
            {
                // Counted whether or not the trace is on: with it off the page would otherwise have no
                // measure of progress at all, and a long run is exactly the one made with it off.
                stream->count_syscall();
                stream->tick();
            }

            if (!options.trace_syscalls)
            {
                return sogen::instruction_hook_continuation::run_instruction;
            }

            suppress_details = options.ignored.contains(name);
            if (suppress_details)
            {
                return sogen::instruction_hook_continuation::run_instruction;
            }

            std::string fields = field("name", name);
            fields += "," + field("id", id);
            fields += "," + field("pc", hex(emu->emu().read_instruction_pointer()));
            emit_event("syscall", fields);
            return sogen::instruction_hook_continuation::run_instruction;
        };

        if (options.trace_syscalls)
        {
            emu->callbacks.on_trace_detail = [&](const sogen::macos_trace_detail& detail) {
                if (suppress_details)
                {
                    return;
                }

                emit_event("detail", field("label", detail.label) + "," + field("value", detail.value));
            };

            emu->callbacks.on_syscall_error = [&](std::string_view, const int64_t error, const std::string_view name) {
                if (suppress_details)
                {
                    return;
                }

                emit_event("failed", field("errno", static_cast<uint64_t>(error)) + "," + field("name", name));
            };
        }

        emit_event("started", field("backend", emu->emu().get_name()) + "," + field("exe", options.executable));

        if (!emu->load_executable(options.executable, options.argv, options.envp))
        {
            emit_event("fatal",
                       field("message", emu->last_stop_detail()) + "," + field("reason", sogen::stop_reason_name(emu->last_stop_reason())));
            return 1;
        }

        emu->start(static_cast<size_t>(options.max_instructions));

        if (stream)
        {
            stream->finish();
        }

        // Emitted as events rather than left to the logger: these two are the only account of why a launch
        // failed, and --log is off by default.
        const auto dyld_error = emu->dyld_error_message();
        if (!dyld_error.empty())
        {
            emit_event("dyld-error", field("message", dyld_error));
        }

        const auto status = emu->process.exit_status;

        if (status.has_value() && *status == 6)
        {
            for (const auto& frame : emu->backtrace())
            {
                emit_event("frame", field("text", frame));
            }
        }

        if (options.memory_report)
        {
            const auto report = sogen::collect_macos_memory_report(emu->memory);
            std::string fields = field("committed", report.guest_committed_bytes);
            fields += "," + field("reserved", report.guest_reserved_bytes);
            fields += "," + field("regions", report.guest_region_count);
            fields += "," + field("host", report.host_heap_bytes);
            emit_event("memory", fields);
        }

        // Reported as a presence flag rather than a bare zero: the browser build cannot register the
        // basic-block hook that maintains the tally, and printing 0 would read as "nothing executed".
        std::string counters = field("counted", static_cast<uint64_t>(emu->counts_executed_instructions() ? 1 : 0));
        if (emu->counts_executed_instructions())
        {
            counters += "," + field("instructions", emu->get_executed_instructions());
            counters += "," + field("blocks", emu->get_executed_basic_blocks());
        }
        emit_event("counters", counters);

        if (status.has_value())
        {
            emit_event("exited", field("status", static_cast<uint64_t>(static_cast<uint32_t>(*status))));
            return 0;
        }

        emit_event("stopped", field("reason", sogen::stop_reason_name(emu->last_stop_reason())) + "," +
                                  field("detail", emu->last_stop_detail()) + "," + field("pc", hex(emu->emu().read_instruction_pointer())));
        return 1;
    }
}

namespace
{
    int run_command(const int argc, char** argv)
    {
        for (int i = 1; i < argc; ++i)
        {
            std::string_view value{};
            if (matches(std::string_view{argv[i]}, "--probe-range", value))
            {
                // path:offset:size
                const std::string spec{value};
                const auto first = spec.rfind(':');
                const auto second = first == std::string::npos ? std::string::npos : spec.rfind(':', first - 1);
                if (first == std::string::npos || second == std::string::npos)
                {
                    emit_event("fatal", field("message", "--probe-range wants path:offset:size"));
                    return 1;
                }

                return probe_range(spec.substr(0, second), std::strtoull(spec.substr(second + 1, first - second - 1).c_str(), nullptr, 10),
                                   static_cast<size_t>(std::strtoull(spec.substr(first + 1).c_str(), nullptr, 10)));
            }
        }

        const auto options = parse_options(argc, argv);

        try
        {
            describe_image(std::filesystem::path{options.root} / std::filesystem::path{options.executable}.relative_path());
            return run(options);
        }
        catch (const std::exception& error)
        {
            emit_event("fatal", field("message", error.what()));
            return 1;
        }
    }
}

int main(const int argc, char** argv)
{
    const auto code = run_command(argc, argv);

    // callMain cannot report this once anything in the run has yielded: ASYNCIFY makes it return
    // undefined at the first unwind and the real value only surfaces through Asyncify.whenDone().
    sogen_web_finished(static_cast<double>(code));
    return code;
}
