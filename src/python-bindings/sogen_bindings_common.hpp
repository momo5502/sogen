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

    std::vector<std::u16string> parse_arguments(nb::object object)
    {
        std::vector<std::u16string> result{};
        if (object.is_none())
        {
            return result;
        }

        const nb::sequence seq = nb::cast<nb::sequence>(object);
        result.reserve(static_cast<size_t>(nb::len(seq)));
        for (const auto& item : seq)
        {
            result.emplace_back(u8_to_u16(nb::cast<std::string>(item)));
        }

        return result;
    }

    utils::unordered_insensitive_u16string_map<std::u16string> parse_environment(nb::object object)
    {
        utils::unordered_insensitive_u16string_map<std::u16string> result{};
        if (object.is_none())
        {
            return result;
        }

        const nb::dict dict = nb::cast<nb::dict>(object);
        for (const auto& item : dict)
        {
            result.emplace(u8_to_u16(nb::cast<std::string>(item.first)), u8_to_u16(nb::cast<std::string>(item.second)));
        }

        return result;
    }

    std::unordered_map<windows_path, std::filesystem::path> parse_path_mappings(nb::object object)
    {
        std::unordered_map<windows_path, std::filesystem::path> result{};
        if (object.is_none())
        {
            return result;
        }

        const nb::dict dict = nb::cast<nb::dict>(object);
        for (const auto& item : dict)
        {
            result.emplace(nb::cast<std::filesystem::path>(item.first), nb::cast<std::filesystem::path>(item.second));
        }

        return result;
    }

    std::unordered_map<uint16_t, uint16_t> parse_port_mappings(nb::object object)
    {
        std::unordered_map<uint16_t, uint16_t> result{};
        if (object.is_none())
        {
            return result;
        }

        const nb::dict dict = nb::cast<nb::dict>(object);
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

    application_settings make_application_settings(nb::object application, nb::object args, const nb::kwargs& kwargs)
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

    [[maybe_unused]] std::unique_ptr<windows_emulator> create_application_emulator(nb::object application, nb::object args,
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

    struct hook_registry
    {
        windows_emulator* emu{};

        explicit hook_registry(windows_emulator& emulator)
            : emu(&emulator)
        {
        }

        hook_handle make_hook(emulator_hook* hook)
        {
            return hook_handle(*this->emu, hook, nb::cast(this, nb::rv_policy::reference_internal));
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

        nb::object active_thread()
        {
            if (!this->ctx->active_thread)
            {
                return nb::none();
            }

            return nb::cast(this->ctx->active_thread, nb::rv_policy::reference_internal);
        }

        callback_registry& callback_view()
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

        windows_emulator& native()
        {
            return *this->emu;
        }

        const windows_emulator& native() const
        {
            return *this->emu;
        }

        void start(size_t count = 0)
        {
            this->emu->start(count);
        }

        void run(size_t count = 0)
        {
            this->emu->start(count);
        }

        void stop()
        {
            this->emu->stop();
        }

        void setup_process_if_necessary()
        {
            this->emu->setup_process_if_necessary();
        }

        void yield_thread(bool alertable = false)
        {
            this->emu->yield_thread(alertable);
        }

        bool perform_thread_switch()
        {
            return this->emu->perform_thread_switch();
        }

        bool activate_thread(uint32_t id)
        {
            return this->emu->activate_thread(id);
        }

        sogen_process_context process()
        {
            return sogen_process_context(this->emu->process, this->callbacks, nb::cast(this, nb::rv_policy::reference_internal));
        }

        nb::object memory()
        {
            return nb::cast(&this->emu->memory, nb::rv_policy::reference_internal);
        }

        nb::object current_thread()
        {
            if (!this->emu->process.active_thread)
            {
                return nb::none();
            }

            return nb::cast(this->emu->process.active_thread, nb::rv_policy::reference_internal);
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

        void write_memory(const uint64_t address, const nb::bytes& buffer)
        {
            write_memory_bytes(this->emu->memory, address, buffer);
        }

        uint64_t read_register(const x86_register reg) const
        {
            return this->emu->emu().reg<uint64_t>(reg);
        }

        void write_register(const x86_register reg, const uint64_t value)
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

        void map_port(const uint16_t emulator_port, const uint16_t host_port)
        {
            this->emu->map_port(emulator_port, host_port);
        }
    };

}

void register_sogen_types_bindings(nb::module_& m);
void register_sogen_runtime_bindings(nb::module_& m);
void register_sogen_bindings(nb::module_& m);
