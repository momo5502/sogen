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

    struct callback_registry
    {
        windows_emulator* emu{};
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
            this->emu->callbacks.on_stdout = [this](std::string_view data) { invoke_callback(this->stdout_cb, std::string(data)); };
            this->emu->callbacks.on_syscall = [this](const uint32_t syscall_id, const std::string_view syscall_name) {
                return invoke_bool_callback(this->syscall_cb, syscall_id, std::string(syscall_name))
                           ? instruction_hook_continuation::skip_instruction
                           : instruction_hook_continuation::run_instruction;
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
                invoke_callback(this->memory_violate_cb, address, length, operation, type);
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
            if (callable.is_valid() && !callable.is_none() && !PyCallable_Check(callable.ptr()))
            {
                throw std::runtime_error("callback must be callable or None");
            }

            const auto assign = [&](nb::object& slot) { slot = std::move(callable); };

            if (name == "stdout")
            {
                assign(this->stdout_cb);
            }
            else if (name == "syscall")
            {
                assign(this->syscall_cb);
            }
            else if (name == "generic_access")
            {
                assign(this->generic_access_cb);
            }
            else if (name == "generic_activity")
            {
                assign(this->generic_activity_cb);
            }
            else if (name == "suspicious_activity")
            {
                assign(this->suspicious_activity_cb);
            }
            else if (name == "exception")
            {
                assign(this->exception_cb);
            }
            else if (name == "instruction")
            {
                assign(this->instruction_cb);
            }
            else if (name == "memory_protect")
            {
                assign(this->memory_protect_cb);
            }
            else if (name == "memory_allocate")
            {
                assign(this->memory_allocate_cb);
            }
            else if (name == "memory_violate")
            {
                assign(this->memory_violate_cb);
            }
            else if (name == "rdtsc")
            {
                assign(this->rdtsc_cb);
            }
            else if (name == "rdtscp")
            {
                assign(this->rdtscp_cb);
            }
            else if (name == "ioctrl")
            {
                assign(this->ioctrl_cb);
            }
            else if (name == "debug_string")
            {
                assign(this->debug_string_cb);
            }
            else if (name == "thread_create")
            {
                assign(this->thread_create_cb);
            }
            else if (name == "thread_terminated")
            {
                assign(this->thread_terminated_cb);
            }
            else if (name == "thread_set_name")
            {
                assign(this->thread_set_name_cb);
            }
            else if (name == "thread_switch")
            {
                assign(this->thread_switch_cb);
            }
            else
            {
                throw std::runtime_error("Unknown callback name: " + std::string(name));
            }
        }

        void clear(const std::string_view name)
        {
            set(name, nb::none());
        }
    };

    struct sogen_windows_emulator
    {
        std::unique_ptr<windows_emulator> emu{};
        std::shared_ptr<callback_registry> callbacks{};

        explicit sogen_windows_emulator(std::unique_ptr<windows_emulator> emulator)
            : emu(std::move(emulator)),
              callbacks(std::make_shared<callback_registry>(*this->emu))
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

        nb::object process()
        {
            return nb::cast(&this->emu->process, nb::rv_policy::reference_internal);
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
        .value("section_view", memory_region_kind::section_view)
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
        .def("find_free_allocation_base", &memory_manager::find_free_allocation_base)
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

    nb::class_<process_context>(m, "ProcessContext")
        .def_prop_ro("is_wow64_process", [](const process_context& self) { return self.is_wow64_process; })
        .def_prop_ro("exit_status",
                     [](const process_context& self) -> nb::object {
                         if (!self.exit_status.has_value())
                         {
                             return nb::none();
                         }

                         return nb::int_(*self.exit_status);
                     })
        .def_prop_ro("live_thread_count", &process_context::get_live_thread_count)
        .def_prop_ro("spawned_thread_count", [](const process_context& self) { return self.spawned_thread_count; })
        .def_prop_ro("active_thread", [](process_context& self) -> nb::object {
            if (!self.active_thread)
            {
                return nb::none();
            }

            return nb::cast(self.active_thread, nb::rv_policy::reference_internal);
        });

    nb::class_<callback_registry>(m, "Callbacks")
        .def("set", [](callback_registry& self, const std::string& name, nb::object callback) { self.set(name, std::move(callback)); })
        .def("clear", [](callback_registry& self, const std::string& name) { self.clear(name); });

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
