#include "../std_include.hpp"
#include "macos_cf_bridge.hpp"

#include "macos_guest_call.hpp"
#include "macos_native_dispatch.hpp"
#include "macos_window_server.hpp"
#include "../host_range_reader.hpp"
#include "../macos_emulator.hpp"
#include "../module/macos_cache_symbols.hpp"

#include <platform/unicode.hpp>

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <set>

namespace sogen
{
    namespace
    {
        constexpr std::string_view CORE_FOUNDATION_IMAGE_PATH = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/"
                                                                "CoreFoundation";

        constexpr uint64_t SCRATCH_ALIGNMENT = 8;

        // sogen's single console session. The guest asks who is logged in; nothing about the host may
        // leak into the answer, so these are values sogen owns, the way MACOS_MAIN_CONNECTION_ID is.
        constexpr int64_t SESSION_ID = 257;
        constexpr int64_t SESSION_AUDIT_ID = 100000;
        constexpr std::string_view SESSION_USER_NAME = "sogen";
        constexpr std::string_view SESSION_LONG_USER_NAME = "sogen";
        constexpr std::string_view SESSION_UUID = "00000000-0000-4000-8000-000000000001";

        // Measured 25G76 on an ordinary titled app window; see
        constexpr int64_t WINDOW_STORE_TYPE = 1;
        constexpr int64_t WINDOW_SHARING_STATE = 1;

        struct compiler
        {
            const macos_cf_symbols& symbols;
            macos_cf_program program{};
            std::map<std::string, int32_t> strings{};
            std::vector<int32_t> owned{};

            int32_t add_slot(const uint64_t initial)
            {
                this->program.initial_slots.push_back(initial);
                return static_cast<int32_t>(this->program.initial_slots.size() - 1);
            }

            uint64_t add_scratch(const void* bytes, const size_t size)
            {
                while (this->program.scratch.size() % SCRATCH_ALIGNMENT != 0)
                {
                    this->program.scratch.push_back(0);
                }

                const auto offset = this->program.scratch.size();
                const auto* source = static_cast<const uint8_t*>(bytes);
                this->program.scratch.insert(this->program.scratch.end(), source, source + size);
                return offset;
            }

            void emit(const std::string_view name, const uint64_t function, const std::vector<macos_cf_argument>& args,
                      const int32_t result_slot)
            {
                macos_cf_step step{.function = function, .name = name, .result_slot = result_slot};
                step.argument_count = std::min(args.size(), step.args.size());
                std::copy_n(args.begin(), step.argument_count, step.args.begin());
                this->program.steps.push_back(step);
            }

            static macos_cf_argument literal(const uint64_t value)
            {
                return {.from = macos_cf_argument::source::literal, .value = value};
            }

            static macos_cf_argument slot(const int32_t index)
            {
                return {.from = macos_cf_argument::source::slot, .value = static_cast<uint64_t>(index)};
            }

            static macos_cf_argument scratch(const uint64_t offset)
            {
                return {.from = macos_cf_argument::source::scratch, .value = offset};
            }

            int32_t emit_string(const std::string& text)
            {
                const auto found = this->strings.find(text);
                if (found != this->strings.end())
                {
                    return found->second;
                }

                const auto offset = this->add_scratch(text.data(), text.size());
                const auto result = this->add_slot(0);

                this->emit("CFStringCreateWithBytes", this->symbols.string_create_with_bytes,
                           {literal(0), scratch(offset), literal(text.size()), literal(MACOS_CF_STRING_ENCODING_UTF8), literal(0)}, result);

                this->strings.emplace(text, result);
                this->owned.push_back(result);
                return result;
            }

            int32_t emit_number(const uint64_t type, const void* bytes)
            {
                const auto offset = this->add_scratch(bytes, sizeof(uint64_t));
                const auto result = this->add_slot(0);
                this->emit("CFNumberCreate", this->symbols.number_create, {literal(0), literal(type), scratch(offset)}, result);
                this->owned.push_back(result);
                return result;
            }

            int32_t emit(const macos_cf_value& value)
            {
                switch (value.type())
                {
                case macos_cf_value::kind::string:
                    return this->emit_string(value.text());

                case macos_cf_value::kind::integer: {
                    const auto raw = value.integer_value();
                    return this->emit_number(MACOS_CF_NUMBER_SINT64, &raw);
                }

                case macos_cf_value::kind::real: {
                    const auto raw = value.real_value();
                    return this->emit_number(MACOS_CF_NUMBER_FLOAT64, &raw);
                }

                case macos_cf_value::kind::boolean:
                    return this->add_slot(value.boolean_value() ? this->symbols.boolean_true : this->symbols.boolean_false);

                case macos_cf_value::kind::array: {
                    const auto result = this->add_slot(0);
                    this->emit("CFArrayCreateMutable", this->symbols.array_create_mutable,
                               {literal(0), literal(0), literal(this->symbols.type_array_callbacks)}, result);
                    this->owned.push_back(result);

                    for (const auto& element : value.elements())
                    {
                        const auto child = this->emit(element);
                        this->emit("CFArrayAppendValue", this->symbols.array_append_value, {slot(result), slot(child)}, -1);
                    }

                    return result;
                }

                case macos_cf_value::kind::dictionary: {
                    const auto result = this->add_slot(0);
                    this->emit("CFDictionaryCreateMutable", this->symbols.dictionary_create_mutable,
                               {literal(0), literal(0), literal(this->symbols.type_dictionary_key_callbacks),
                                literal(this->symbols.type_dictionary_value_callbacks)},
                               result);
                    this->owned.push_back(result);

                    for (const auto& [key, entry] : value.entries())
                    {
                        const auto key_slot = this->emit_string(key);
                        const auto value_slot = this->emit(entry);
                        this->emit("CFDictionarySetValue", this->symbols.dictionary_set_value,
                                   {slot(result), slot(key_slot), slot(value_slot)}, -1);
                    }

                    return result;
                }
                }

                return -1;
            }
        };

        struct run_state
        {
            macos_cf_program program{};
            std::vector<uint64_t> slots{};
            size_t next_step{};
            uint64_t scratch_base{};
            macos_cf_completion done{};
        };

        // Recycled rather than retired: CFStringCreateWithBytes and CFNumberCreate copy what they are
        // given -- the NoCopy variants exist because the plain ones do -- so no reference to the block
        // outlives the step that read it. Handing it back to the arena is also what keeps the block
        // count at one: the program runs a step at a time on the serialised guest-call stack, so the
        // next program takes the same block instead of churning a fresh mapping per call.
        void release_scratch(macos_emulator& emu, run_state& state)
        {
            if (state.scratch_base != 0)
            {
                emu.ui.arena.recycle(state.scratch_base);
                state.scratch_base = 0;
            }
        }

        void finish(macos_emulator& emu, const std::shared_ptr<run_state>& state, const uint64_t root)
        {
            release_scratch(emu, *state);

            if (state->done)
            {
                auto done = std::move(state->done);
                state->done = {};
                done(emu, root);
            }
        }

        bool any_null_slot(const run_state& state, const macos_cf_step& step)
        {
            for (size_t i = 0; i < step.argument_count; ++i)
            {
                const auto& argument = step.args.at(i);
                if (argument.from == macos_cf_argument::source::slot && state.slots.at(static_cast<size_t>(argument.value)) == 0)
                {
                    return true;
                }
            }

            return false;
        }

        void run_next_step(macos_emulator& emu, std::shared_ptr<run_state> state);

        bool start_step(macos_emulator& emu, const std::shared_ptr<run_state>& state, const macos_cf_step& step)
        {
            auto* calls = emu.guest_call_stack();
            if (calls == nullptr)
            {
                return false;
            }

            macos_guest_call_request request{.function = step.function};

            for (size_t i = 0; i < step.argument_count; ++i)
            {
                const auto& argument = step.args.at(i);
                switch (argument.from)
                {
                case macos_cf_argument::source::literal:
                    request.args.at(i) = argument.value;
                    break;

                case macos_cf_argument::source::slot:
                    request.args.at(i) = state->slots.at(static_cast<size_t>(argument.value));
                    break;

                case macos_cf_argument::source::scratch:
                    request.args.at(i) = state->scratch_base + argument.value;
                    break;
                }
            }

            const auto result_slot = step.result_slot;
            request.on_return = [state, result_slot](macos_emulator& inner, const uint64_t result) {
                if (result_slot >= 0)
                {
                    state->slots.at(static_cast<size_t>(result_slot)) = result;
                }

                run_next_step(inner, state);
            };

            return calls->begin(emu, std::move(request));
        }

        void run_next_step(macos_emulator& emu, std::shared_ptr<run_state> state)
        {
            while (state->next_step < state->program.steps.size())
            {
                const auto& step = state->program.steps.at(state->next_step);
                ++state->next_step;

                // A store or a release naming an object that never materialised would hand CoreFoundation
                // a null CFTypeRef, which it dereferences. Skipping keeps one failed create from becoming
                // a guest crash; the create that failed is what reports the problem.
                if (step.result_slot < 0 && any_null_slot(*state, step))
                {
                    continue;
                }

                if (start_step(emu, state, step))
                {
                    return;
                }

                emu.log.warn("CoreFoundation bridge could not call %.*s in the guest; the container is abandoned\n",
                             static_cast<int>(step.name.size()), step.name.data());
                finish(emu, state, 0);
                return;
            }

            const auto root = state->program.root_slot >= 0 ? state->slots.at(static_cast<size_t>(state->program.root_slot)) : 0;
            finish(emu, state, root);
        }

        std::optional<macos_cf_symbols> resolve_addresses(macos_emulator& emu)
        {
            static std::map<std::string, std::optional<macos_cf_symbols>> resolved{};

            const auto key = emu.shared_cache_host_path.string();
            if (key.empty())
            {
                return std::nullopt;
            }

            const auto cached = resolved.find(key);
            if (cached != resolved.end())
            {
                return cached->second;
            }

            std::optional<dyld_shared_cache_reader> cache{};
            try
            {
                cache.emplace(
                    dyld_shared_cache_reader::parse(emu.shared_cache_host_path, make_host_range_cache_opener(default_host_range_reader())));
            }
            catch (const std::exception& e)
            {
                emu.log.warn("CoreFoundation bridge needs the shared cache and could not read %s: %s\n", key.c_str(), e.what());
                resolved.emplace(key, std::nullopt);
                return std::nullopt;
            }

            const macos_cache_symbols symbols{*cache};
            bool missing = false;

            const auto find = [&](const std::string_view name) {
                const auto address = symbols.find_export(CORE_FOUNDATION_IMAGE_PATH, name);
                if (!address.has_value())
                {
                    emu.log.warn("CoreFoundation does not export %.*s on this system; CF containers cannot be built\n",
                                 static_cast<int>(name.size()), name.data());
                    missing = true;
                    return uint64_t{0};
                }

                return *address;
            };

            macos_cf_symbols found{};
            found.array_create_mutable = find("_CFArrayCreateMutable");
            found.array_append_value = find("_CFArrayAppendValue");
            found.dictionary_create_mutable = find("_CFDictionaryCreateMutable");
            found.dictionary_set_value = find("_CFDictionarySetValue");
            found.string_create_with_bytes = find("_CFStringCreateWithBytes");
            found.number_create = find("_CFNumberCreate");
            found.release = find("_CFRelease");
            found.type_array_callbacks = find("_kCFTypeArrayCallBacks");
            found.type_dictionary_key_callbacks = find("_kCFTypeDictionaryKeyCallBacks");
            found.type_dictionary_value_callbacks = find("_kCFTypeDictionaryValueCallBacks");

            // Held as the addresses of the variables here; macos_cf_resolve turns them into the
            // singletons by reading guest memory, which only makes sense once CF has initialised.
            found.boolean_true = find("_kCFBooleanTrue");
            found.boolean_false = find("_kCFBooleanFalse");

            std::optional<macos_cf_symbols> answer{};
            if (!missing)
            {
                answer = found;
            }

            resolved.emplace(key, answer);
            return answer;
        }
    }

    macos_cf_value macos_cf_value::string(std::string text)
    {
        macos_cf_value value{};
        value.kind_ = kind::string;
        value.text_ = std::move(text);
        return value;
    }

    macos_cf_value macos_cf_value::integer(const int64_t number)
    {
        macos_cf_value value{};
        value.kind_ = kind::integer;
        value.integer_ = number;
        return value;
    }

    macos_cf_value macos_cf_value::real(const double number)
    {
        macos_cf_value value{};
        value.kind_ = kind::real;
        value.real_ = number;
        return value;
    }

    macos_cf_value macos_cf_value::boolean(const bool flag)
    {
        macos_cf_value value{};
        value.kind_ = kind::boolean;
        value.integer_ = flag ? 1 : 0;
        return value;
    }

    macos_cf_value macos_cf_value::array()
    {
        macos_cf_value value{};
        value.kind_ = kind::array;
        return value;
    }

    macos_cf_value macos_cf_value::dictionary()
    {
        macos_cf_value value{};
        value.kind_ = kind::dictionary;
        return value;
    }

    macos_cf_value& macos_cf_value::append(macos_cf_value value)
    {
        this->elements_.push_back(std::move(value));
        return *this;
    }

    macos_cf_value& macos_cf_value::set(std::string key, macos_cf_value value)
    {
        for (auto& entry : this->entries_)
        {
            if (entry.first == key)
            {
                entry.second = std::move(value);
                return *this;
            }
        }

        this->entries_.emplace_back(std::move(key), std::move(value));
        return *this;
    }

    const macos_cf_value* macos_cf_value::find(const std::string_view key) const
    {
        for (const auto& entry : this->entries_)
        {
            if (entry.first == key)
            {
                return &entry.second;
            }
        }

        return nullptr;
    }

    bool macos_cf_symbols::complete() const
    {
        return this->array_create_mutable != 0 && this->array_append_value != 0 && this->dictionary_create_mutable != 0 &&
               this->dictionary_set_value != 0 && this->string_create_with_bytes != 0 && this->number_create != 0 && this->release != 0 &&
               this->type_array_callbacks != 0 && this->type_dictionary_key_callbacks != 0 && this->type_dictionary_value_callbacks != 0 &&
               this->boolean_true != 0 && this->boolean_false != 0;
    }

    macos_cf_program macos_cf_compile(const macos_cf_symbols& symbols, const macos_cf_value& value)
    {
        if (!symbols.complete())
        {
            return {};
        }

        compiler state{.symbols = symbols};
        const auto root = state.emit(value);
        if (root < 0)
        {
            return {};
        }

        // The containers retain what is put into them, and the caller of a Copy function owns the
        // result, so every reference this program created except the root has to be given back. Newest
        // first, which is the order the guest's own code would unwind in.
        for (auto slot = state.owned.rbegin(); slot != state.owned.rend(); ++slot)
        {
            if (*slot != root)
            {
                state.emit("CFRelease", symbols.release, {compiler::slot(*slot)}, -1);
            }
        }

        state.program.root_slot = root;
        return std::move(state.program);
    }

    std::optional<macos_cf_symbols> macos_cf_resolve(macos_emulator& emu)
    {
        auto addresses = resolve_addresses(emu);
        if (!addresses.has_value())
        {
            return std::nullopt;
        }

        const auto read_singleton = [&](const uint64_t address) {
            uint64_t singleton = 0;
            return emu.memory.try_read_memory(address, &singleton, sizeof(singleton)) ? singleton : uint64_t{0};
        };

        addresses->boolean_true = read_singleton(addresses->boolean_true);
        addresses->boolean_false = read_singleton(addresses->boolean_false);

        if (!addresses->complete())
        {
            static bool reported = false;
            if (!std::exchange(reported, true))
            {
                emu.log.warn("kCFBooleanTrue/kCFBooleanFalse are not readable yet; CoreFoundation has not initialised in this guest\n");
            }

            return std::nullopt;
        }

        return addresses;
    }

    bool macos_cf_run(macos_emulator& emu, macos_cf_program program, macos_cf_completion done)
    {
        if (!program.valid() || emu.guest_call_stack() == nullptr)
        {
            return false;
        }

        auto state = std::make_shared<run_state>();
        state->slots = program.initial_slots;
        state->done = std::move(done);

        if (!program.scratch.empty())
        {
            state->scratch_base = emu.ui.arena.acquire(emu, program.scratch.size());

            if (state->scratch_base == 0 ||
                !emu.memory.try_write_memory(state->scratch_base, program.scratch.data(), program.scratch.size()))
            {
                release_scratch(emu, *state);
                emu.log.warn("No room in the GUI arena for a %zu byte CoreFoundation scratch block\n", program.scratch.size());
                return false;
            }
        }

        state->program = std::move(program);

        run_next_step(emu, std::move(state));
        return true;
    }

    bool macos_cf_build(macos_emulator& emu, const macos_cf_value& value, macos_cf_completion done)
    {
        const auto symbols = macos_cf_resolve(emu);
        if (!symbols.has_value())
        {
            return false;
        }

        return macos_cf_run(emu, macos_cf_compile(*symbols, value), std::move(done));
    }

    macos_cf_value macos_cf_window_list(const macos_window_server& server, const uint32_t option, const uint32_t relative_to,
                                        const uint32_t owner_pid, const std::string_view owner_name)
    {
        const auto& windows = server.windows();

        size_t pivot = windows.size();
        for (size_t i = 0; i < windows.size(); ++i)
        {
            if (windows[i].id == relative_to)
            {
                pivot = i;
            }
        }

        auto list = macos_cf_value::array();

        for (size_t i = 0; i < windows.size(); ++i)
        {
            const auto& window = windows[i];

            // The three OnScreen options all mean on screen; only kCGWindowListOptionAll and the
            // including-window form reach a window that is not ordered in.
            constexpr auto on_screen_options =
                MACOS_CG_WINDOW_LIST_ON_SCREEN_ONLY | MACOS_CG_WINDOW_LIST_ON_SCREEN_ABOVE | MACOS_CG_WINDOW_LIST_ON_SCREEN_BELOW;

            if ((option & on_screen_options) != 0 && !window.ordered_in)
            {
                continue;
            }

            if ((option & MACOS_CG_WINDOW_LIST_INCLUDING_WINDOW) != 0 && window.id != relative_to)
            {
                continue;
            }

            // Above and below are relative to the window named in the second argument, which is why a
            // pivot that is not in the list leaves both filters selecting nothing.
            if ((option & MACOS_CG_WINDOW_LIST_ON_SCREEN_ABOVE) != 0 && (pivot >= windows.size() || i <= pivot))
            {
                continue;
            }

            if ((option & MACOS_CG_WINDOW_LIST_ON_SCREEN_BELOW) != 0 && (pivot >= windows.size() || i >= pivot))
            {
                continue;
            }

            auto bounds = macos_cf_value::dictionary();
            bounds.set("X", macos_cf_value::real(static_cast<double>(window.x)));
            bounds.set("Y", macos_cf_value::real(static_cast<double>(window.y)));
            bounds.set("Width", macos_cf_value::real(static_cast<double>(window.width)));
            bounds.set("Height", macos_cf_value::real(static_cast<double>(window.height)));

            auto entry = macos_cf_value::dictionary();
            entry.set("kCGWindowNumber", macos_cf_value::integer(window.id));
            entry.set("kCGWindowStoreType", macos_cf_value::integer(WINDOW_STORE_TYPE));
            entry.set("kCGWindowLayer", macos_cf_value::integer(window.level));
            entry.set("kCGWindowBounds", std::move(bounds));
            entry.set("kCGWindowSharingState", macos_cf_value::integer(WINDOW_SHARING_STATE));
            entry.set("kCGWindowAlpha", macos_cf_value::real(1.0));
            entry.set("kCGWindowOwnerPID", macos_cf_value::integer(owner_pid));
            entry.set("kCGWindowMemoryUsage", macos_cf_value::integer(static_cast<int64_t>(window.backing_bytes())));
            entry.set("kCGWindowOwnerName", macos_cf_value::string(std::string{owner_name}));
            entry.set("kCGWindowName", macos_cf_value::string(u16_to_u8(window.title)));
            entry.set("kCGWindowIsOnscreen", macos_cf_value::boolean(window.ordered_in));

            list.append(std::move(entry));
        }

        return list;
    }

    macos_cf_value macos_cf_spaces_for_windows(const uint32_t mask, const size_t window_count)
    {
        auto spaces = macos_cf_value::array();

        if (window_count > 0 && (mask & MACOS_CGS_MAIN_SPACE_PROPERTIES) == MACOS_CGS_MAIN_SPACE_PROPERTIES)
        {
            spaces.append(macos_cf_value::integer(MACOS_CGS_MAIN_SPACE_ID));
        }

        return spaces;
    }

    macos_cf_value macos_cf_session_properties(const uint32_t uid, const uint32_t gid)
    {
        auto properties = macos_cf_value::dictionary();
        properties.set("kCGSSessionIDKey", macos_cf_value::integer(SESSION_ID));
        properties.set("kCGSSessionAuditIDKey", macos_cf_value::integer(SESSION_AUDIT_ID));
        properties.set("kSCSecuritySessionID", macos_cf_value::integer(SESSION_AUDIT_ID));
        properties.set("kCGSSessionUserIDKey", macos_cf_value::integer(uid));
        properties.set("kCGSSessionGroupIDKey", macos_cf_value::integer(gid));
        properties.set("kCGSSessionUserNameKey", macos_cf_value::string(std::string{SESSION_USER_NAME}));
        properties.set("kCGSessionLongUserNameKey", macos_cf_value::string(std::string{SESSION_LONG_USER_NAME}));
        properties.set("CGSSessionUniqueSessionUUID", macos_cf_value::string(std::string{SESSION_UUID}));
        properties.set("kCGSSessionOnConsoleKey", macos_cf_value::boolean(true));
        properties.set("kCGSessionLoginDoneKey", macos_cf_value::boolean(true));
        properties.set("kCGSSessionSystemSafeBoot", macos_cf_value::boolean(false));
        properties.set("kCGSSessionLoginwindowSafeLogin", macos_cf_value::boolean(false));
        return properties;
    }

    namespace
    {
        // The value the caller sees is whatever the last continuation leaves in x0, so a handler that
        // starts a build must not also write one.
        void answer_with(const macos_native_call& call, const macos_cf_value& value)
        {
            const auto name = call.name;

            const auto started = macos_cf_build(call.emu_ref, value,
                                                [](macos_emulator& emu, const uint64_t root) { emu.emu().reg(arm64_register::x0, root); });

            if (!started)
            {
                static std::set<std::string> reported{};
                if (reported.emplace(name).second)
                {
                    call.emu_ref.log.warn("%.*s cannot answer: sogen could not reach the guest's CoreFoundation to build the "
                                          "container, so it returns NULL\n",
                                          static_cast<int>(name.size()), name.data());
                }

                call.ret(0);
            }
        }

        void sl_window_list_copy_window_info(const macos_native_call& call)
        {
            auto& emu = call.emu_ref;
            const auto option = static_cast<uint32_t>(call.arg(0));
            const auto relative_to = static_cast<uint32_t>(call.arg(1));

            answer_with(call, macos_cf_window_list(emu.ui.server, option, relative_to, emu.process.pid,
                                                   std::filesystem::path{emu.process.executable_path}.filename().string()));
        }

        void sls_copy_spaces_for_windows(const macos_native_call& call)
        {
            auto& emu = call.emu_ref;
            const auto connection = static_cast<uint32_t>(call.arg(0));
            const auto mask = static_cast<uint32_t>(call.arg(1));

            if (!emu.ui.server.has_connection(connection))
            {
                call.ret(0);
                return;
            }

            // The window list argument is a guest CFArray, and reading it would be a chain of guest
            // calls before the first one that builds anything. sogen owns the window list, and its one
            // space holds every window in it, so the answer only turns on whether there is a window at
            // all -- which is a question sogen can answer without asking the guest.
            answer_with(call, macos_cf_spaces_for_windows(mask, emu.ui.server.windows().size()));
        }

        // The query dictionary in x0 selects a session by audit/session id on a machine that has several;
        // sogen has one console session, so every query that could match resolves to it and the argument
        // is not read. The host answers NULL for a query that matches nothing, which is why a caller that
        // handles NULL exists at all.
        void sls_copy_session_properties(const macos_native_call& call)
        {
            auto& emu = call.emu_ref;
            answer_with(call, macos_cf_session_properties(emu.process.uid, emu.process.gid));
        }
    }

    void register_cf_container_routines(macos_native_dispatch& dispatch)
    {
        const std::string skylight{MACOS_SKYLIGHT_IMAGE_PATH};

        // CoreGraphics re-exports _CGWindowListCopyWindowInfo and _CGSCopySpacesForWindows from
        // SkyLight, so a re-export's trie payload is a library ordinal rather than an address and only
        // SkyLight's names can be patched.
        dispatch.register_routine(skylight, "_SLWindowListCopyWindowInfo", sl_window_list_copy_window_info);
        dispatch.register_routine(skylight, "_SLSCopySpacesForWindows", sls_copy_spaces_for_windows);
        dispatch.register_routine(skylight, "_SLSCopySessionPropertiesTemporaryBridge", sls_copy_session_properties);
    }
}
