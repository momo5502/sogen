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
            linux_callback_slot{.name = "memory_violate", .member = &linux_callback_registry::memory_violate_cb},
            linux_callback_slot{.name = "signal", .member = &linux_callback_registry::signal_cb},
            linux_callback_slot{.name = "exception", .member = &linux_callback_registry::signal_cb},
            linux_callback_slot{.name = "memory_allocate", .member = &linux_callback_registry::memory_allocate_cb},
            linux_callback_slot{.name = "memory_protect", .member = &linux_callback_registry::memory_protect_cb},
            linux_callback_slot{.name = "memory_release", .member = &linux_callback_registry::memory_release_cb},
            linux_callback_slot{.name = "module_load", .member = &linux_callback_registry::module_load_cb},
            linux_callback_slot{.name = "thread_create", .member = &linux_callback_registry::thread_create_cb},
            linux_callback_slot{.name = "thread_terminated", .member = &linux_callback_registry::thread_terminated_cb},
            linux_callback_slot{.name = "thread_switch", .member = &linux_callback_registry::thread_switch_cb},
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
        this->signal_observer_id = this->emu->on_signal.add([this](int signum, uint64_t fault_addr, int si_code) {
            if (this->signal_cb.is_none())
            {
                return;
            }

            nb::gil_scoped_acquire gil{};
            this->signal_cb(signum, fault_addr, si_code);
        });
        this->memory_allocate_observer_id = this->emu->memory.add_memory_allocate_callback(
            [this](uint64_t address, size_t size, memory_permission permissions, bool committed) {
                if (this->memory_allocate_cb.is_none())
                {
                    return;
                }

                nb::gil_scoped_acquire gil{};
                this->memory_allocate_cb(address, size, permissions, committed);
            });
        this->memory_protect_observer_id =
            this->emu->memory.add_memory_protect_callback([this](uint64_t address, size_t size, memory_permission permissions) {
                if (this->memory_protect_cb.is_none())
                {
                    return;
                }

                nb::gil_scoped_acquire gil{};
                this->memory_protect_cb(address, size, permissions);
            });
        this->memory_release_observer_id = this->emu->memory.add_memory_release_callback([this](uint64_t address, size_t size) {
            if (this->memory_release_cb.is_none())
            {
                return;
            }

            nb::gil_scoped_acquire gil{};
            this->memory_release_cb(address, size);
        });
        this->module_load_observer_id = this->emu->on_module_load.add([this](linux_mapped_module& module) {
            if (this->module_load_cb.is_none())
            {
                return;
            }

            nb::gil_scoped_acquire gil{};
            this->module_load_cb(module);
        });
        this->thread_create_observer_id = this->emu->on_thread_create.add([this](linux_thread& thread) {
            if (this->thread_create_cb.is_none())
            {
                return;
            }

            nb::gil_scoped_acquire gil{};
            this->thread_create_cb(thread);
        });
        this->thread_terminated_observer_id = this->emu->on_thread_terminated.add([this](linux_thread& thread) {
            if (this->thread_terminated_cb.is_none())
            {
                return;
            }

            nb::gil_scoped_acquire gil{};
            this->thread_terminated_cb(thread);
        });
        this->thread_switch_observer_id = this->emu->on_thread_switch.add([this](uint32_t old_tid, uint32_t new_tid) {
            if (this->thread_switch_cb.is_none())
            {
                return;
            }

            nb::gil_scoped_acquire gil{};
            this->thread_switch_cb(old_tid, new_tid);
        });
    }

    linux_callback_registry::~linux_callback_registry()
    {
        if (this->memory_violate_observer_id != 0 && this->emu)
        {
            this->emu->remove_memory_violation_observer(this->memory_violate_observer_id);
        }
        if (this->emu)
        {
            this->emu->memory.remove_memory_callback(this->memory_allocate_observer_id);
            this->emu->memory.remove_memory_callback(this->memory_protect_observer_id);
            this->emu->memory.remove_memory_callback(this->memory_release_observer_id);
            this->emu->on_signal.remove(this->signal_observer_id);
            this->emu->on_module_load.remove(this->module_load_observer_id);
            this->emu->on_thread_create.remove(this->thread_create_observer_id);
            this->emu->on_thread_terminated.remove(this->thread_terminated_observer_id);
            this->emu->on_thread_switch.remove(this->thread_switch_observer_id);
        }
    }

    void linux_callback_registry::refresh_memory_violate_observer()
    {
        if (this->memory_violate_cb.is_none())
        {
            if (this->memory_violate_observer_id != 0)
            {
                this->emu->remove_memory_violation_observer(this->memory_violate_observer_id);
                this->memory_violate_observer_id = 0;
            }
            return;
        }

        if (this->memory_violate_observer_id != 0)
        {
            return;
        }

        this->memory_violate_observer_id = this->emu->add_memory_violation_observer(
            [this](uint64_t address, size_t size, memory_operation operation, memory_violation_type type) {
                if (this->memory_violate_cb.is_none())
                {
                    return memory_violation_continuation::resume;
                }

                nb::gil_scoped_acquire gil{};
                const auto result = this->memory_violate_cb(address, size, operation, type);
                return coerce_memory_violation_continuation(result);
            });
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
        if (member == &linux_callback_registry::memory_violate_cb)
        {
            this->refresh_memory_violate_observer();
        }
        if (member == &linux_callback_registry::module_load_cb && !this->module_load_cb.is_none())
        {
            for (auto& [_, module] : this->emu->mod_manager.get_modules())
            {
                this->module_load_cb(module);
            }
        }
    }

    void linux_callback_registry::clear(const std::string_view name)
    {
        set(name, nb::none());
    }
}
