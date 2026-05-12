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
    std::string stop_reason_to_string(const stop_reason reason)
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

    std::unique_ptr<windows_emulator> create_empty_emulator(const nb::kwargs& kwargs)
    {
        return std::make_unique<windows_emulator>(create_x86_64_emulator(), make_emulator_settings(kwargs));
    }

    std::unique_ptr<windows_emulator> create_application_emulator(nb::object application, nb::object args, const nb::kwargs& kwargs)
    {
        auto app_settings = make_application_settings(application, args, kwargs);
        return std::make_unique<windows_emulator>(create_x86_64_emulator(), std::move(app_settings), make_emulator_settings(kwargs));
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

        explicit sogen_process_context(process_context& context, std::shared_ptr<callback_registry> callback_registry)
            : ctx(&context),
              callbacks(std::move(callback_registry))
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
            return sogen_process_context(this->emu->process, this->callbacks);
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

NB_MODULE(sogen, m)
{
    m.doc() = "Sogen Python bindings";

    nb::enum_<memory_permission>(m, "MemoryPermission")
        .value("none", memory_permission::none)
        .value("read", memory_permission::read)
        .value("write", memory_permission::write)
        .value("exec", memory_permission::exec)
        .value("read_write", memory_permission::read_write)
        .value("all", memory_permission::all)
        .export_values();

    nb::enum_<memory_region_kind>(m, "MemoryRegionKind")
        .value("free", memory_region_kind::free)
        .value("private_allocation", memory_region_kind::private_allocation)
        .value("file_section_view", memory_region_kind::file_section_view)
        .value("pagefile_section_view", memory_region_kind::pagefile_section_view)
        .value("section_image", memory_region_kind::section_image)
        .value("mmio", memory_region_kind::mmio)
        .export_values();

    nb::enum_<memory_violation_type>(m, "MemoryViolationType")
        .value("unmapped", memory_violation_type::unmapped)
        .value("protection", memory_violation_type::protection)
        .export_values();

    nb::enum_<x86_register>(m, "Register")
        .value("invalid", x86_register::invalid)
        .value("rax", x86_register::rax)
        .value("rbx", x86_register::rbx)
        .value("rcx", x86_register::rcx)
        .value("rdx", x86_register::rdx)
        .value("rsi", x86_register::rsi)
        .value("rdi", x86_register::rdi)
        .value("rbp", x86_register::rbp)
        .value("rsp", x86_register::rsp)
        .value("rip", x86_register::rip)
        .value("r8", x86_register::r8)
        .value("r9", x86_register::r9)
        .value("r10", x86_register::r10)
        .value("r11", x86_register::r11)
        .value("r12", x86_register::r12)
        .value("r13", x86_register::r13)
        .value("r14", x86_register::r14)
        .value("r15", x86_register::r15)
        .value("eax", x86_register::eax)
        .value("ebx", x86_register::ebx)
        .value("ecx", x86_register::ecx)
        .value("edx", x86_register::edx)
        .value("esi", x86_register::esi)
        .value("edi", x86_register::edi)
        .value("ebp", x86_register::ebp)
        .value("esp", x86_register::esp)
        .value("eip", x86_register::eip)
        .value("eflags", x86_register::eflags)
        .value("rflags", x86_register::rflags)
        .value("cs", x86_register::cs)
        .value("ss", x86_register::ss)
        .value("ds", x86_register::ds)
        .value("es", x86_register::es)
        .value("fs", x86_register::fs)
        .value("gs", x86_register::gs)
        .value("xmm0", x86_register::xmm0)
        .value("xmm1", x86_register::xmm1)
        .value("xmm2", x86_register::xmm2)
        .value("xmm3", x86_register::xmm3)
        .value("xmm4", x86_register::xmm4)
        .value("xmm5", x86_register::xmm5)
        .value("xmm6", x86_register::xmm6)
        .value("xmm7", x86_register::xmm7)
        .value("xmm8", x86_register::xmm8)
        .value("xmm9", x86_register::xmm9)
        .value("xmm10", x86_register::xmm10)
        .value("xmm11", x86_register::xmm11)
        .value("xmm12", x86_register::xmm12)
        .value("xmm13", x86_register::xmm13)
        .value("xmm14", x86_register::xmm14)
        .value("xmm15", x86_register::xmm15)
        .export_values();

    nb::class_<memory_stats>(m, "MemoryStats")
        .def_prop_ro("reserved_memory", [](const memory_stats& self) { return self.reserved_memory; })
        .def_prop_ro("committed_memory", [](const memory_stats& self) { return self.committed_memory; });

    nb::class_<handle>(m, "Handle")
        .def(nb::init<>())
        .def_prop_rw(
            "bits", [](const handle& self) { return self.bits; }, [](handle& self, uint64_t value) { self.bits = value; })
        .def_prop_ro("id", [](const handle& self) { return self.value.id; })
        .def_prop_ro("type", [](const handle& self) { return self.value.type; })
        .def_prop_ro("is_system", [](const handle& self) { return self.value.is_system != 0; })
        .def_prop_ro("is_pseudo", [](const handle& self) { return self.value.is_pseudo != 0; })
        .def_prop_ro("high_bits", [](const handle& self) { return self.value.high_bits; });

    nb::class_<region_info>(m, "MemoryRegionInfo")
        .def_prop_ro("start", [](const region_info& self) { return self.start; })
        .def_prop_ro("length", [](const region_info& self) { return self.length; })
        .def_prop_ro("permissions", [](const region_info& self) { return self.permissions; })
        .def_prop_ro("allocation_base", [](const region_info& self) { return self.allocation_base; })
        .def_prop_ro("allocation_length", [](const region_info& self) { return self.allocation_length; })
        .def_prop_ro("is_reserved", [](const region_info& self) { return self.is_reserved; })
        .def_prop_ro("is_committed", [](const region_info& self) { return self.is_committed; })
        .def_prop_ro("initial_permissions", [](const region_info& self) { return self.initial_permissions; })
        .def_prop_ro("kind", [](const region_info& self) { return self.kind; });

    nb::enum_<instruction_hook_continuation>(m, "HookContinuation")
        .value("run", instruction_hook_continuation::run_instruction)
        .value("skip", instruction_hook_continuation::skip_instruction)
        .export_values();

    nb::enum_<memory_violation_continuation>(m, "MemoryViolationContinuation")
        .value("stop", memory_violation_continuation::stop)
        .value("resume", memory_violation_continuation::resume)
        .value("restart", memory_violation_continuation::restart)
        .export_values();

    nb::enum_<x86_hookable_instructions>(m, "Instruction")
        .value("invalid", x86_hookable_instructions::invalid)
        .value("syscall", x86_hookable_instructions::syscall)
        .value("cpuid", x86_hookable_instructions::cpuid)
        .value("rdtsc", x86_hookable_instructions::rdtsc)
        .value("rdtscp", x86_hookable_instructions::rdtscp)
        .export_values();

    nb::class_<basic_block>(m, "BasicBlock")
        .def_prop_ro("address", [](const basic_block& self) { return self.address; })
        .def_prop_ro("instruction_count", [](const basic_block& self) { return self.instruction_count; })
        .def_prop_ro("size", [](const basic_block& self) { return self.size; });

    nb::class_<mapped_module>(m, "MappedModule")
        .def_prop_ro("name", [](const mapped_module& self) { return self.name; })
        .def_prop_ro("path", [](const mapped_module& self) { return self.path; })
        .def_prop_ro("module_path", [](const mapped_module& self) { return self.module_path.string(); })
        .def_prop_ro("image_base", [](const mapped_module& self) { return self.image_base; })
        .def_prop_ro("image_base_file", [](const mapped_module& self) { return self.image_base_file; })
        .def_prop_ro("size_of_image", [](const mapped_module& self) { return self.size_of_image; })
        .def_prop_ro("entry_point", [](const mapped_module& self) { return self.entry_point; })
        .def_prop_ro("is_static", [](const mapped_module& self) { return self.is_static; });

    nb::class_<hook_handle>(m, "Hook").def("remove", &hook_handle::remove).def_prop_ro("active", &hook_handle::active);

    nb::class_<hook_registry>(m, "Hooks")
        .def("memory_execution", &hook_registry::memory_execution)
        .def("memory_execution_at", &hook_registry::memory_execution_at)
        .def("memory_read", &hook_registry::memory_read)
        .def("memory_write", &hook_registry::memory_write)
        .def("instruction", &hook_registry::instruction)
        .def("interrupt", &hook_registry::interrupt)
        .def("memory_violation", &hook_registry::memory_violation)
        .def("basic_block", &hook_registry::basic_block);

    nb::class_<memory_manager>(m, "MemoryManager")
        .def("read_memory",
             [](const memory_manager& self, uint64_t address, size_t size) { return read_memory_bytes(self, address, size); })
        .def("write_memory",
             [](memory_manager& self, uint64_t address, const nb::bytes& buffer) { write_memory_bytes(self, address, buffer); })
        .def(
            "allocate_memory",
            [](memory_manager& self, size_t size, memory_permission permissions, bool reserve_only, uint64_t start,
               memory_region_kind kind) { return self.allocate_memory(size, permissions, reserve_only, start, kind); },
            nb::arg("size"), nb::arg("permissions"), nb::arg("reserve_only") = false, nb::arg("start") = 0,
            nb::arg("kind") = memory_region_kind::private_allocation)
        .def("protect_memory",
             [](memory_manager& self, uint64_t address, size_t size, memory_permission permissions) {
                 return self.protect_memory(address, size, nt_memory_permission{permissions});
             })
        .def("commit_memory",
             [](memory_manager& self, uint64_t address, size_t size, memory_permission permissions) {
                 return self.commit_memory(address, size, nt_memory_permission{permissions});
             })
        .def("decommit_memory", &memory_manager::decommit_memory)
        .def("release_memory", &memory_manager::release_memory)
        .def("find_free_allocation_base",
             static_cast<uint64_t (memory_manager::*)(size_t, uint64_t) const>(&memory_manager::find_free_allocation_base))
        .def("get_region_info", &memory_manager::get_region_info)
        .def("compute_memory_stats", &memory_manager::compute_memory_stats)
        .def_prop_rw("default_allocation_address", &memory_manager::get_default_allocation_address,
                     &memory_manager::set_default_allocation_address);

    nb::class_<emulator_thread>(m, "Thread")
        .def_prop_ro("id", [](const emulator_thread& self) { return self.id; })
        .def_prop_ro("name", [](const emulator_thread& self) { return u16_to_u8(self.name); })
        .def_prop_ro("start_address", [](const emulator_thread& self) { return self.start_address; })
        .def_prop_ro("argument", [](const emulator_thread& self) { return self.argument; })
        .def_prop_ro("executed_instructions", [](const emulator_thread& self) { return self.executed_instructions; })
        .def_prop_ro("current_ip", [](const emulator_thread& self) { return self.current_ip; })
        .def_prop_ro("previous_ip", [](const emulator_thread& self) { return self.previous_ip; })
        .def_prop_ro("setup_done", [](const emulator_thread& self) { return self.setup_done; })
        .def_prop_ro("exit_status", [](const emulator_thread& self) -> nb::object {
            if (!self.exit_status.has_value())
            {
                return nb::none();
            }

            return nb::int_(*self.exit_status);
        });

    nb::class_<callback_registry>(m, "Callbacks")
        .def("set", [](callback_registry& self, const std::string& name, nb::object callback) { self.set(name, std::move(callback)); })
        .def("clear", [](callback_registry& self, const std::string& name) { self.clear(name); })
        .def_prop_rw(
            "on_module_load", [](callback_registry& self) { return self.module_load_cb; },
            [](callback_registry& self, nb::object callback) { self.set("module_load", std::move(callback)); })
        .def_prop_rw(
            "on_module_unload", [](callback_registry& self) { return self.module_unload_cb; },
            [](callback_registry& self, nb::object callback) { self.set("module_unload", std::move(callback)); })
        .def_prop_rw(
            "on_stdout", [](callback_registry& self) { return self.stdout_cb; },
            [](callback_registry& self, nb::object callback) { self.set("stdout", std::move(callback)); })
        .def_prop_rw(
            "on_syscall", [](callback_registry& self) { return self.syscall_cb; },
            [](callback_registry& self, nb::object callback) { self.set("syscall", std::move(callback)); })
        .def_prop_rw(
            "on_generic_access", [](callback_registry& self) { return self.generic_access_cb; },
            [](callback_registry& self, nb::object callback) { self.set("generic_access", std::move(callback)); })
        .def_prop_rw(
            "on_generic_activity", [](callback_registry& self) { return self.generic_activity_cb; },
            [](callback_registry& self, nb::object callback) { self.set("generic_activity", std::move(callback)); })
        .def_prop_rw(
            "on_suspicious_activity", [](callback_registry& self) { return self.suspicious_activity_cb; },
            [](callback_registry& self, nb::object callback) { self.set("suspicious_activity", std::move(callback)); })
        .def_prop_rw(
            "on_exception", [](callback_registry& self) { return self.exception_cb; },
            [](callback_registry& self, nb::object callback) { self.set("exception", std::move(callback)); })
        .def_prop_rw(
            "on_instruction", [](callback_registry& self) { return self.instruction_cb; },
            [](callback_registry& self, nb::object callback) { self.set("instruction", std::move(callback)); })
        .def_prop_rw(
            "on_memory_protect", [](callback_registry& self) { return self.memory_protect_cb; },
            [](callback_registry& self, nb::object callback) { self.set("memory_protect", std::move(callback)); })
        .def_prop_rw(
            "on_memory_allocate", [](callback_registry& self) { return self.memory_allocate_cb; },
            [](callback_registry& self, nb::object callback) { self.set("memory_allocate", std::move(callback)); })
        .def_prop_rw(
            "on_memory_violate", [](callback_registry& self) { return self.memory_violate_cb; },
            [](callback_registry& self, nb::object callback) { self.set("memory_violate", std::move(callback)); })
        .def_prop_rw(
            "on_rdtsc", [](callback_registry& self) { return self.rdtsc_cb; },
            [](callback_registry& self, nb::object callback) { self.set("rdtsc", std::move(callback)); })
        .def_prop_rw(
            "on_rdtscp", [](callback_registry& self) { return self.rdtscp_cb; },
            [](callback_registry& self, nb::object callback) { self.set("rdtscp", std::move(callback)); })
        .def_prop_rw(
            "on_ioctrl", [](callback_registry& self) { return self.ioctrl_cb; },
            [](callback_registry& self, nb::object callback) { self.set("ioctrl", std::move(callback)); })
        .def_prop_rw(
            "on_debug_string", [](callback_registry& self) { return self.debug_string_cb; },
            [](callback_registry& self, nb::object callback) { self.set("debug_string", std::move(callback)); })
        .def_prop_rw(
            "on_thread_create", [](callback_registry& self) { return self.thread_create_cb; },
            [](callback_registry& self, nb::object callback) { self.set("thread_create", std::move(callback)); })
        .def_prop_rw(
            "on_thread_terminated", [](callback_registry& self) { return self.thread_terminated_cb; },
            [](callback_registry& self, nb::object callback) { self.set("thread_terminated", std::move(callback)); })
        .def_prop_rw(
            "on_thread_set_name", [](callback_registry& self) { return self.thread_set_name_cb; },
            [](callback_registry& self, nb::object callback) { self.set("thread_set_name", std::move(callback)); })
        .def_prop_rw(
            "on_thread_switch", [](callback_registry& self) { return self.thread_switch_cb; },
            [](callback_registry& self, nb::object callback) { self.set("thread_switch", std::move(callback)); });

    nb::class_<sogen_process_context>(m, "ProcessContext")
        .def_prop_ro("is_wow64_process", [](const sogen_process_context& self) { return self.is_wow64_process(); })
        .def_prop_ro("exit_status",
                     [](const sogen_process_context& self) -> nb::object {
                         if (!self.exit_status().has_value())
                         {
                             return nb::none();
                         }

                         return nb::int_(*self.exit_status());
                     })
        .def_prop_ro("live_thread_count", &sogen_process_context::live_thread_count)
        .def_prop_ro("spawned_thread_count", &sogen_process_context::spawned_thread_count)
        .def_prop_ro("active_thread", &sogen_process_context::active_thread, nb::rv_policy::reference_internal)
        .def_prop_ro("callbacks", &sogen_process_context::callback_view, nb::rv_policy::reference_internal);

    nb::class_<sogen_windows_emulator>(m, "WindowsEmulator")
        .def("start", &sogen_windows_emulator::start, nb::arg("count") = 0, nb::call_guard<nb::gil_scoped_release>())
        .def("run", &sogen_windows_emulator::run, nb::arg("count") = 0, nb::call_guard<nb::gil_scoped_release>())
        .def("stop", &sogen_windows_emulator::stop)
        .def("setup_process_if_necessary", &sogen_windows_emulator::setup_process_if_necessary, nb::call_guard<nb::gil_scoped_release>())
        .def("yield_thread", &sogen_windows_emulator::yield_thread, nb::arg("alertable") = false, nb::call_guard<nb::gil_scoped_release>())
        .def("perform_thread_switch", &sogen_windows_emulator::perform_thread_switch, nb::call_guard<nb::gil_scoped_release>())
        .def("activate_thread", &sogen_windows_emulator::activate_thread)
        .def_prop_ro("executed_instructions", [](const sogen_windows_emulator& self) { return self.native().get_executed_instructions(); })
        .def_prop_ro("last_stop_reason",
                     [](const sogen_windows_emulator& self) { return stop_reason_to_string(self.native().last_stop_reason()); })
        .def_prop_ro("last_stop_reason_code",
                     [](const sogen_windows_emulator& self) { return static_cast<int>(self.native().last_stop_reason()); })
        .def_prop_ro("last_stop_detail", [](const sogen_windows_emulator& self) { return self.native().last_stop_detail(); })
        .def_prop_ro("backend_name", [](const sogen_windows_emulator& self) { return self.native().emu().get_name(); })
        .def_prop_ro("emulation_root", [](const sogen_windows_emulator& self) { return self.native().emulation_root; })
        .def_prop_ro("process", &sogen_windows_emulator::process, nb::rv_policy::reference_internal)
        .def_prop_ro("memory", &sogen_windows_emulator::memory, nb::rv_policy::reference_internal)
        .def_prop_ro("current_thread", &sogen_windows_emulator::current_thread, nb::rv_policy::reference_internal)
        .def_prop_ro("current_thread_id",
                     [](const sogen_windows_emulator& self) -> nb::object {
                         const auto id = self.current_thread_id();
                         if (!id.has_value())
                         {
                             return nb::none();
                         }

                         return nb::int_(*id);
                     })
        .def_prop_ro(
            "callbacks", [](sogen_windows_emulator& self) -> callback_registry& { return *self.callbacks; },
            nb::rv_policy::reference_internal)
        .def_prop_ro(
            "hooks", [](sogen_windows_emulator& self) -> hook_registry& { return *self.hooks; }, nb::rv_policy::reference_internal)
        .def("read_memory", &sogen_windows_emulator::read_memory)
        .def("write_memory", &sogen_windows_emulator::write_memory)
        .def("read_register", &sogen_windows_emulator::read_register)
        .def("write_register", &sogen_windows_emulator::write_register)
        .def("get_host_port", &sogen_windows_emulator::get_host_port)
        .def("get_emulator_port", &sogen_windows_emulator::get_emulator_port)
        .def("map_port", &sogen_windows_emulator::map_port);

    m.def("create_empty", [](nb::kwargs kwargs) { return sogen_windows_emulator(create_empty_emulator(kwargs)); });
    m.def(
        "create_application",
        [](nb::object application, nb::object args, nb::kwargs kwargs) {
            return sogen_windows_emulator(create_application_emulator(application, args, kwargs));
        },
        nb::arg("application"), nb::arg("args") = nb::none(), nb::arg("kwargs"));
}
