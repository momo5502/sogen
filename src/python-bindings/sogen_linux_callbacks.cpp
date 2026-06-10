#include "sogen_linux_internal.hpp"

namespace sogen::py
{
    namespace
    {
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

        struct linux_callback_slot
        {
            std::string_view name;
            nb::object linux_callback_registry::* member;
        };

        constexpr std::array linux_callback_slots{
            linux_callback_slot{.name = "stdout", .member = &linux_callback_registry::stdout_cb},
            linux_callback_slot{.name = "stderr", .member = &linux_callback_registry::stderr_cb},
            linux_callback_slot{.name = "syscall", .member = &linux_callback_registry::syscall_cb},
            linux_callback_slot{.name = "module_load", .member = &linux_callback_registry::module_load_cb},
            linux_callback_slot{.name = "instruction", .member = &linux_callback_registry::instruction_cb},
            linux_callback_slot{.name = "memory_violate", .member = &linux_callback_registry::memory_violate_cb},
        };
    }

    nb::object linux_callback_registry::* linux_callback_registry::slot_for(const std::string_view name)
    {
        const auto key = name.starts_with("on_") ? name.substr(3) : name;
        for (const auto& slot : linux_callback_slots)
        {
            if (slot.name == key)
            {
                return slot.member;
            }
        }

        return nullptr;
    }

    linux_callback_registry::linux_callback_registry(linux_emulator& emulator, std::shared_ptr<linux_syscall_hook_registry> syscall_hooks)
        : emu(&emulator),
          syscall_hooks(std::move(syscall_hooks))
    {
        this->emu->callbacks.on_stdout = [this](const std::string_view data) { invoke_callback(this->stdout_cb, std::string(data)); };
        this->emu->callbacks.on_stderr = [this](const std::string_view data) { invoke_callback(this->stderr_cb, std::string(data)); };

        this->emu->callbacks.on_syscall = [this](const linux_syscall_info& info) {
            if (this->syscall_hooks)
            {
                const auto named = this->syscall_hooks->invoke(info);
                if (named != linux_syscall_hook_continuation::run_handler)
                {
                    return named;
                }
            }

            if (this->syscall_cb.is_none())
            {
                return linux_syscall_hook_continuation::run_handler;
            }

            nb::gil_scoped_acquire gil{};
            return coerce_linux_syscall_continuation(this->syscall_cb(nb::cast(info)));
        };

        this->emu->callbacks.on_module_load.add([this](const linux_mapped_module& module) { invoke_callback(this->module_load_cb, module); });
        this->emu->callbacks.on_instruction = [this](const uint64_t address) { invoke_callback(this->instruction_cb, address); };

        this->emu->callbacks.on_memory_violate = [this](const uint64_t address, const size_t size, const memory_operation operation,
                                                        const memory_violation_type type) {
            if (this->memory_violate_cb.is_none())
            {
                return memory_violation_continuation::resume;
            }

            nb::gil_scoped_acquire gil{};
            return coerce_memory_violation_continuation(this->memory_violate_cb(address, size, operation, type));
        };
    }

    void linux_callback_registry::set(const std::string_view name, nb::object callable)
    {
        const auto member = slot_for(name);
        if (member == nullptr)
        {
            throw std::invalid_argument(std::string("Unknown callback: ") + std::string(name));
        }

        if (!callable.is_none() && !PyCallable_Check(callable.ptr()))
        {
            throw std::invalid_argument(std::string("Callback must be callable: ") + std::string(name));
        }

        this->*member = std::move(callable);
    }

    void linux_callback_registry::clear(const std::string_view name)
    {
        set(name, nb::none());
    }
}
