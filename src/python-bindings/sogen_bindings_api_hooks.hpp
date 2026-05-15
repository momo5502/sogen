#pragma once

#include <disassembler.hpp>

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

    void return_from_api(const api_call_info& call) const
    {
        auto& backend = this->win_emu->emu();
        const auto stack_adjust = is_32bit_code_segment(backend) ? sizeof(uint32_t) : sizeof(uint64_t);
        backend.reg<uint64_t>(x86_register::rax, call.return_value);
        backend.reg<uint64_t>(x86_register::rsp, call.stack_pointer + stack_adjust);
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
        try
        {
            const auto result = coerce_api_continuation(entry.callback(nb::cast(call), params));
            if (result == api_call_continuation::intercept)
            {
                this->return_from_api(*call);
            }
        }
        catch (const std::exception& e)
        {
            PyErr_Clear();
            std::fprintf(stderr, "[sogen] api hook '%s' raised: %s\n", hit.export_name.c_str(), e.what());
            std::fflush(stderr);
        }
        catch (...)
        {
            PyErr_Clear();
            std::fprintf(stderr, "[sogen] api hook '%s' raised an unknown exception\n", hit.export_name.c_str());
            std::fflush(stderr);
        }
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

        // Mirror analyzer behavior: rely on per-module address_names so that thunks/forwarders
        // (e.g. kernel32!Sleep jmp into KERNELBASE!Sleep) are dispatched naturally because both
        // modules expose their own export entries with the same name.
        for (const auto& [address, name] : module.address_names)
        {
            if (name != entry.name)
            {
                continue;
            }

            this->address_index[address].push_back(api_hook_hit{
                .key = key,
                .module_name = module.name,
                .export_name = name,
                .address = address,
            });
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
            this->invoke_hook(hit);
        }
    }
};
