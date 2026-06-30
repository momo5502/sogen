#include "sogen_internal.hpp"

#include <linux_emulator.hpp>
#include <disassembler.hpp>
#include <array>
#include <cstdio>
#include <limits>
#include <sstream>
#include <utility>

namespace sogen::py
{
    namespace
    {
        nb::object get_kwarg_object(const nb::kwargs& kwargs, const char* name)
        {
            return kwargs.get(name, nb::none());
        }

        template <typename T>
        T get_kwarg(const nb::kwargs& kwargs, const char* name, const T& default_value)
        {
            const auto value = get_kwarg_object(kwargs, name);
            return value.is_none() ? default_value : nb::cast<T>(value);
        }

        nb::object sequence_item(const nb::sequence& seq, const size_t index)
        {
            auto* item = PySequence_GetItem(seq.ptr(), static_cast<Py_ssize_t>(index));
            if (item == nullptr)
            {
                throw nb::python_error();
            }
            return nb::steal<nb::object>(item);
        }

        std::vector<std::string> parse_linux_arguments(const nb::object& object)
        {
            std::vector<std::string> result{};
            if (object.is_none())
            {
                return result;
            }

            const auto seq = nb::cast<nb::sequence>(object);
            result.reserve(static_cast<size_t>(nb::len(seq)));
            for (const auto& item : seq)
            {
                result.emplace_back(nb::cast<std::string>(item));
            }

            return result;
        }

        std::vector<std::string> parse_linux_environment(const nb::object& object)
        {
            std::vector<std::string> result{};
            if (object.is_none())
            {
                return {"PATH=/usr/bin:/bin", "HOME=/root", "TERM=xterm"};
            }

            const auto dict = nb::cast<nb::dict>(object);
            result.reserve(static_cast<size_t>(nb::len(dict)));
            for (const auto& item : dict)
            {
                result.emplace_back(nb::cast<std::string>(item.first) + "=" + nb::cast<std::string>(item.second));
            }

            return result;
        }

        uint16_t parse_linux_port(const nb::handle value)
        {
            const auto port = nb::cast<uint32_t>(value);
            if (port == 0 || port > std::numeric_limits<uint16_t>::max())
            {
                throw std::runtime_error("Linux port mappings require ports in range 1..65535");
            }
            return static_cast<uint16_t>(port);
        }

        void apply_linux_path_mappings(linux_emulator& emu, const nb::object& object, const bool read_only)
        {
            if (object.is_none())
            {
                return;
            }

            if (nb::isinstance<nb::dict>(object))
            {
                const auto dict = nb::cast<nb::dict>(object);
                for (const auto& item : dict)
                {
                    emu.file_sys.add_path_mapping(nb::cast<std::filesystem::path>(item.first), nb::cast<std::filesystem::path>(item.second),
                                                  read_only);
                }
                return;
            }

            const auto seq = nb::cast<nb::sequence>(object);
            for (const auto& item : seq)
            {
                const auto pair = nb::cast<nb::sequence>(item);
                if (nb::len(pair) != 2)
                {
                    throw std::runtime_error("Linux path mappings must be {guest: host} or [(guest, host), ...]");
                }
                emu.file_sys.add_path_mapping(nb::cast<std::filesystem::path>(sequence_item(pair, 0)),
                                              nb::cast<std::filesystem::path>(sequence_item(pair, 1)), read_only);
            }
        }

        void apply_linux_port_mappings(linux_emulator& emu, const nb::object& object)
        {
            if (object.is_none())
            {
                return;
            }

            if (nb::isinstance<nb::dict>(object))
            {
                const auto dict = nb::cast<nb::dict>(object);
                for (const auto& item : dict)
                {
                    emu.port_mapper.map_port(parse_linux_port(item.first), parse_linux_port(item.second));
                }
                return;
            }

            const auto seq = nb::cast<nb::sequence>(object);
            for (const auto& item : seq)
            {
                const auto pair = nb::cast<nb::sequence>(item);
                if (nb::len(pair) != 2)
                {
                    throw std::runtime_error("Linux port mappings must be {emulator_port: host_port} or [(emulator_port, host_port), ...]");
                }
                emu.port_mapper.map_port(parse_linux_port(sequence_item(pair, 0)), parse_linux_port(sequence_item(pair, 1)));
            }
        }

        struct linux_symbol_hook_target
        {
            std::optional<std::string> module{};
            std::string name{};
        };

        linux_symbol_hook_target parse_linux_symbol_hook_target(const std::string& key)
        {
            const auto pos = key.find('!');
            if (pos == std::string::npos)
            {
                return {.module = std::nullopt, .name = key};
            }

            return {.module = key.substr(0, pos), .name = key.substr(pos + 1)};
        }

        bool linux_symbol_module_matches(const linux_symbol_hook_entry& entry, const linux_mapped_module& module)
        {
            if (!entry.module_filter.has_value())
            {
                return true;
            }

            return *entry.module_filter == module.name || *entry.module_filter == module.path.stem().string();
        }

        void validate_linux_symbol_ctype(const nb::handle ctype, const std::string_view role)
        {
            if (ctype.is_none())
            {
                return;
            }

            const auto ctypes = nb::module_::import_("ctypes");
            size_t size{};
            try
            {
                size = nb::cast<size_t>(ctypes.attr("sizeof")(ctype));
            }
            catch (...)
            {
                PyErr_Clear();
                throw std::runtime_error("Linux symbol hook " + std::string(role) + " must be a ctypes scalar or pointer type");
            }

            if (size > sizeof(uint64_t))
            {
                throw std::runtime_error("Linux symbol hook " + std::string(role) + " ctypes value is wider than 8 bytes");
            }

            bool is_pointer = false;
            if (nb::hasattr(ctypes, "_Pointer"))
            {
                const int subclass = PyObject_IsSubclass(ctype.ptr(), ctypes.attr("_Pointer").ptr());
                if (subclass == 1)
                {
                    is_pointer = true;
                }
                else if (subclass < 0)
                {
                    PyErr_Clear();
                }
            }

            if (is_pointer)
            {
                return;
            }

            if (!nb::hasattr(ctype, "_type_"))
            {
                throw std::runtime_error("Linux symbol hook " + std::string(role) + " must be an integer or pointer ctypes type");
            }

            const auto type_code_obj = nb::getattr(ctype, "_type_");
            if (!nb::isinstance<nb::str>(type_code_obj))
            {
                throw std::runtime_error("Linux symbol hook " + std::string(role) + " must be an integer or pointer ctypes type");
            }

            const auto type_code = nb::cast<std::string>(type_code_obj);
            static constexpr std::string_view allowed_codes{"?bBhHiIlLqQnNPzZc"};
            if (type_code.size() != 1 || allowed_codes.find(type_code.front()) == std::string_view::npos)
            {
                throw std::runtime_error("Linux symbol hook " + std::string(role) + " must be an integer or pointer ctypes type");
            }
        }

        void validate_linux_symbol_signature(const nb::object& params, const nb::object& restype)
        {
            if (!params.is_none())
            {
                const auto seq = nb::cast<nb::sequence>(params);
                for (const auto& item : seq)
                {
                    validate_linux_symbol_ctype(item, "parameter");
                }
            }

            validate_linux_symbol_ctype(restype, "return");
        }

        std::pair<nb::object, nb::object> read_linux_symbol_signature(const nb::object& callback)
        {
            nb::object params = nb::none();
            nb::object restype = nb::none();
            if (nb::hasattr(callback, "_sogen_linux_symbol_params"))
            {
                params = nb::getattr(callback, "_sogen_linux_symbol_params");
            }
            if (nb::hasattr(callback, "_sogen_linux_symbol_restype"))
            {
                restype = nb::getattr(callback, "_sogen_linux_symbol_restype");
            }

            validate_linux_symbol_signature(params, restype);
            return {std::move(params), std::move(restype)};
        }

        bool linux_symbol_ctype_is_pointer(const nb::handle ctype)
        {
            const auto ctypes = nb::module_::import_("ctypes");
            if (nb::hasattr(ctypes, "_Pointer"))
            {
                const int subclass = PyObject_IsSubclass(ctype.ptr(), ctypes.attr("_Pointer").ptr());
                if (subclass == 1)
                {
                    return true;
                }
                if (subclass < 0)
                {
                    PyErr_Clear();
                }
            }

            if (!nb::hasattr(ctype, "_type_"))
            {
                return false;
            }

            const auto type_code_obj = nb::getattr(ctype, "_type_");
            if (!nb::isinstance<nb::str>(type_code_obj))
            {
                return false;
            }

            const auto type_code = nb::cast<std::string>(type_code_obj);
            return type_code == "P" || type_code == "z" || type_code == "Z";
        }

        nb::object decode_linux_symbol_param(const nb::handle ctype, uint64_t value)
        {
            if (ctype.is_none())
            {
                return nb::int_(value);
            }

            const auto ctypes = nb::module_::import_("ctypes");
            const auto size = nb::cast<size_t>(ctypes.attr("sizeof")(ctype));
            if (size == 0)
            {
                return nb::int_(0);
            }

            if (linux_symbol_ctype_is_pointer(ctype))
            {
                return nb::int_(value);
            }

            const auto type_code = nb::cast<std::string>(nb::getattr(ctype, "_type_"));
            const auto bits = size * 8;
            const auto mask = bits >= 64 ? std::numeric_limits<uint64_t>::max() : ((uint64_t{1} << bits) - 1);
            value &= mask;

            if (type_code == "?")
            {
                return nb::bool_(value != 0);
            }

            if (type_code == "c")
            {
                const char ch = static_cast<char>(value & 0xFF);
                return nb::bytes(&ch, 1);
            }

            static constexpr std::string_view signed_codes{"bhiqnl"};
            if (type_code.size() == 1 && signed_codes.find(type_code.front()) != std::string_view::npos)
            {
                if (bits >= 64)
                {
                    return nb::int_(static_cast<int64_t>(value));
                }

                const auto sign_bit = uint64_t{1} << (bits - 1);
                const auto signed_value = static_cast<int64_t>((value ^ sign_bit) - sign_bit);
                return nb::int_(signed_value);
            }

            return nb::int_(value);
        }

        backend_type get_backend_type(const nb::kwargs& kwargs)
        {
            return get_kwarg<backend_type>(kwargs, "backend", backend_type::unicorn);
        }

        std::string hex_address(const uint64_t address)
        {
            std::ostringstream stream{};
            stream << "0x" << std::hex << address;
            return stream.str();
        }

        nb::dict linux_module_dict(const linux_mapped_module& module)
        {
            nb::dict result{};
            set_dict_item(result, "name", module.name);
            set_dict_item(result, "path", module.path.string());
            set_dict_item(result, "image_base", module.image_base);
            set_dict_item(result, "size_of_image", module.size_of_image);
            set_dict_item(result, "entry_point", module.entry_point);
            return result;
        }

        std::string linux_module_name_for(linux_emulator& emu, const uint64_t address)
        {
            const auto* module = emu.mod_manager.find_by_address(address);
            return module ? module->name : std::string{};
        }
    }

    std::unique_ptr<linux_emulator> create_empty_linux_emulator(const nb::kwargs& kwargs)
    {
        auto linux_emu = std::make_unique<linux_emulator>(create_x86_64_emulator(get_backend_type(kwargs)),
                                                          get_kwarg<std::filesystem::path>(kwargs, "emulation_root", {}));
        apply_linux_path_mappings(*linux_emu, get_kwarg_object(kwargs, "path_mappings"), false);
        apply_linux_path_mappings(*linux_emu, get_kwarg_object(kwargs, "read_only_path_mappings"), true);
        apply_linux_port_mappings(*linux_emu, get_kwarg_object(kwargs, "port_mappings"));
        linux_emu->log.disable_output(get_kwarg<bool>(kwargs, "disable_logging", true));
        return linux_emu;
    }

    std::unique_ptr<linux_emulator> create_linux_application_emulator(const nb::object& application, const nb::object& args,
                                                                      const nb::kwargs& kwargs)
    {
        const auto application_path = nb::cast<std::filesystem::path>(application);
        auto argv = std::vector<std::string>{application_path.string()};
        auto application_args = parse_linux_arguments(args);
        for (auto& arg : application_args)
        {
            argv.emplace_back(std::move(arg));
        }

        auto envp = parse_linux_environment(get_kwarg_object(kwargs, "environment"));
        auto linux_emu = std::make_unique<linux_emulator>(create_x86_64_emulator(get_backend_type(kwargs)),
                                                          get_kwarg<std::filesystem::path>(kwargs, "emulation_root", {}));
        linux_emu->process.current_working_directory = linux_file_system::normalize_guest_path_string(
            get_kwarg<std::filesystem::path>(kwargs, "working_directory", std::filesystem::path{"/"}).string());
        apply_linux_path_mappings(*linux_emu, get_kwarg_object(kwargs, "path_mappings"), false);
        apply_linux_path_mappings(*linux_emu, get_kwarg_object(kwargs, "read_only_path_mappings"), true);
        apply_linux_port_mappings(*linux_emu, get_kwarg_object(kwargs, "port_mappings"));
        linux_emu->load_application(application_path, std::move(argv), envp);
        linux_emu->log.disable_output(get_kwarg<bool>(kwargs, "disable_logging", true));
        return linux_emu;
    }

    namespace
    {
        sogen_linux_thread_snapshot snapshot_linux_thread(const linux_thread& thread)
        {
            return sogen_linux_thread_snapshot{
                .tid = thread.tid,
                .stack_base = thread.stack_base,
                .stack_size = thread.stack_size,
                .fs_base = thread.fs_base,
                .current_ip = thread.saved_regs.rip,
                .start_address = thread.saved_regs.rip,
                .wait_state = thread.wait_state,
                .terminated = thread.terminated,
                .exit_code = thread.exit_code,
                .executed_instructions = thread.executed_instructions,
            };
        }
    }

    sogen_linux_thread::sogen_linux_thread(linux_thread& thread, linux_emulator& emulator, nb::object owner, bool resolve_live)
        : tid(thread.tid),
          emu(&emulator),
          owner(std::move(owner)),
          snapshot(snapshot_linux_thread(thread)),
          resolve_live(resolve_live)
    {
    }

    const linux_thread* sogen_linux_thread::live_thread() const
    {
        if (!this->resolve_live || !this->emu)
        {
            return nullptr;
        }

        const auto it = this->emu->process.threads.find(this->tid);
        if (it == this->emu->process.threads.end())
        {
            return nullptr;
        }

        return &it->second;
    }

    sogen_linux_thread_snapshot sogen_linux_thread::view() const
    {
        if (const auto* thread = this->live_thread())
        {
            return snapshot_linux_thread(*thread);
        }

        return this->snapshot;
    }

    uint64_t sogen_linux_thread::current_ip() const
    {
        const auto* thread = this->live_thread();
        if (!thread)
        {
            return this->snapshot.current_ip;
        }

        if (this->emu && this->emu->process.active_thread && this->emu->process.active_thread->tid == this->tid)
        {
            return this->emu->emu().reg<uint64_t>(x86_register::rip);
        }

        return thread->saved_regs.rip;
    }

    // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
    nb::object sogen_linux_thread::previous_ip() const
    {
        PyErr_SetString(PyExc_NotImplementedError, "sogen.linux.Thread.previous_ip is not tracked on Linux yet; use current_ip, "
                                                   "debug.call_stack(), or debug.run_to(address) for explicit control-flow navigation");
        throw nb::python_error();
    }

    sogen_linux_process_context::sogen_linux_process_context(linux_process_context& context, linux_emulator& emulator, nb::object owner)
        : ctx(&context),
          emu(&emulator),
          owner(std::move(owner))
    {
    }

    std::optional<int> sogen_linux_process_context::exit_status() const
    {
        return this->ctx->exit_status;
    }

    uint32_t sogen_linux_process_context::pid() const
    {
        return this->ctx->pid;
    }

    uint32_t sogen_linux_process_context::ppid() const
    {
        return this->ctx->ppid;
    }

    uint32_t sogen_linux_process_context::uid() const
    {
        return this->ctx->uid;
    }

    uint32_t sogen_linux_process_context::gid() const
    {
        return this->ctx->gid;
    }

    uint32_t sogen_linux_process_context::euid() const
    {
        return this->ctx->euid;
    }

    uint32_t sogen_linux_process_context::egid() const
    {
        return this->ctx->egid;
    }

    size_t sogen_linux_process_context::thread_count() const
    {
        return this->ctx->threads.size();
    }

    nb::object sogen_linux_process_context::active_thread() const
    {
        if (!this->ctx->active_thread)
        {
            return nb::none();
        }

        return nb::cast(sogen_linux_thread{*this->ctx->active_thread, *this->emu, this->owner});
    }

    nb::list sogen_linux_process_context::threads() const
    {
        nb::list result{};
        for (auto& [_, thread] : this->ctx->threads)
        {
            if (!thread.terminated)
            {
                result.append(nb::cast(sogen_linux_thread{thread, *this->emu, this->owner}));
            }
        }
        return result;
    }

    // ----- linux_symbol_hook_registry -----

    linux_symbol_hook_registry::linux_symbol_hook_registry(linux_emulator& emulator)
        : emu(&emulator)
    {
        this->module_load_id = this->emu->on_module_load.add([this](linux_mapped_module&) { this->refresh_index(); });
    }

    linux_symbol_hook_registry::~linux_symbol_hook_registry()
    {
        this->clear();
        if (this->emu)
        {
            this->emu->on_module_load.remove(this->module_load_id);
        }
    }

    void linux_symbol_hook_registry::clear()
    {
        for (auto& [_, entry] : this->entries)
        {
            entry.hooks.clear();
        }
        this->entries.clear();
    }

    void linux_symbol_hook_registry::del_item(const std::string& key)
    {
        if (this->entries.erase(key) != 0)
        {
            this->refresh_index();
        }
    }

    void linux_symbol_hook_registry::set_item(const std::string& key, nb::object callback)
    {
        if (callback.is_none())
        {
            this->del_item(key);
            return;
        }

        if (!PyCallable_Check(callback.ptr()))
        {
            throw std::runtime_error("Linux symbol hook callback must be callable or None");
        }

        auto target = parse_linux_symbol_hook_target(key);
        auto [params, restype] = read_linux_symbol_signature(callback);
        auto [entry_it, inserted] = this->entries.try_emplace(key);
        (void)inserted;
        auto& entry = entry_it->second;
        entry.module_filter = std::move(target.module);
        entry.name = std::move(target.name);
        entry.params = std::move(params);
        entry.restype = std::move(restype);
        entry.callback = std::move(callback);
        this->refresh_index();
    }

    void linux_symbol_hook_registry::refresh_after_state_restore()
    {
        this->refresh_index();
    }

    nb::list linux_symbol_hook_registry::resolve_params(const linux_symbol_hook_entry& entry) const
    {
        nb::list params{};
        if (entry.params.is_none())
        {
            return params;
        }

        const auto param_types = nb::cast<nb::sequence>(entry.params);
        const auto count = static_cast<size_t>(nb::len(param_types));
        auto& backend = this->emu->emu();
        const std::array<x86_register, 6> argument_registers{
            x86_register::rdi, x86_register::rsi, x86_register::rdx, x86_register::rcx, x86_register::r8, x86_register::r9,
        };
        const auto rsp = backend.reg<uint64_t>(x86_register::rsp);

        for (size_t i = 0; i < count; ++i)
        {
            uint64_t value{};
            if (i < argument_registers.size())
            {
                value = backend.reg<uint64_t>(argument_registers.at(i));
            }
            else if (!backend.try_read_memory(rsp + sizeof(uint64_t) + ((i - argument_registers.size()) * sizeof(uint64_t)), &value,
                                              sizeof(value)))
            {
                throw std::runtime_error("Failed to read Linux symbol hook stack argument");
            }

            params.append(decode_linux_symbol_param(sequence_item(param_types, i), value));
        }

        return params;
    }

    void linux_symbol_hook_registry::return_from_symbol(const linux_symbol_call_info& call) const
    {
        auto& backend = this->emu->emu();
        backend.reg<uint64_t>(x86_register::rax, call.return_value);
        backend.reg<uint64_t>(x86_register::rsp, call.stack_pointer + sizeof(uint64_t));
        backend.reg<uint64_t>(x86_register::rip, call.return_address);
    }

    void linux_symbol_hook_registry::invoke_hook(const std::string& key, linux_mapped_module& module, const linux_exported_symbol& symbol,
                                                 const uint64_t return_address)
    {
        const auto it = this->entries.find(key);
        if (it == this->entries.end())
        {
            return;
        }

        auto& entry = it->second;
        auto call = std::make_shared<linux_symbol_call_info>();
        call->module = std::make_shared<linux_mapped_module>(module);
        call->name = symbol.name;
        call->address = symbol.address;
        call->return_address = return_address;
        call->stack_pointer = this->emu->emu().reg<uint64_t>(x86_register::rsp);

        nb::gil_scoped_acquire gil{};

        try
        {
            const auto params = this->resolve_params(entry);
            const auto result = coerce_api_continuation(entry.callback(nb::cast(call), params));
            if (result == api_call_continuation::intercept)
            {
                this->return_from_symbol(*call);
            }
        }
        catch (const std::exception& e)
        {
            PyErr_Clear();
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
            std::fprintf(stderr, "[sogen] linux symbol hook '%s' raised: %s\n", symbol.name.c_str(), e.what());
            std::fflush(stderr);
        }
        catch (...)
        {
            PyErr_Clear();
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
            std::fprintf(stderr, "[sogen] linux symbol hook '%s' raised an unknown exception\n", symbol.name.c_str());
            std::fflush(stderr);
        }
    }

    void linux_symbol_hook_registry::refresh_index()
    {
        for (auto& [_, entry] : this->entries)
        {
            entry.hooks.clear();
        }

        for (auto& [_, module] : this->emu->mod_manager.get_modules())
        {
            for (auto& [key, entry] : this->entries)
            {
                this->add_entry_for_module(key, entry, module);
            }
        }
    }

    void linux_symbol_hook_registry::add_entry_for_module(const std::string& key, linux_symbol_hook_entry& entry,
                                                          linux_mapped_module& module)
    {
        if (!linux_symbol_module_matches(entry, module))
        {
            return;
        }

        for (const auto& symbol : module.exports)
        {
            if (symbol.name != entry.name || symbol.address == 0)
            {
                continue;
            }

            auto* hook = this->emu->emu().hook_memory_execution(symbol.address, [this, key, symbol](uint64_t address) {
                auto& backend = this->emu->emu();
                uint64_t return_address{};
                if (!backend.try_read_memory(backend.reg<uint64_t>(x86_register::rsp), &return_address, sizeof(return_address)))
                {
                    return;
                }

                auto* current_module = this->emu->mod_manager.find_by_address(address);
                if (!current_module)
                {
                    return;
                }

                this->invoke_hook(key, *current_module, symbol, return_address);
            });
            entry.hooks.emplace_back(this->emu->emu(), hook, nb::none());
        }
    }

    // ----- linux_debug_facade -----

    linux_debug_facade::linux_debug_facade(linux_emulator& emulator)
        : emu(&emulator)
    {
    }

    linux_debug_facade::~linux_debug_facade()
    {
        for (auto& [_, hook] : this->breakpoints)
        {
            hook.remove();
        }
    }

    void linux_debug_facade::handle_breakpoint(const uint64_t address) const
    {
        this->emu->record_stop(stop_reason::breakpoint, "address=" + hex_address(address));
        this->emu->emu().reg<uint64_t>(x86_register::rip, address);
        this->emu->stop();
    }

    bool linux_debug_facade::set_breakpoint(const uint64_t address)
    {
        if (this->breakpoints.contains(address))
        {
            return true;
        }

        auto* hook = this->emu->emu().hook_memory_execution(address, [this, address](uint64_t) { this->handle_breakpoint(address); });
        this->breakpoints.emplace(address, hook_handle{this->emu->emu(), hook, nb::none()});
        return true;
    }

    bool linux_debug_facade::clear_breakpoint(const uint64_t address)
    {
        const auto it = this->breakpoints.find(address);
        if (it == this->breakpoints.end())
        {
            return false;
        }

        it->second.remove();
        this->breakpoints.erase(it);
        return true;
    }

    nb::list linux_debug_facade::list_breakpoints() const
    {
        nb::list result{};
        for (const auto& [address, hook] : this->breakpoints)
        {
            if (hook.active())
            {
                result.append(address);
            }
        }
        return result;
    }

    bool linux_debug_facade::suppress_current_breakpoint_once()
    {
        const auto address = this->emu->emu().reg<uint64_t>(x86_register::rip);
        auto it = this->breakpoints.find(address);
        if (it == this->breakpoints.end() || !it->second.active())
        {
            return false;
        }

        it->second.remove();
        try
        {
            this->emu->start(1);
        }
        catch (...)
        {
            auto* hook = this->emu->emu().hook_memory_execution(address, [this, address](uint64_t) { this->handle_breakpoint(address); });
            it->second = hook_handle{this->emu->emu(), hook, nb::none()};
            throw;
        }

        auto* hook = this->emu->emu().hook_memory_execution(address, [this, address](uint64_t) { this->handle_breakpoint(address); });
        it->second = hook_handle{this->emu->emu(), hook, nb::none()};
        return true;
    }

    void linux_debug_facade::step_into()
    {
        if (!this->suppress_current_breakpoint_once())
        {
            this->emu->start(1);
        }
    }

    void linux_debug_facade::step_over()
    {
        this->step_into();
    }

    void linux_debug_facade::step_out()
    {
        auto& backend = this->emu->emu();
        const auto frame_pointer = backend.reg<uint64_t>(x86_register::rbp);
        if (frame_pointer == 0)
        {
            throw std::runtime_error("step_out cannot find a caller because RBP is zero; use run_to(address) when the target "
                                     "return address is known");
        }

        const auto return_address_location = frame_pointer + sizeof(uint64_t);
        uint64_t return_address{};
        if (!this->emu->memory.try_read_memory(return_address_location, &return_address, sizeof(return_address)))
        {
            std::ostringstream message{};
            message << "step_out cannot read the saved return address at 0x" << std::hex << return_address_location
                    << "; use run_to(address) if frame pointers are unavailable";
            throw std::runtime_error(message.str());
        }

        if (return_address == 0)
        {
            std::ostringstream message{};
            message << "step_out found a zero saved return address at 0x" << std::hex << return_address_location
                    << "; use run_to(address) for an explicit destination";
            throw std::runtime_error(message.str());
        }

        this->run_to(return_address);
    }

    void linux_debug_facade::run_to(const uint64_t address)
    {
        auto* hook = this->emu->emu().hook_memory_execution(address, [this, address](uint64_t) { this->handle_breakpoint(address); });
        hook_handle temporary{this->emu->emu(), hook, nb::none()};
        try
        {
            this->emu->start();
        }
        catch (...)
        {
            temporary.remove();
            throw;
        }
        temporary.remove();
    }

    void linux_debug_facade::continue_execution()
    {
        if (this->suppress_current_breakpoint_once() && this->emu->last_stop_reason() != stop_reason::instruction_limit)
        {
            return;
        }

        this->emu->start();
    }

    void linux_debug_facade::pause() const
    {
        this->emu->stop();
    }

    nb::dict linux_debug_facade::registers() const
    {
        auto& backend = this->emu->emu();
        nb::dict result{};
        static constexpr auto gpr = std::to_array<std::pair<const char*, x86_register>>({
            {"rax", x86_register::rax},
            {"rbx", x86_register::rbx},
            {"rcx", x86_register::rcx},
            {"rdx", x86_register::rdx},
            {"rsi", x86_register::rsi},
            {"rdi", x86_register::rdi},
            {"rbp", x86_register::rbp},
            {"rsp", x86_register::rsp},
            {"r8", x86_register::r8},
            {"r9", x86_register::r9},
            {"r10", x86_register::r10},
            {"r11", x86_register::r11},
            {"r12", x86_register::r12},
            {"r13", x86_register::r13},
            {"r14", x86_register::r14},
            {"r15", x86_register::r15},
            {"rip", x86_register::rip},
            {"rflags", x86_register::eflags},
        });
        static constexpr auto seg = std::to_array<std::pair<const char*, x86_register>>({
            {"cs", x86_register::cs},
            {"ss", x86_register::ss},
            {"ds", x86_register::ds},
            {"es", x86_register::es},
            {"fs", x86_register::fs},
            {"gs", x86_register::gs},
        });

        for (const auto& [name, reg] : gpr)
        {
            set_dict_item(result, name, backend.reg<uint64_t>(reg));
        }
        for (const auto& [name, reg] : seg)
        {
            set_dict_item(result, name, backend.reg<uint16_t>(reg));
        }
        return result;
    }

    nb::list linux_debug_facade::modules() const
    {
        nb::list result{};
        for (const auto& [_, module] : this->emu->mod_manager.get_modules())
        {
            result.append(linux_module_dict(module));
        }
        return result;
    }

    nb::list linux_debug_facade::threads() const
    {
        nb::list result{};
        const auto* active = this->emu->process.active_thread;
        for (const auto& [_, thread] : this->emu->process.threads)
        {
            if (thread.terminated)
            {
                continue;
            }

            nb::dict item{};
            set_dict_item(item, "tid", thread.tid);
            set_dict_item(item, "current_ip",
                          &thread == active ? this->emu->emu().reg<uint64_t>(x86_register::rip) : thread.saved_regs.rip);
            set_dict_item(item, "active", &thread == active);
            set_dict_item(item, "terminated", thread.terminated);
            result.append(std::move(item));
        }
        return result;
    }

    nb::list linux_debug_facade::disassemble(const uint64_t address, const size_t count_or_size) const
    {
        nb::list result{};
        if (count_or_size == 0)
        {
            return result;
        }

        auto& backend = this->emu->emu();
        static constexpr size_t max_instruction_count = 4096;
        const auto count = std::min(count_or_size, max_instruction_count);
        std::vector<uint8_t> bytes(count * 16);
        if (!this->emu->memory.try_read_memory(address, bytes.data(), bytes.size()))
        {
            bytes.resize(std::min<size_t>(count_or_size, 4096));
            if (bytes.empty() || !this->emu->memory.try_read_memory(address, bytes.data(), bytes.size()))
            {
                return result;
            }
        }

        disassembler dis{};
        const auto cs_selector = backend.reg<uint16_t>(x86_register::cs);
        const auto instructions =
            dis.disassemble(backend, cs_selector, std::span<const uint8_t>(bytes.data(), bytes.size()), count, address);
        for (const auto& insn : instructions)
        {
            nb::dict item{};
            set_dict_item(item, "address", insn.address);
            set_dict_item(item, "size", insn.size);
            set_dict_item(item, "bytes", nb::bytes(reinterpret_cast<const char*>(insn.bytes), static_cast<nb::ssize_t>(insn.size)));
            set_dict_item(item, "mnemonic", std::string(insn.mnemonic));
            set_dict_item(item, "operands", std::string(insn.op_str));
            result.append(std::move(item));
        }
        return result;
    }

    nb::list linux_debug_facade::call_stack() const
    {
        auto& backend = this->emu->emu();
        nb::list frames{};

        const auto rip = backend.reg<uint64_t>(x86_register::rip);
        const auto rsp = backend.reg<uint64_t>(x86_register::rsp);
        nb::dict current{};
        set_dict_item(current, "instruction_pointer", rip);
        set_dict_item(current, "stack_pointer", rsp);
        set_dict_item(current, "module", linux_module_name_for(*this->emu, rip));
        frames.append(std::move(current));

        auto frame_pointer = backend.reg<uint64_t>(x86_register::rbp);
        for (size_t depth = 0; depth < 64 && frame_pointer != 0; ++depth)
        {
            uint64_t saved_rbp{};
            uint64_t return_address{};
            if (!this->emu->memory.try_read_memory(frame_pointer, &saved_rbp, sizeof(saved_rbp)) ||
                !this->emu->memory.try_read_memory(frame_pointer + sizeof(uint64_t), &return_address, sizeof(return_address)))
            {
                break;
            }
            if (return_address == 0 || saved_rbp <= frame_pointer)
            {
                break;
            }

            nb::dict frame{};
            set_dict_item(frame, "instruction_pointer", return_address);
            set_dict_item(frame, "stack_pointer", frame_pointer);
            set_dict_item(frame, "module", linux_module_name_for(*this->emu, return_address));
            frames.append(std::move(frame));
            frame_pointer = saved_rbp;
        }

        return frames;
    }

    // ----- linux_hook_registry -----

    linux_hook_registry::linux_hook_registry(linux_emulator& emulator)
        : emu(&emulator),
          symbols(std::make_shared<linux_symbol_hook_registry>(emulator))
    {
    }

    hook_handle linux_hook_registry::make_hook(emulator_hook* hook)
    {
        hook_handle stored{this->emu->emu(), hook, nb::none()};
        this->active_hooks.emplace_back(stored);

        auto exposed = stored;
        exposed.owner = nb::cast(this, nb::rv_policy::reference_internal);
        return exposed;
    }

    hook_handle linux_hook_registry::memory_execution(nb::object callback)
    {
        auto* hook = this->emu->emu().hook_memory_execution([cb = std::move(callback)](uint64_t address) {
            nb::gil_scoped_acquire gil{};
            cb(address);
        });
        return make_hook(hook);
    }

    hook_handle linux_hook_registry::memory_execution_at(uint64_t address, nb::object callback)
    {
        auto* hook = this->emu->emu().hook_memory_execution(address, [cb = std::move(callback)](uint64_t addr) {
            nb::gil_scoped_acquire gil{};
            cb(addr);
        });
        return make_hook(hook);
    }

    hook_handle linux_hook_registry::memory_read(uint64_t address, uint64_t size, nb::object callback)
    {
        auto* hook =
            this->emu->emu().hook_memory_read(address, size, [cb = std::move(callback)](uint64_t addr, const void* data, size_t length) {
                nb::gil_scoped_acquire gil{};
                cb(addr, nb::bytes(static_cast<const char*>(data), static_cast<nb::ssize_t>(length)));
            });
        return make_hook(hook);
    }

    hook_handle linux_hook_registry::memory_write(uint64_t address, uint64_t size, nb::object callback)
    {
        auto* hook =
            this->emu->emu().hook_memory_write(address, size, [cb = std::move(callback)](uint64_t addr, const void* data, size_t length) {
                nb::gil_scoped_acquire gil{};
                cb(addr, nb::bytes(static_cast<const char*>(data), static_cast<nb::ssize_t>(length)));
            });
        return make_hook(hook);
    }

    hook_handle linux_hook_registry::instruction(x86_hookable_instructions instruction_type, nb::object callback)
    {
        auto* hook = this->emu->emu().hook_instruction(instruction_type, [cb = std::move(callback)](uint64_t data) {
            nb::gil_scoped_acquire gil{};
            return coerce_instruction_continuation(cb(data));
        });
        return make_hook(hook);
    }

    hook_handle linux_hook_registry::interrupt(nb::object callback)
    {
        auto* emulator = this->emu;
        auto callback_holder = std::shared_ptr<nb::object>{
            new nb::object(std::move(callback)),
            [](nb::object* cb) {
                nb::gil_scoped_acquire gil{};
                delete cb;
            },
        };
        const auto observer_id = emulator->add_interrupt_observer([cb = std::move(callback_holder)](int interrupt) {
            nb::gil_scoped_acquire gil{};
            (*cb)(interrupt);
        });

        hook_handle stored{[emulator, observer_id] { emulator->remove_interrupt_observer(observer_id); }, nb::none()};
        this->active_hooks.emplace_back(stored);

        auto exposed = stored;
        exposed.owner = nb::cast(this, nb::rv_policy::reference_internal);
        return exposed;
    }

    hook_handle linux_hook_registry::basic_block(nb::object callback)
    {
        auto* hook = this->emu->emu().hook_basic_block([cb = std::move(callback)](const sogen::basic_block& block) {
            nb::gil_scoped_acquire gil{};
            cb(block);
        });
        return make_hook(hook);
    }

    hook_handle linux_hook_registry::memory_violation(nb::object callback)
    {
        auto* emulator = this->emu;
        const auto observer_id = emulator->add_memory_violation_observer(
            [cb = std::move(callback)](uint64_t address, size_t size, memory_operation operation, memory_violation_type type) {
                nb::gil_scoped_acquire gil{};
                const auto result = cb(address, size, operation, type);
                return coerce_memory_violation_continuation(result);
            });

        hook_handle stored{[emulator, observer_id] { emulator->remove_memory_violation_observer(observer_id); }, nb::none()};
        this->active_hooks.emplace_back(stored);

        auto exposed = stored;
        exposed.owner = nb::cast(this, nb::rv_policy::reference_internal);
        return exposed;
    }

    sogen_linux_emulator::sogen_linux_emulator(std::unique_ptr<linux_emulator> emulator)
        : emu(std::move(emulator)),
          callbacks(std::make_shared<linux_callback_registry>(*this->emu)),
          hooks(std::make_shared<linux_hook_registry>(*this->emu)),
          debug(std::make_shared<linux_debug_facade>(*this->emu))
    {
    }

    sogen_linux_emulator::~sogen_linux_emulator() = default;

    sogen_linux_emulator::sogen_linux_emulator(sogen_linux_emulator&&) noexcept = default;

    sogen_linux_emulator& sogen_linux_emulator::operator=(sogen_linux_emulator&&) noexcept = default;

    linux_emulator& sogen_linux_emulator::native() const
    {
        return *this->emu;
    }

    void sogen_linux_emulator::start(const size_t count) const
    {
        this->emu->start(count);
    }

    void sogen_linux_emulator::stop() const
    {
        this->emu->stop();
    }

    nb::bytes sogen_linux_emulator::serialize_state() const
    {
        utils::buffer_serializer serializer{};
        this->emu->serialize(serializer, false);
        const auto data = serializer.move_buffer();
        return nb::bytes(reinterpret_cast<const char*>(data.data()), static_cast<nb::ssize_t>(data.size()));
    }

    void sogen_linux_emulator::deserialize_state(const nb::bytes& buffer) const
    {
        const auto* begin = reinterpret_cast<const std::byte*>(buffer.data());
        const auto* end = begin + buffer.size();
        utils::buffer_deserializer deserializer{std::span(begin, end)};
        this->emu->deserialize(deserializer, false);
        this->hooks->symbols->refresh_after_state_restore();
    }

    void sogen_linux_emulator::save_snapshot()
    {
        utils::buffer_serializer serializer{};
        this->emu->serialize(serializer, false);
        this->snapshot_ = serializer.move_buffer();
    }

    void sogen_linux_emulator::restore_snapshot() const
    {
        if (this->snapshot_.empty())
        {
            throw std::runtime_error("Linux emulator snapshot has not been saved");
        }

        utils::buffer_deserializer deserializer{this->snapshot_};
        this->emu->deserialize(deserializer, false);
        this->hooks->symbols->refresh_after_state_restore();
    }

    sogen_linux_process_context sogen_linux_emulator::process()
    {
        return {this->emu->process, *this->emu, nb::cast(this, nb::rv_policy::reference_internal)};
    }

    linux_memory_manager& sogen_linux_emulator::memory() const
    {
        return this->emu->memory;
    }

    nb::bytes sogen_linux_emulator::read_memory(const uint64_t address, const size_t size) const
    {
        return read_memory_bytes(this->emu->memory, address, size);
    }

    void sogen_linux_emulator::write_memory(const uint64_t address, const nb::bytes& buffer) const
    {
        write_memory_bytes(this->emu->memory, address, buffer);
    }

    uint64_t sogen_linux_emulator::read_register(const x86_register reg) const
    {
        return this->emu->emu().reg<uint64_t>(reg);
    }

    void sogen_linux_emulator::write_register(const x86_register reg, const uint64_t value) const
    {
        this->emu->emu().reg<uint64_t>(reg, value);
    }

    uint64_t sogen_linux_emulator::executed_instructions() const
    {
        return this->emu->get_executed_instructions();
    }

    std::string sogen_linux_emulator::backend_name() const
    {
        return this->emu->emu().get_name();
    }

    std::filesystem::path sogen_linux_emulator::emulation_root() const
    {
        return this->emu->emulation_root;
    }

    std::string sogen_linux_emulator::last_stop_reason() const
    {
        return stop_reason_to_string(this->emu->last_stop_reason());
    }

    int sogen_linux_emulator::last_stop_reason_code() const
    {
        return static_cast<int>(this->emu->last_stop_reason());
    }

    const std::string& sogen_linux_emulator::last_stop_detail() const
    {
        return this->emu->last_stop_detail();
    }

    nb::list sogen_linux_emulator::modules() const
    {
        nb::list result{};
        for (auto& [_, module] : this->emu->mod_manager.get_modules())
        {
            result.append(nb::cast(module));
        }
        return result;
    }

    nb::object sogen_linux_emulator::current_thread() const
    {
        auto* thread = this->emu->current_thread();
        if (!thread)
        {
            return nb::none();
        }

        return nb::cast(sogen_linux_thread{*thread, *this->emu, nb::cast(this, nb::rv_policy::reference_internal)});
    }

    std::optional<uint32_t> sogen_linux_emulator::current_thread_id() const
    {
        return this->emu->current_thread_id();
    }

    bool sogen_linux_emulator::activate_thread(const uint32_t tid) const
    {
        return this->emu->activate_thread(tid);
    }

    bool sogen_linux_emulator::perform_thread_switch() const
    {
        return this->emu->perform_thread_switch();
    }

    void sogen_linux_emulator::yield_thread() const
    {
        this->emu->yield_thread();
    }

    linux_mapped_module* sogen_linux_emulator::find_module_by_address(const uint64_t address) const
    {
        return this->emu->mod_manager.find_by_address(address);
    }

    linux_mapped_module* sogen_linux_emulator::find_module_by_name(const std::string_view name) const
    {
        return this->emu->mod_manager.find_by_name(name);
    }
}
