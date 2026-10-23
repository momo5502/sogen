#include "sogen_internal.hpp"
#include <windows_emulator.hpp>

#include <cstdio>

namespace sogen::py
{
    namespace
    {
        std::string to_lower_ascii(std::string value)
        {
            std::ranges::transform(value, value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return value;
        }

        api_hook_target parse_api_hook_target(const std::string& key)
        {
            const auto pos = key.find('!');
            if (pos == std::string::npos)
            {
                return {.module = std::nullopt, .name = key};
            }

            return {.module = key.substr(0, pos), .name = key.substr(pos + 1)};
        }

        api_hook_signature read_signature(const nb::object& callback)
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

        bool matches_module(const api_hook_entry& entry, const mapped_module& module)
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
    }

    // ----- hook_handle -----

    hook_handle::hook_state::hook_state(sogen::guest_machine& emulator, emulator_hook* hook)
        : emu(&emulator),
          hook(hook)
    {
    }

    hook_handle::hook_state::hook_state(std::function<void()> remover)
        : remover(std::move(remover))
    {
    }

    hook_handle::hook_state::~hook_state()
    {
        remove();
    }

    void hook_handle::hook_state::remove()
    {
        if (this->remover)
        {
            auto remove_callback = std::move(this->remover);
            this->remover = {};
            this->hook = nullptr;
            this->emu = nullptr;
            try
            {
                remove_callback();
            }
            catch (...)
            {
            }
            return;
        }

        if (!this->emu || !this->hook)
        {
            return;
        }

        auto* hook_to_remove = this->hook;
        this->hook = nullptr;

        try
        {
            this->emu->delete_hook(hook_to_remove);
        }
        catch (...)
        {
        }
    }

    hook_handle::hook_handle(sogen::guest_machine& emulator, emulator_hook* hook, nb::object owner)
        : shared_state(std::make_shared<hook_state>(emulator, hook)),
          owner(std::move(owner))
    {
    }

    hook_handle::hook_handle(std::function<void()> remover, nb::object owner)
        : shared_state(std::make_shared<hook_state>(std::move(remover))),
          owner(std::move(owner))
    {
    }

    void hook_handle::remove() const
    {
        if (this->shared_state)
        {
            this->shared_state->remove();
        }
    }

    // ----- api_hook_registry -----

    api_hook_registry::api_hook_registry(windows_emulator& emulator)
        : win_emu(&emulator)
    {
        this->module_load_id = this->win_emu->callbacks.on_module_load.add([this](mapped_module&) { this->refresh_index(); });
        this->module_unload_id = this->win_emu->callbacks.on_module_unload.add([this](mapped_module&) { this->refresh_index(); });
    }

    api_hook_registry::~api_hook_registry()
    {
        this->clear();
        this->win_emu->callbacks.on_module_load.remove(this->module_load_id);
        this->win_emu->callbacks.on_module_unload.remove(this->module_unload_id);
    }

    void api_hook_registry::clear()
    {
        for (auto& [_, entry] : this->entries)
        {
            entry.hooks.clear();
        }
        this->entries.clear();
        this->address_index.clear();
        this->address_hooks.clear();
    }

    void api_hook_registry::del_item(const std::string& key)
    {
        if (this->entries.erase(key) != 0)
        {
            this->refresh_index();
        }
    }

    void api_hook_registry::set_item(const std::string& key, nb::object callback)
    {
        if (callback.is_none())
        {
            this->del_item(key);
            return;
        }

        auto target = parse_api_hook_target(key);
        auto signature = read_signature(callback);
        auto& entry = this->entries[key];
        entry.module_filter = std::move(target.module);
        entry.name = std::move(target.name);
        entry.callback = std::move(callback);
        entry.cc = signature.cc;
        entry.params = std::move(signature.params);
        this->refresh_index();
    }

    nb::list api_hook_registry::resolve_params(const api_hook_entry& entry) const
    {
        nb::list params{};
        const auto count = entry.params.is_none() ? size_t{} : static_cast<size_t>(nb::len(entry.params));
        auto& cpu = this->win_emu->active_cpu();
        auto* x64 = dynamic_cast<x86_64_cpu*>(&cpu);
        if (!x64)
        {
            for (size_t i = 0; i < count; ++i)
            {
                params.append(this->win_emu->abi().function_argument(cpu, i));
            }
            return params;
        }

        const auto cc = is_32bit_code_segment(*x64) ? entry.cc : function_calling_convention::x64_fastcall;
        for (const auto value : get_function_arguments(*x64, cc, count))
        {
            params.append(value);
        }
        return params;
    }

    void api_hook_registry::return_from_api(const api_call_info& call) const
    {
        auto& cpu = this->win_emu->active_cpu();
        const auto& abi = this->win_emu->abi();
        abi.return_value(cpu, call.return_value);
        if (auto* x64 = dynamic_cast<x86_64_cpu*>(&cpu))
        {
            const auto stack_adjust = is_32bit_code_segment(*x64) ? sizeof(uint32_t) : sizeof(uint64_t);
            abi.stack_pointer(cpu, call.stack_pointer + stack_adjust);
        }
        abi.instruction_pointer(cpu, call.return_address);
    }

    void api_hook_registry::invoke_hook(const api_hook_hit& hit)
    {
        const auto it = this->entries.find(hit.key);
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
        call->stack_pointer = this->win_emu->abi().stack_pointer(this->win_emu->active_cpu());

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

    void api_hook_registry::refresh_index()
    {
        this->address_index.clear();
        for (auto& [_, entry] : this->entries)
        {
            entry.hooks.clear();
        }

        for (const auto& [_, module] : this->win_emu->mod_manager.modules())
        {
            for (auto& [key, entry] : this->entries)
            {
                this->add_entry_for_module(key, entry, module);
            }
        }

        this->sync_address_hooks();
    }

    // One execution hook per hooked export address works on every backend, including the hardware
    // ones that cannot report every executed instruction. Unchanged addresses keep their hooks.
    void api_hook_registry::sync_address_hooks()
    {
        std::erase_if(this->address_hooks, [this](const auto& hook) { return !this->address_index.contains(hook.first); });

        for (const auto& [address, _] : this->address_index)
        {
            if (this->address_hooks.contains(address))
            {
                continue;
            }

            auto* hook = this->win_emu->emu().hook_memory_execution(address, [this, address](cpu_interface&, uint64_t) {
                try
                {
                    this->dispatch_address(address);
                }
                catch (...)
                {
                }
            });
            this->address_hooks.emplace(address, hook_handle{this->win_emu->emu(), hook, nb::none()});
        }
    }

    void api_hook_registry::add_entry_for_module(const std::string& key, api_hook_entry& entry, const mapped_module& module)
    {
        if (!matches_module(entry, module))
        {
            return;
        }

        // Mirror analyzer behavior: rely on per-module address_names so that
        // thunks/forwarders (e.g. kernel32!Sleep jmp into KERNELBASE!Sleep) are
        // dispatched naturally because both modules expose their own export
        // entries with the same name.
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

    void api_hook_registry::dispatch_address(const uint64_t address)
    {
        const auto it = this->address_index.find(address);
        if (it == this->address_index.end())
        {
            return;
        }

        auto& cpu = this->win_emu->active_cpu();
        uint64_t return_address{};
        if (auto* arm64 = dynamic_cast<arm64_cpu*>(&cpu))
        {
            return_address = arm64->reg(arm64_register::lr);
        }
        else if (!cpu.try_read_memory(cpu.read_stack_pointer(), &return_address, sizeof(return_address)))
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
}
