#include "sogen_internal.hpp"

#include <linux_emulator.hpp>

#include <array>

namespace sogen::py
{
    namespace
    {
        void invoke_linux_callback(const nb::object& cb, std::string_view data)
        {
            if (cb.is_none())
            {
                return;
            }

            nb::gil_scoped_acquire gil{};
            cb(std::string(data));
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

    linux_callback_registry::linux_callback_registry(linux_emulator& emulator)
        : emu(&emulator)
    {
        this->emu->on_stdout = [this](std::string_view data) { invoke_linux_callback(this->stdout_cb, data); };
        this->emu->on_stderr = [this](std::string_view data) { invoke_linux_callback(this->stderr_cb, data); };
        this->emu->on_syscall = [this](const uint64_t syscall_id, const std::string_view syscall_name) {
            if (this->syscall_cb.is_none())
            {
                return instruction_hook_continuation::run_instruction;
            }

            nb::gil_scoped_acquire gil{};
            const auto result = this->syscall_cb(syscall_id, std::string(syscall_name));
            return coerce_instruction_continuation(result);
        };
    }

    void linux_callback_registry::set(const std::string_view name, nb::object callable)
    {
        if (callable.is_valid() && !callable.is_none() && !PyCallable_Check(callable.ptr()))
        {
            throw std::runtime_error("callback must be callable or None");
        }

        const auto member = slot_for(name);
        if (!member)
        {
            throw std::runtime_error("Unknown callback name: " + std::string(name));
        }

        this->*member = std::move(callable);
    }

    void linux_callback_registry::clear(const std::string_view name)
    {
        set(name, nb::none());
    }
}
