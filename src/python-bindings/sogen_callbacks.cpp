#include "sogen_internal.hpp"

#include <array>

namespace sogen_py
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

        // Maps user-facing slot name to the corresponding nb::object member
        // pointer. Single source of truth for both `set` and the property
        // bindings in sogen_bindings_runtime.cpp.
        struct callback_slot
        {
            std::string_view name;
            nb::object callback_registry::* member;
        };

        constexpr std::array callback_slots{
            callback_slot{"module_load", &callback_registry::module_load_cb},
            callback_slot{"module_unload", &callback_registry::module_unload_cb},
            callback_slot{"stdout", &callback_registry::stdout_cb},
            callback_slot{"syscall", &callback_registry::syscall_cb},
            callback_slot{"generic_access", &callback_registry::generic_access_cb},
            callback_slot{"generic_activity", &callback_registry::generic_activity_cb},
            callback_slot{"suspicious_activity", &callback_registry::suspicious_activity_cb},
            callback_slot{"exception", &callback_registry::exception_cb},
            callback_slot{"instruction", &callback_registry::instruction_cb},
            callback_slot{"memory_protect", &callback_registry::memory_protect_cb},
            callback_slot{"memory_allocate", &callback_registry::memory_allocate_cb},
            callback_slot{"memory_violate", &callback_registry::memory_violate_cb},
            callback_slot{"rdtsc", &callback_registry::rdtsc_cb},
            callback_slot{"rdtscp", &callback_registry::rdtscp_cb},
            callback_slot{"ioctrl", &callback_registry::ioctrl_cb},
            callback_slot{"debug_string", &callback_registry::debug_string_cb},
            callback_slot{"thread_create", &callback_registry::thread_create_cb},
            callback_slot{"thread_terminated", &callback_registry::thread_terminated_cb},
            callback_slot{"thread_set_name", &callback_registry::thread_set_name_cb},
            callback_slot{"thread_switch", &callback_registry::thread_switch_cb},
        };
    }

    nb::object callback_registry::* callback_registry::slot_for(const std::string_view name)
    {
        const auto key = name.starts_with("on_") ? name.substr(3) : name;
        for (const auto& slot : callback_slots)
        {
            if (slot.name == key)
            {
                return slot.member;
            }
        }
        return nullptr;
    }

    callback_registry::callback_registry(windows_emulator& emulator)
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

    void callback_registry::set(const std::string_view name, nb::object callable)
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

    void callback_registry::clear(const std::string_view name)
    {
        set(name, nb::none());
    }
}
