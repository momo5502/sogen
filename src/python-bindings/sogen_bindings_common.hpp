#pragma once

#include <nanobind/nanobind.h>
#include <nanobind/stl/filesystem.h>
#include <nanobind/stl/map.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/set.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/string_view.h>
#include <nanobind/stl/unordered_map.h>
#include <nanobind/stl/vector.h>

#include <backend_selection.hpp>
#include <windows_emulator.hpp>
#include <x86_register.hpp>

namespace nb = nanobind;

namespace
{
    [[maybe_unused]] std::string stop_reason_to_string(const stop_reason reason)
    {
        switch (reason)
        {
        case stop_reason::none:
            return "none";
        case stop_reason::unknown_syscall:
            return "unknown_syscall";
        case stop_reason::unimplemented_syscall:
            return "unimplemented_syscall";
        case stop_reason::syscall_exception:
            return "syscall_exception";
        }

        return "unknown";
    }

    template <typename T>
    T get_kwarg(const nb::kwargs& kwargs, const char* name, T default_value)
    {
        if (!kwargs.contains(name))
        {
            return default_value;
        }

        return nb::cast<T>(kwargs[name]);
    }

    std::vector<std::u16string> parse_arguments(const nb::object& object)
    {
        std::vector<std::u16string> result{};
        if (object.is_none())
        {
            return result;
        }

        const auto seq = nb::cast<nb::sequence>(object);
        result.reserve(static_cast<size_t>(nb::len(seq)));
        for (const auto& item : seq)
        {
            result.emplace_back(u8_to_u16(nb::cast<std::string>(item)));
        }

        return result;
    }

    utils::unordered_insensitive_u16string_map<std::u16string> parse_environment(const nb::object& object)
    {
        utils::unordered_insensitive_u16string_map<std::u16string> result{};
        if (object.is_none())
        {
            return result;
        }

        const auto dict = nb::cast<nb::dict>(object);
        for (const auto& item : dict)
        {
            result.emplace(u8_to_u16(nb::cast<std::string>(item.first)), u8_to_u16(nb::cast<std::string>(item.second)));
        }

        return result;
    }

    std::unordered_map<windows_path, std::filesystem::path> parse_path_mappings(const nb::object& object)
    {
        std::unordered_map<windows_path, std::filesystem::path> result{};
        if (object.is_none())
        {
            return result;
        }

        const auto dict = nb::cast<nb::dict>(object);
        for (const auto& item : dict)
        {
            result.emplace(nb::cast<std::filesystem::path>(item.first), nb::cast<std::filesystem::path>(item.second));
        }

        return result;
    }

    std::unordered_map<uint16_t, uint16_t> parse_port_mappings(const nb::object& object)
    {
        std::unordered_map<uint16_t, uint16_t> result{};
        if (object.is_none())
        {
            return result;
        }

        const auto dict = nb::cast<nb::dict>(object);
        for (const auto& item : dict)
        {
            result.emplace(nb::cast<uint16_t>(item.first), nb::cast<uint16_t>(item.second));
        }

        return result;
    }

    emulator_settings make_emulator_settings(const nb::kwargs& kwargs)
    {
        emulator_settings settings{};
        settings.disable_logging = get_kwarg<bool>(kwargs, "disable_logging", false);
        settings.use_relative_time = get_kwarg<bool>(kwargs, "use_relative_time", false);
        settings.emulation_root = get_kwarg<std::filesystem::path>(kwargs, "emulation_root", {});
        settings.registry_directory = get_kwarg<std::filesystem::path>(kwargs, "registry_directory", std::filesystem::path{"./registry"});
        settings.path_mappings = parse_path_mappings(kwargs.contains("path_mappings") ? kwargs["path_mappings"] : nb::none());
        settings.port_mappings = parse_port_mappings(kwargs.contains("port_mappings") ? kwargs["port_mappings"] : nb::none());
        settings.fake_env.number_of_processors = get_kwarg<uint32_t>(kwargs, "number_of_processors", 4);
        settings.fake_env.nt_product_type = static_cast<uint8_t>(get_kwarg<uint32_t>(kwargs, "nt_product_type", 1));
        return settings;
    }

    application_settings make_application_settings(const nb::object& application, const nb::object& args, const nb::kwargs& kwargs)
    {
        application_settings settings{};
        settings.application = nb::cast<std::filesystem::path>(application);
        settings.working_directory = get_kwarg<std::filesystem::path>(kwargs, "working_directory", {});
        settings.arguments = parse_arguments(args);
        settings.environment = parse_environment(kwargs.contains("environment") ? kwargs["environment"] : nb::none());
        return settings;
    }

    backend_type get_backend_type(const nb::kwargs& kwargs)
    {
        return get_kwarg<backend_type>(kwargs, "backend", backend_type::unicorn);
    }

    [[maybe_unused]] std::unique_ptr<windows_emulator> create_empty_emulator(const nb::kwargs& kwargs)
    {
        return std::make_unique<windows_emulator>(create_x86_64_emulator(get_backend_type(kwargs)), make_emulator_settings(kwargs));
    }

    [[maybe_unused]] std::unique_ptr<windows_emulator> create_application_emulator(const nb::object& application, const nb::object& args,
                                                                                   const nb::kwargs& kwargs)
    {
        auto app_settings = make_application_settings(application, args, kwargs);
        return std::make_unique<windows_emulator>(create_x86_64_emulator(get_backend_type(kwargs)), std::move(app_settings),
                                                  make_emulator_settings(kwargs));
    }

    nb::bytes read_memory_bytes(const memory_interface& memory, const uint64_t address, const size_t size)
    {
        const auto data = memory.read_memory(address, size);
        return nb::bytes(reinterpret_cast<const char*>(data.data()), static_cast<nb::ssize_t>(data.size()));
    }

    void write_memory_bytes(memory_interface& memory, const uint64_t address, const nb::bytes& buffer)
    {
        memory.write_memory(address, buffer.data(), buffer.size());
    }

    nb::bytes serialize_state_bytes(const windows_emulator& emulator)
    {
        utils::buffer_serializer serializer{};
        emulator.serialize(serializer);

        const auto data = serializer.move_buffer();
        return nb::bytes(reinterpret_cast<const char*>(data.data()), static_cast<nb::ssize_t>(data.size()));
    }

    void deserialize_state_bytes(windows_emulator& emulator, const nb::bytes& buffer)
    {
        const auto* begin = reinterpret_cast<const std::byte*>(buffer.data());
        const auto* end = begin + buffer.size();
        utils::buffer_deserializer deserializer{std::span(begin, end)};
        emulator.deserialize(deserializer);
    }

    template <typename... Args>
    void invoke_callback(const nb::object& cb, Args&&... args)
    {
        if (cb.is_none())
        {
            return;
        }

        nb::gil_scoped_acquire gil{};
        cb(std::forward<Args>(args)...);
    }

    template <typename... Args>
    bool invoke_bool_callback(const nb::object& cb, Args&&... args)
    {
        if (cb.is_none())
        {
            return false;
        }

        nb::gil_scoped_acquire gil{};
        return nb::cast<bool>(cb(std::forward<Args>(args)...));
    }

    instruction_hook_continuation coerce_instruction_continuation(nb::handle result)
    {
        if (result.is_none())
        {
            return instruction_hook_continuation::run_instruction;
        }

        if (nb::isinstance<nb::bool_>(result))
        {
            return nb::cast<bool>(result) ? instruction_hook_continuation::skip_instruction
                                          : instruction_hook_continuation::run_instruction;
        }

        return nb::cast<instruction_hook_continuation>(result);
    }

    memory_violation_continuation coerce_memory_violation_continuation(nb::handle result)
    {
        if (result.is_none())
        {
            return memory_violation_continuation::resume;
        }

        if (nb::isinstance<nb::bool_>(result))
        {
            return nb::cast<bool>(result) ? memory_violation_continuation::resume : memory_violation_continuation::stop;
        }

        return nb::cast<memory_violation_continuation>(result);
    }

    enum class api_call_continuation
    {
        run_original,
        intercept,
    };

    struct api_call_info
    {
        std::string module{};
        std::string name{};
        uint64_t address{};
        uint64_t return_address{};
        uint64_t stack_pointer{};
        uint64_t return_value{0};
    };

    struct api_hook_target
    {
        std::optional<std::string> module{};
        std::string name{};
    };

    struct api_hook_signature
    {
        function_calling_convention cc{function_calling_convention::x64_fastcall};
        nb::object params = nb::none();
    };

    [[maybe_unused]] std::string to_lower_ascii(std::string value)
    {
        std::ranges::transform(value, value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    }

    [[maybe_unused]] api_hook_target parse_api_hook_target(const std::string& key)
    {
        const auto pos = key.find('!');
        if (pos == std::string::npos)
        {
            return {.module = std::nullopt, .name = key};
        }

        return {.module = key.substr(0, pos), .name = key.substr(pos + 1)};
    }

    struct hook_handle
    {
        windows_emulator* emu{};
        emulator_hook* hook{};
        nb::object owner = nb::none();

        hook_handle() = default;

        hook_handle(windows_emulator& emulator, emulator_hook* hook, nb::object owner)
            : emu(&emulator),
              hook(hook),
              owner(std::move(owner))
        {
        }

        ~hook_handle()
        {
            remove();
        }

        hook_handle(const hook_handle&) = delete;
        hook_handle& operator=(const hook_handle&) = delete;

        hook_handle(hook_handle&& other) noexcept
            : emu(other.emu),
              hook(other.hook),
              owner(std::move(other.owner))
        {
            other.emu = nullptr;
            other.hook = nullptr;
        }

        hook_handle& operator=(hook_handle&& other) noexcept
        {
            if (this != &other)
            {
                remove();
                emu = other.emu;
                hook = other.hook;
                owner = std::move(other.owner);
                other.emu = nullptr;
                other.hook = nullptr;
            }

            return *this;
        }

        bool active() const
        {
            return this->hook != nullptr;
        }

        void remove()
        {
            if (!this->emu || !this->hook)
            {
                return;
            }

            try
            {
                this->emu->emu().delete_hook(this->hook);
            }
            catch (...)
            {
            }

            this->hook = nullptr;
        }
    };

    struct api_hook_hit
    {
        std::string key{};
        std::string module_name{};
        std::string export_name{};
        uint64_t address{};
        uint64_t return_address{};
        bool entered_from_call{false};
    };

    struct api_hook_entry
    {
        std::optional<std::string> module_filter{};
        std::string name{};
        function_calling_convention cc{function_calling_convention::x64_fastcall};
        nb::object params = nb::none();
        nb::object callback = nb::none();
        std::vector<std::pair<uint64_t, hook_handle>> hooks{};

        api_hook_entry() = default;
        api_hook_entry(const api_hook_entry&) = delete;
        api_hook_entry& operator=(const api_hook_entry&) = delete;
        api_hook_entry(api_hook_entry&&) noexcept = default;
        api_hook_entry& operator=(api_hook_entry&&) noexcept = default;
    };

    [[maybe_unused]] api_call_continuation coerce_api_continuation(nb::handle result)
    {
        if (result.is_none())
        {
            return api_call_continuation::run_original;
        }

        if (nb::isinstance<nb::bool_>(result))
        {
            return nb::cast<bool>(result) ? api_call_continuation::intercept : api_call_continuation::run_original;
        }

        return nb::cast<api_call_continuation>(result);
    }

    struct api_hook_registry
    {
        windows_emulator* win_emu{};
        std::map<std::string, api_hook_entry, std::less<>> entries{};
        std::map<uint64_t, std::vector<api_hook_hit>> address_index{};
        std::optional<hook_handle> execution_hook{};
        utils::callback_id_type module_load_id{};
        utils::callback_id_type module_unload_id{};

        api_hook_registry() = delete;
        api_hook_registry(const api_hook_registry&) = delete;
        api_hook_registry& operator=(const api_hook_registry&) = delete;
        api_hook_registry(api_hook_registry&&) = delete;
        api_hook_registry& operator=(api_hook_registry&&) = delete;

        explicit api_hook_registry(windows_emulator& emulator)
            : win_emu(&emulator)
        {
            this->module_load_id = this->win_emu->callbacks.on_module_load.add([this](mapped_module&) { this->refresh_index(); });
            this->module_unload_id = this->win_emu->callbacks.on_module_unload.add([this](mapped_module&) { this->refresh_index(); });
        }

        ~api_hook_registry()
        {
            this->clear();
            this->win_emu->callbacks.on_module_load.remove(this->module_load_id);
            this->win_emu->callbacks.on_module_unload.remove(this->module_unload_id);
        }

        void clear()
        {
            for (auto& [_, entry] : this->entries)
            {
                entry.hooks.clear();
            }
            this->entries.clear();
            this->address_index.clear();
            this->remove_execution_hook();
        }

        void del_item(const std::string& key)
        {
            if (this->entries.erase(key) != 0)
            {
                this->refresh_index();
            }
        }

        void set_item(const std::string& key, nb::object callback)
        {
            if (callback.is_none())
            {
                this->del_item(key);
                return;
            }

            auto target = parse_api_hook_target(key);
            auto& entry = this->entries[key];

            entry.module_filter = std::move(target.module);
            entry.name = std::move(target.name);
            entry.callback = std::move(callback);
            api_hook_registry::read_signature(entry);
            this->refresh_index();
        }

      private:
        static api_hook_signature read_signature(const nb::object& callback)
        {
            if (!nb::hasattr(callback, "_sogen_api_cc") || !nb::hasattr(callback, "_sogen_api_params"))
            {
                throw std::runtime_error("API hook callback must be decorated with sogen.api_call()");
            }

            api_hook_signature signature{};
            signature.cc = nb::cast<function_calling_convention>(nb::getattr(callback, "_sogen_api_cc"));
            signature.params = nb::getattr(callback, "_sogen_api_params");
            return signature;
        }

        static void read_signature(api_hook_entry& entry)
        {
            auto signature = read_signature(entry.callback);
            entry.cc = signature.cc;
            entry.params = std::move(signature.params);
        }

        static bool matches_module(const api_hook_entry& entry, const mapped_module& module)
        {
            if (!entry.module_filter.has_value())
            {
                return true;
            }

            const auto expected = to_lower_ascii(*entry.module_filter);
            const auto name = to_lower_ascii(module.name);
            auto stem = name;
            if (const auto dot = stem.rfind('.'); dot != std::string::npos)
            {
                stem.erase(dot);
            }

            return expected == name || expected == stem;
        }

        nb::list resolve_params(const api_hook_entry& entry) const
        {
            nb::list params{};
            const auto count = entry.params.is_none() ? size_t{} : static_cast<size_t>(nb::len(entry.params));
            const auto cc = is_32bit_code_segment(this->win_emu->emu()) ? entry.cc : function_calling_convention::x64_fastcall;
            for (const auto value : get_function_arguments(this->win_emu->emu(), cc, count))
            {
                params.append(value);
            }
            return params;
        }

        void return_from_api(const api_call_info& call, const bool entered_from_call) const
        {
            auto& backend = this->win_emu->emu();
            backend.reg<uint64_t>(x86_register::rax, call.return_value);
            backend.reg<uint64_t>(x86_register::rsp, call.stack_pointer + (entered_from_call ? sizeof(uint64_t) : 0));
            backend.reg<uint64_t>(x86_register::rip, call.return_address);
        }

        void invoke_hook(const api_hook_hit& hit)
        {
            auto it = this->entries.find(hit.key);
            if (it == this->entries.end())
            {
                return;
            }

            auto& entry = it->second;
            auto call = std::make_shared<api_call_info>();
            call->module = hit.module_name;
            call->name = hit.export_name;
            call->address = hit.address;
            call->return_address = hit.return_address;
            call->stack_pointer = this->win_emu->emu().reg<uint64_t>(x86_register::rsp);
            nb::gil_scoped_acquire gil{};

            const auto params = this->resolve_params(entry);
            nb::dict call_view;
            call_view["module"] = call->module;
            call_view["name"] = call->name;
            call_view["address"] = call->address;
            call_view["return_address"] = call->return_address;
            call_view["return_value"] = call->return_value;
            try
            {
                const auto result = coerce_api_continuation(entry.callback(call_view, params));
                if (result == api_call_continuation::intercept)
                {
                    this->return_from_api(*call, hit.entered_from_call);
                }
            }
            catch (...)
            {
            }
        }

        static bool decode_call_target(x86_64_emulator& backend, const mapped_module* caller_module, const uint64_t address,
                                       uint64_t& target, uint64_t& return_address)
        {
            std::array<uint8_t, 16> bytes{};
            if (!backend.try_read_memory(address, bytes.data(), bytes.size()))
            {
                return false;
            }

            const auto prefix = instruction_prefix_length(bytes);
            const auto opcode = bytes[prefix];
            if (opcode == 0xE8)
            {
                int32_t rel{};
                std::memcpy(&rel, bytes.data() + 1, sizeof(rel));
                target = address + 5 + rel;
                return_address = address + 5;
                return true;
            }

            if (opcode == 0xFF)
            {
                const auto modrm = bytes[1];
                const auto reg = (modrm >> 3) & 0x7;
                const auto mod = (modrm >> 6) & 0x3;
                const auto rm = modrm & 0x7;
                if (reg != 2)
                {
                    return false;
                }

                if (mod == 0 && rm == 5)
                {
                    int32_t disp{};
                    std::memcpy(&disp, bytes.data() + 2, sizeof(disp));
                    const auto ptr_addr = address + 6 + disp;
                    if (!backend.try_read_memory(ptr_addr, &target, sizeof(target)))
                    {
                        return false;
                    }
                    if (caller_module && target < caller_module->size_of_image)
                    {
                        const auto relocated_target = caller_module->image_base + target;
                        std::array<uint8_t, 1> probe{};
                        if (backend.try_read_memory(relocated_target, probe.data(), probe.size()))
                        {
                            target = relocated_target;
                        }
                    }
                    return_address = address + 6;
                    return true;
                }

                if (mod == 3)
                {
                    switch (rm)
                    {
                    case 0:
                        target = backend.reg<uint64_t>(x86_register::rax);
                        break;
                    case 1:
                        target = backend.reg<uint64_t>(x86_register::rcx);
                        break;
                    case 2:
                        target = backend.reg<uint64_t>(x86_register::rdx);
                        break;
                    case 3:
                        target = backend.reg<uint64_t>(x86_register::rbx);
                        break;
                    case 4:
                        target = backend.reg<uint64_t>(x86_register::rsp);
                        break;
                    case 5:
                        target = backend.reg<uint64_t>(x86_register::rbp);
                        break;
                    case 6:
                        target = backend.reg<uint64_t>(x86_register::rsi);
                        break;
                    case 7:
                        target = backend.reg<uint64_t>(x86_register::rdi);
                        break;
                    default:
                        return false;
                    }
                    return_address = address + 2;
                    return true;
                }
            }

            return false;
        }

        static size_t instruction_prefix_length(const std::array<uint8_t, 16>& bytes)
        {
            size_t prefix = 0;
            while (prefix < bytes.size())
            {
                const auto b = bytes[prefix];
                if (b == 0xF0 || b == 0xF2 || b == 0xF3 || b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26 || b == 0x64 || b == 0x65 ||
                    b == 0x66 || b == 0x67 || (b >= 0x40 && b <= 0x4F))
                {
                    ++prefix;
                    continue;
                }
                break;
            }
            return prefix;
        }

        static bool resolve_jump_target(x86_64_emulator& backend, uint64_t& target)
        {
            for (size_t depth = 0; depth < 8; ++depth)
            {
                std::array<uint8_t, 16> bytes{};
                if (!backend.try_read_memory(target, bytes.data(), bytes.size()))
                {
                    return false;
                }

                const auto prefix = instruction_prefix_length(bytes);
                const auto opcode = bytes[prefix];
                if (opcode == 0xE9)
                {
                    int32_t rel{};
                    std::memcpy(&rel, bytes.data() + prefix + 1, sizeof(rel));
                    target += prefix + 5 + rel;
                    continue;
                }

                if (opcode == 0xFF)
                {
                    const auto modrm = bytes[prefix + 1];
                    const auto reg = (modrm >> 3) & 0x7;
                    const auto mod = (modrm >> 6) & 0x3;
                    const auto rm = modrm & 0x7;
                    if (reg != 4)
                    {
                        return true;
                    }

                    if (mod == 0 && rm == 5)
                    {
                        int32_t disp{};
                        std::memcpy(&disp, bytes.data() + prefix + 2, sizeof(disp));
                        const auto ptr_addr = target + prefix + 6 + disp;
                        if (!backend.try_read_memory(ptr_addr, &target, sizeof(target)))
                        {
                            return false;
                        }
                        continue;
                    }

                    if (mod == 3)
                    {
                        switch (rm)
                        {
                        case 0:
                            target = backend.reg<uint64_t>(x86_register::rax);
                            break;
                        case 1:
                            target = backend.reg<uint64_t>(x86_register::rcx);
                            break;
                        case 2:
                            target = backend.reg<uint64_t>(x86_register::rdx);
                            break;
                        case 3:
                            target = backend.reg<uint64_t>(x86_register::rbx);
                            break;
                        case 4:
                            target = backend.reg<uint64_t>(x86_register::rsp);
                            break;
                        case 5:
                            target = backend.reg<uint64_t>(x86_register::rbp);
                            break;
                        case 6:
                            target = backend.reg<uint64_t>(x86_register::rsi);
                            break;
                        case 7:
                            target = backend.reg<uint64_t>(x86_register::rdi);
                            break;
                        default:
                            return false;
                        }
                        continue;
                    }
                }

                return true;
            }

            return true;
        }

        void refresh_index()
        {
            this->address_index.clear();
            for (auto& [_, entry] : this->entries)
            {
                entry.hooks.clear();
            }

            if (this->entries.empty())
            {
                this->remove_execution_hook();
                return;
            }

            this->ensure_execution_hook();

            for (const auto& [_, module] : this->win_emu->mod_manager.modules())
            {
                for (auto& [key, entry] : this->entries)
                {
                    this->add_entry_for_module(key, entry, module);
                }
            }
        }

        void ensure_execution_hook()
        {
            if (this->execution_hook.has_value())
            {
                return;
            }

            auto* hook = this->win_emu->emu().hook_memory_execution([this](uint64_t address) {
                try
                {
                    this->dispatch_address(address);
                }
                catch (...)
                {
                }
            });
            this->execution_hook.emplace(*this->win_emu, hook, nb::none());
        }

        void remove_execution_hook()
        {
            if (!this->execution_hook.has_value())
            {
                return;
            }

            this->execution_hook->remove();
            this->execution_hook.reset();
        }

        void add_entry_for_module(const std::string& key, api_hook_entry& entry, const mapped_module& module)
        {
            if (!api_hook_registry::matches_module(entry, module))
            {
                return;
            }

            for (const auto& export_symbol : module.exports)
            {
                if (export_symbol.name != entry.name)
                {
                    continue;
                }

                const api_hook_hit hit{
                    .key = key, .module_name = module.name, .export_name = export_symbol.name, .address = export_symbol.address};
                this->address_index[export_symbol.address].push_back(hit);

                uint64_t resolved = export_symbol.address;
                if (resolve_jump_target(this->win_emu->emu(), resolved) && resolved != export_symbol.address)
                {
                    auto resolved_hit = hit;
                    resolved_hit.address = resolved;
                    this->address_index[resolved].push_back(resolved_hit);
                }
            }
        }

        void dispatch_address(const uint64_t address)
        {
            auto it = this->address_index.find(address);
            if (it == this->address_index.end())
            {
                return;
            }

            auto& backend = this->win_emu->emu();
            uint64_t return_address{};
            if (!backend.try_read_memory(backend.reg<uint64_t>(x86_register::rsp), &return_address, sizeof(return_address)))
            {
                return;
            }

            auto hits = it->second;
            for (auto& hit : hits)
            {
                hit.return_address = return_address;
                hit.entered_from_call = true;
                this->invoke_hook(hit);
            }
        }
    };

    struct hook_registry
    {
        windows_emulator* emu{};
        std::shared_ptr<api_hook_registry> apis{};

        explicit hook_registry(windows_emulator& emulator)
            : emu(&emulator),
              apis(std::make_shared<api_hook_registry>(emulator))
        {
        }

        hook_handle make_hook(emulator_hook* hook)
        {
            return {*this->emu, hook, nb::cast(this, nb::rv_policy::reference_internal)};
        }

        hook_handle memory_execution(nb::object callback)
        {
            auto* hook = this->emu->emu().hook_memory_execution([cb = std::move(callback)](uint64_t address) {
                nb::gil_scoped_acquire gil{};
                cb(address);
            });
            return make_hook(hook);
        }

        hook_handle memory_execution_at(uint64_t address, nb::object callback)
        {
            auto* hook = this->emu->emu().hook_memory_execution(address, [cb = std::move(callback)](uint64_t addr) {
                nb::gil_scoped_acquire gil{};
                cb(addr);
            });
            return make_hook(hook);
        }

        hook_handle memory_read(uint64_t address, uint64_t size, nb::object callback)
        {
            auto* hook = this->emu->emu().hook_memory_read(
                address, size, [cb = std::move(callback)](uint64_t addr, const void* data, size_t length) {
                    nb::gil_scoped_acquire gil{};
                    cb(addr, nb::bytes(static_cast<const char*>(data), static_cast<nb::ssize_t>(length)));
                });
            return make_hook(hook);
        }

        hook_handle memory_write(uint64_t address, uint64_t size, nb::object callback)
        {
            auto* hook = this->emu->emu().hook_memory_write(
                address, size, [cb = std::move(callback)](uint64_t addr, const void* data, size_t length) {
                    nb::gil_scoped_acquire gil{};
                    cb(addr, nb::bytes(static_cast<const char*>(data), static_cast<nb::ssize_t>(length)));
                });
            return make_hook(hook);
        }

        hook_handle instruction(int instruction_type, nb::object callback)
        {
            auto* hook = this->emu->emu().hook_instruction(static_cast<x86_hookable_instructions>(instruction_type),
                                                           [cb = std::move(callback)](uint64_t data) {
                                                               nb::gil_scoped_acquire gil{};
                                                               return coerce_instruction_continuation(cb(data));
                                                           });
            return make_hook(hook);
        }

        hook_handle interrupt(nb::object callback)
        {
            auto* hook = this->emu->emu().hook_interrupt([cb = std::move(callback)](int interrupt) {
                nb::gil_scoped_acquire gil{};
                cb(interrupt);
            });
            return make_hook(hook);
        }

        hook_handle memory_violation(nb::object callback)
        {
            auto* hook = this->emu->emu().hook_memory_violation(
                [cb = std::move(callback)](uint64_t address, size_t size, memory_operation operation, memory_violation_type type) {
                    nb::gil_scoped_acquire gil{};
                    return coerce_memory_violation_continuation(cb(address, size, operation, type));
                });
            return make_hook(hook);
        }

        hook_handle basic_block(nb::object callback)
        {
            auto* hook = this->emu->emu().hook_basic_block([cb = std::move(callback)](const ::basic_block& block) {
                nb::gil_scoped_acquire gil{};
                cb(block);
            });
            return make_hook(hook);
        }
    };

    struct callback_registry
    {
        windows_emulator* emu{};
        nb::object module_load_cb = nb::none();
        nb::object module_unload_cb = nb::none();
        nb::object stdout_cb = nb::none();
        nb::object syscall_cb = nb::none();
        nb::object generic_access_cb = nb::none();
        nb::object generic_activity_cb = nb::none();
        nb::object suspicious_activity_cb = nb::none();
        nb::object exception_cb = nb::none();
        nb::object instruction_cb = nb::none();
        nb::object memory_protect_cb = nb::none();
        nb::object memory_allocate_cb = nb::none();
        nb::object memory_violate_cb = nb::none();
        nb::object rdtsc_cb = nb::none();
        nb::object rdtscp_cb = nb::none();
        nb::object ioctrl_cb = nb::none();
        nb::object debug_string_cb = nb::none();
        nb::object thread_create_cb = nb::none();
        nb::object thread_terminated_cb = nb::none();
        nb::object thread_set_name_cb = nb::none();
        nb::object thread_switch_cb = nb::none();

        explicit callback_registry(windows_emulator& emulator)
            : emu(&emulator)
        {
            this->emu->callbacks.on_module_load.add([this](mapped_module& mod) { invoke_callback(this->module_load_cb, mod); });
            this->emu->callbacks.on_module_unload.add([this](mapped_module& mod) { invoke_callback(this->module_unload_cb, mod); });
            this->emu->callbacks.on_stdout = [this](std::string_view data) { invoke_callback(this->stdout_cb, std::string(data)); };
            this->emu->callbacks.on_syscall = [this](const uint32_t syscall_id, const std::string_view syscall_name) {
                if (this->syscall_cb.is_none())
                {
                    return instruction_hook_continuation::run_instruction;
                }

                nb::gil_scoped_acquire gil{};
                const auto result = this->syscall_cb(syscall_id, std::string(syscall_name));
                return coerce_instruction_continuation(result);
            };
            this->emu->callbacks.on_generic_access = [this](std::string_view type, std::u16string_view name) {
                invoke_callback(this->generic_access_cb, std::string(type), u16_to_u8(name));
            };
            this->emu->callbacks.on_generic_activity = [this](std::string_view description) {
                invoke_callback(this->generic_activity_cb, std::string(description));
            };
            this->emu->callbacks.on_suspicious_activity = [this](std::string_view description) {
                invoke_callback(this->suspicious_activity_cb, std::string(description));
            };
            this->emu->callbacks.on_exception = [this] { invoke_callback(this->exception_cb); };
            this->emu->callbacks.on_instruction = [this](const uint64_t address) { invoke_callback(this->instruction_cb, address); };
            this->emu->callbacks.on_memory_protect = [this](uint64_t address, uint64_t length, memory_permission permission) {
                invoke_callback(this->memory_protect_cb, address, length, permission);
            };
            this->emu->callbacks.on_memory_allocate = [this](uint64_t address, uint64_t length, memory_permission permission, bool commit) {
                invoke_callback(this->memory_allocate_cb, address, length, permission, commit);
            };
            this->emu->callbacks.on_memory_violate = [this](uint64_t address, uint64_t length, memory_operation operation,
                                                            memory_violation_type type) {
                if (this->memory_violate_cb.is_none())
                {
                    return memory_violation_continuation::resume;
                }

                nb::gil_scoped_acquire gil{};
                const auto result = this->memory_violate_cb(address, length, operation, type);
                return coerce_memory_violation_continuation(result);
            };
            this->emu->callbacks.on_rdtsc = [this] { invoke_callback(this->rdtsc_cb); };
            this->emu->callbacks.on_rdtscp = [this] { invoke_callback(this->rdtscp_cb); };
            this->emu->callbacks.on_ioctrl = [this](io_device&, std::u16string_view device_name, ULONG code) {
                invoke_callback(this->ioctrl_cb, u16_to_u8(device_name), static_cast<uint32_t>(code));
            };
            this->emu->callbacks.on_debug_string.add(
                [this](std::string_view message) { invoke_callback(this->debug_string_cb, std::string(message)); });

            if (this->emu->process.callbacks_)
            {
                this->emu->process.callbacks_->on_thread_create = [this](handle h, emulator_thread& thr) {
                    invoke_callback(this->thread_create_cb, h.bits, thr.id, thr.start_address, thr.argument);
                };
                this->emu->process.callbacks_->on_thread_terminated = [this](handle h, emulator_thread& thr) {
                    invoke_callback(this->thread_terminated_cb, h.bits, thr.id);
                };
                this->emu->process.callbacks_->on_thread_set_name = [this](emulator_thread& thr) {
                    invoke_callback(this->thread_set_name_cb, thr.id, u16_to_u8(thr.name));
                };
                this->emu->process.callbacks_->on_thread_switch = [this](emulator_thread& current_thread, emulator_thread& new_thread) {
                    invoke_callback(this->thread_switch_cb, current_thread.id, new_thread.id);
                };
            }
        }

        void set(const std::string_view name, nb::object callable)
        {
            const std::string key = name.starts_with("on_") ? std::string(name.substr(3)) : std::string(name);

            if (callable.is_valid() && !callable.is_none() && !PyCallable_Check(callable.ptr()))
            {
                throw std::runtime_error("callback must be callable or None");
            }

            const auto assign = [&](nb::object& slot) { slot = std::move(callable); };

            if (key == "module_load")
            {
                assign(this->module_load_cb);
            }
            else if (key == "module_unload")
            {
                assign(this->module_unload_cb);
            }
            else if (key == "stdout")
            {
                assign(this->stdout_cb);
            }
            else if (key == "syscall")
            {
                assign(this->syscall_cb);
            }
            else if (key == "generic_access")
            {
                assign(this->generic_access_cb);
            }
            else if (key == "generic_activity")
            {
                assign(this->generic_activity_cb);
            }
            else if (key == "suspicious_activity")
            {
                assign(this->suspicious_activity_cb);
            }
            else if (key == "exception")
            {
                assign(this->exception_cb);
            }
            else if (key == "instruction")
            {
                assign(this->instruction_cb);
            }
            else if (key == "memory_protect")
            {
                assign(this->memory_protect_cb);
            }
            else if (key == "memory_allocate")
            {
                assign(this->memory_allocate_cb);
            }
            else if (key == "memory_violate")
            {
                assign(this->memory_violate_cb);
            }
            else if (key == "rdtsc")
            {
                assign(this->rdtsc_cb);
            }
            else if (key == "rdtscp")
            {
                assign(this->rdtscp_cb);
            }
            else if (key == "ioctrl")
            {
                assign(this->ioctrl_cb);
            }
            else if (key == "debug_string")
            {
                assign(this->debug_string_cb);
            }
            else if (key == "thread_create")
            {
                assign(this->thread_create_cb);
            }
            else if (key == "thread_terminated")
            {
                assign(this->thread_terminated_cb);
            }
            else if (key == "thread_set_name")
            {
                assign(this->thread_set_name_cb);
            }
            else if (key == "thread_switch")
            {
                assign(this->thread_switch_cb);
            }
            else
            {
                throw std::runtime_error("Unknown callback name: " + key);
            }
        }

        void clear(const std::string_view name)
        {
            set(name, nb::none());
        }
    };

    struct sogen_process_context
    {
        process_context* ctx{};
        std::shared_ptr<callback_registry> callbacks{};
        nb::object owner = nb::none();

        explicit sogen_process_context(process_context& context, std::shared_ptr<callback_registry> callback_registry, nb::object owner)
            : ctx(&context),
              callbacks(std::move(callback_registry)),
              owner(std::move(owner))
        {
        }

        bool is_wow64_process() const
        {
            return this->ctx->is_wow64_process;
        }

        std::optional<NTSTATUS> exit_status() const
        {
            return this->ctx->exit_status;
        }

        size_t live_thread_count() const
        {
            return this->ctx->get_live_thread_count();
        }

        uint32_t spawned_thread_count() const
        {
            return this->ctx->spawned_thread_count;
        }

        emulator_thread* active_thread() const
        {
            return this->ctx->active_thread;
        }

        callback_registry& callback_view() const
        {
            return *this->callbacks;
        }
    };

    struct sogen_windows_emulator
    {
        std::unique_ptr<windows_emulator> emu{};
        std::shared_ptr<callback_registry> callbacks{};
        std::shared_ptr<hook_registry> hooks{};

        explicit sogen_windows_emulator(std::unique_ptr<windows_emulator> emulator)
            : emu(std::move(emulator)),
              callbacks(std::make_shared<callback_registry>(*this->emu)),
              hooks(std::make_shared<hook_registry>(*this->emu))
        {
        }

        windows_emulator& native() const
        {
            return *this->emu;
        }

        void start(size_t count = 0) const
        {
            this->emu->start(count);
        }

        void run(size_t count = 0) const
        {
            this->emu->start(count);
        }

        void stop() const
        {
            this->emu->stop();
        }

        void save_snapshot() const
        {
            this->emu->save_snapshot();
        }

        void restore_snapshot() const
        {
            this->emu->restore_snapshot();
        }

        nb::bytes serialize_state() const
        {
            return serialize_state_bytes(*this->emu);
        }

        void deserialize_state(const nb::bytes& buffer) const
        {
            deserialize_state_bytes(*this->emu, buffer);
        }

        void setup_process_if_necessary() const
        {
            this->emu->setup_process_if_necessary();
        }

        void yield_thread(bool alertable = false) const
        {
            this->emu->yield_thread(alertable);
        }

        bool perform_thread_switch() const
        {
            return this->emu->perform_thread_switch();
        }

        bool activate_thread(uint32_t id) const
        {
            return this->emu->activate_thread(id);
        }

        sogen_process_context process()
        {
            return sogen_process_context(this->emu->process, this->callbacks, nb::cast(this, nb::rv_policy::reference_internal));
        }

        memory_manager& memory() const
        {
            return this->emu->memory;
        }

        emulator_thread* current_thread() const
        {
            return this->emu->process.active_thread;
        }

        std::optional<uint32_t> current_thread_id() const
        {
            if (!this->emu->process.active_thread)
            {
                return std::nullopt;
            }

            return this->emu->process.active_thread->id;
        }

        nb::bytes read_memory(const uint64_t address, const size_t size) const
        {
            return read_memory_bytes(this->emu->memory, address, size);
        }

        void write_memory(const uint64_t address, const nb::bytes& buffer) const
        {
            write_memory_bytes(this->emu->memory, address, buffer);
        }

        uint64_t read_register(const x86_register reg) const
        {
            return this->emu->emu().reg<uint64_t>(reg);
        }

        void write_register(const x86_register reg, const uint64_t value) const
        {
            this->emu->emu().reg<uint64_t>(reg, value);
        }

        uint16_t get_host_port(const uint16_t emulator_port) const
        {
            return this->emu->get_host_port(emulator_port);
        }

        uint16_t get_emulator_port(const uint16_t host_port) const
        {
            return this->emu->get_emulator_port(host_port);
        }

        void map_port(const uint16_t emulator_port, const uint16_t host_port) const
        {
            this->emu->map_port(emulator_port, host_port);
        }
    };

}

void register_sogen_types_bindings(nb::module_& m);
void register_sogen_runtime_bindings(nb::module_& m);
void register_sogen_bindings(nb::module_& m);
