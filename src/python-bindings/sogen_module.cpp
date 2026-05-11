#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/stl/filesystem.h>

#include <backend_selection.hpp>
#include <windows_emulator.hpp>
#include <platform/unicode.hpp>
#include <x86_register.hpp>

namespace py = pybind11;

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
    T get_kwarg(const py::kwargs& kwargs, const char* name, T default_value)
    {
        if (!kwargs.contains(name))
        {
            return default_value;
        }

        return kwargs[name].cast<T>();
    }

    std::vector<std::u16string> parse_arguments(py::object object)
    {
        std::vector<std::u16string> result{};

        if (object.is_none())
        {
            return result;
        }

        const py::sequence seq = py::reinterpret_borrow<py::sequence>(object);
        result.reserve(static_cast<size_t>(seq.size()));
        for (const auto& item : seq)
        {
            result.emplace_back(u8_to_u16(py::cast<std::string>(item)));
        }

        return result;
    }

    utils::unordered_insensitive_u16string_map<std::u16string> parse_environment(py::object object)
    {
        utils::unordered_insensitive_u16string_map<std::u16string> result{};

        if (object.is_none())
        {
            return result;
        }

        const py::dict dict = py::reinterpret_borrow<py::dict>(object);
        for (const auto& item : dict)
        {
            result.emplace(u8_to_u16(py::cast<std::string>(item.first)), u8_to_u16(py::cast<std::string>(item.second)));
        }

        return result;
    }

    std::unordered_map<windows_path, std::filesystem::path> parse_path_mappings(py::object object)
    {
        std::unordered_map<windows_path, std::filesystem::path> result{};

        if (object.is_none())
        {
            return result;
        }

        const py::dict dict = py::reinterpret_borrow<py::dict>(object);
        for (const auto& item : dict)
        {
            result.emplace(py::cast<std::filesystem::path>(item.first), py::cast<std::filesystem::path>(item.second));
        }

        return result;
    }

    std::unordered_map<uint16_t, uint16_t> parse_port_mappings(py::object object)
    {
        std::unordered_map<uint16_t, uint16_t> result{};

        if (object.is_none())
        {
            return result;
        }

        const py::dict dict = py::reinterpret_borrow<py::dict>(object);
        for (const auto& item : dict)
        {
            result.emplace(py::cast<uint16_t>(item.first), py::cast<uint16_t>(item.second));
        }

        return result;
    }

    emulator_settings make_emulator_settings(const py::kwargs& kwargs)
    {
        emulator_settings settings{};
        settings.disable_logging = get_kwarg<bool>(kwargs, "disable_logging", false);
        settings.use_relative_time = get_kwarg<bool>(kwargs, "use_relative_time", false);
        settings.emulation_root = get_kwarg<std::filesystem::path>(kwargs, "emulation_root", {});
        settings.registry_directory = get_kwarg<std::filesystem::path>(kwargs, "registry_directory", std::filesystem::path{"./registry"});
        settings.path_mappings = parse_path_mappings(kwargs.contains("path_mappings") ? kwargs["path_mappings"] : py::none());
        settings.port_mappings = parse_port_mappings(kwargs.contains("port_mappings") ? kwargs["port_mappings"] : py::none());
        settings.fake_env.number_of_processors = get_kwarg<uint32_t>(kwargs, "number_of_processors", 4);
        settings.fake_env.nt_product_type = static_cast<uint8_t>(get_kwarg<uint32_t>(kwargs, "nt_product_type", 1));
        return settings;
    }

    application_settings make_application_settings(py::object application, py::object args, const py::kwargs& kwargs)
    {
        application_settings settings{};
        settings.application = py::cast<std::filesystem::path>(application);
        settings.working_directory = get_kwarg<std::filesystem::path>(kwargs, "working_directory", {});
        settings.arguments = parse_arguments(args);
        settings.environment = parse_environment(kwargs.contains("environment") ? kwargs["environment"] : py::none());
        return settings;
    }

    std::unique_ptr<windows_emulator> create_empty_emulator(const py::kwargs& kwargs)
    {
        return std::make_unique<windows_emulator>(create_x86_64_emulator(), make_emulator_settings(kwargs));
    }

    std::unique_ptr<windows_emulator> create_application_emulator(py::object application, py::object args, const py::kwargs& kwargs)
    {
        auto app_settings = make_application_settings(application, args, kwargs);
        return std::make_unique<windows_emulator>(create_x86_64_emulator(), std::move(app_settings), make_emulator_settings(kwargs));
    }

    template <typename ByteContainer>
    py::bytes make_py_bytes(const ByteContainer& data)
    {
        return py::bytes(reinterpret_cast<const char*>(data.data()), static_cast<py::ssize_t>(data.size()));
    }

    py::bytes read_memory_bytes(const memory_manager& memory, const uint64_t address, const size_t size)
    {
        const auto data = static_cast<const memory_interface&>(memory).read_memory(address, size);
        return make_py_bytes(data);
    }

    void write_memory_bytes(memory_manager& memory, const uint64_t address, py::buffer buffer)
    {
        const py::buffer_info info = buffer.request();
        memory.write_memory(address, info.ptr, static_cast<size_t>(info.size) * static_cast<size_t>(info.itemsize));
    }

    uint64_t read_register_value(windows_emulator& self, const x86_register reg)
    {
        return self.emu().reg<uint64_t>(reg);
    }

    void write_register_value(windows_emulator& self, const x86_register reg, const uint64_t value)
    {
        self.emu().reg<uint64_t>(reg, value);
    }

    template <typename Fn>
    void set_py_callback(Fn& slot, py::object callable)
    {
        if (callable.is_none())
        {
            slot = {};
            return;
        }

        py::function fn = py::reinterpret_borrow<py::function>(callable);
        slot = [fn = std::move(fn)](auto&&... args) {
            py::gil_scoped_acquire gil{};
            fn(std::forward<decltype(args)>(args)...);
        };
    }
}

PYBIND11_MODULE(sogen, m)
{
    m.doc() = "Sogen Python bindings";

    py::enum_<memory_permission>(m, "MemoryPermission")
        .value("none", memory_permission::none)
        .value("read", memory_permission::read)
        .value("write", memory_permission::write)
        .value("exec", memory_permission::exec)
        .value("read_write", memory_permission::read_write)
        .value("all", memory_permission::all)
        .export_values();

    py::enum_<memory_region_kind>(m, "MemoryRegionKind")
        .value("free", memory_region_kind::free)
        .value("private_allocation", memory_region_kind::private_allocation)
        .value("section_view", memory_region_kind::section_view)
        .value("section_image", memory_region_kind::section_image)
        .value("mmio", memory_region_kind::mmio)
        .export_values();

    py::enum_<x86_register>(m, "Register")
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
        .export_values();

    py::class_<memory_stats>(m, "MemoryStats")
        .def_readonly("reserved_memory", &memory_stats::reserved_memory)
        .def_readonly("committed_memory", &memory_stats::committed_memory);

    py::class_<handle>(m, "Handle")
        .def(py::init<>())
        .def_property(
            "bits", [](const handle& self) { return self.bits; }, [](handle& self, uint64_t value) { self.bits = value; })
        .def_property_readonly("id", [](const handle& self) { return self.value.id; })
        .def_property_readonly("type", [](const handle& self) { return self.value.type; })
        .def_property_readonly("is_system", [](const handle& self) { return self.value.is_system != 0; })
        .def_property_readonly("is_pseudo", [](const handle& self) { return self.value.is_pseudo != 0; })
        .def_property_readonly("high_bits", [](const handle& self) { return self.value.high_bits; });

    py::class_<region_info>(m, "MemoryRegionInfo")
        .def_readonly("start", &region_info::start)
        .def_readonly("length", &region_info::length)
        .def_readonly("permissions", &region_info::permissions)
        .def_readonly("allocation_base", &region_info::allocation_base)
        .def_readonly("allocation_length", &region_info::allocation_length)
        .def_readonly("is_reserved", &region_info::is_reserved)
        .def_readonly("is_committed", &region_info::is_committed)
        .def_readonly("initial_permissions", &region_info::initial_permissions)
        .def_readonly("kind", &region_info::kind);

    py::class_<memory_manager>(m, "MemoryManager")
        .def(
            "read_memory", [](const memory_manager& self, uint64_t address, size_t size) { return read_memory_bytes(self, address, size); },
            py::arg("address"), py::arg("size"))
        .def(
            "write_memory",
            [](memory_manager& self, uint64_t address, py::buffer buffer) { write_memory_bytes(self, address, std::move(buffer)); },
            py::arg("address"), py::arg("buffer"))
        .def(
            "allocate_memory",
            [](memory_manager& self, size_t size, memory_permission permissions, bool reserve_only, uint64_t start,
               memory_region_kind kind) { return self.allocate_memory(size, permissions, reserve_only, start, kind); },
            py::arg("size"), py::arg("permissions"), py::arg("reserve_only") = false, py::arg("start") = 0,
            py::arg("kind") = memory_region_kind::private_allocation)
        .def(
            "protect_memory",
            [](memory_manager& self, uint64_t address, size_t size, memory_permission permissions) {
                return self.protect_memory(address, size, nt_memory_permission{permissions});
            },
            py::arg("address"), py::arg("size"), py::arg("permissions"))
        .def(
            "commit_memory",
            [](memory_manager& self, uint64_t address, size_t size, memory_permission permissions) {
                return self.commit_memory(address, size, nt_memory_permission{permissions});
            },
            py::arg("address"), py::arg("size"), py::arg("permissions"))
        .def("decommit_memory", &memory_manager::decommit_memory, py::arg("address"), py::arg("size"))
        .def("release_memory", &memory_manager::release_memory, py::arg("address"), py::arg("size"))
        .def("find_free_allocation_base", &memory_manager::find_free_allocation_base, py::arg("size"), py::arg("start") = 0)
        .def("get_region_info", &memory_manager::get_region_info, py::arg("address"))
        .def("compute_memory_stats", &memory_manager::compute_memory_stats)
        .def_property("default_allocation_address", &memory_manager::get_default_allocation_address,
                      &memory_manager::set_default_allocation_address);

    py::class_<emulator_thread>(m, "Thread")
        .def_property_readonly("id", [](const emulator_thread& self) { return self.id; })
        .def_property_readonly("name", [](const emulator_thread& self) { return u16_to_u8(self.name); })
        .def_property_readonly("start_address", [](const emulator_thread& self) { return self.start_address; })
        .def_property_readonly("argument", [](const emulator_thread& self) { return self.argument; })
        .def_property_readonly("executed_instructions", [](const emulator_thread& self) { return self.executed_instructions; })
        .def_property_readonly("current_ip", [](const emulator_thread& self) { return self.current_ip; })
        .def_property_readonly("previous_ip", [](const emulator_thread& self) { return self.previous_ip; })
        .def_property_readonly("setup_done", [](const emulator_thread& self) { return self.setup_done; })
        .def_property_readonly("exit_status", [](const emulator_thread& self) -> py::object {
            if (!self.exit_status.has_value())
            {
                return py::none();
            }

            return py::int_(*self.exit_status);
        });

    py::class_<process_context>(m, "ProcessContext")
        .def_property_readonly("is_wow64_process", [](const process_context& self) { return self.is_wow64_process; })
        .def_property_readonly("exit_status",
                               [](const process_context& self) -> py::object {
                                   if (!self.exit_status.has_value())
                                   {
                                       return py::none();
                                   }

                                   return py::int_(*self.exit_status);
                               })
        .def_property_readonly("live_thread_count", &process_context::get_live_thread_count)
        .def_property_readonly("spawned_thread_count", [](const process_context& self) { return self.spawned_thread_count; })
        .def_property_readonly("active_thread", [](process_context& self) -> py::object {
            if (!self.active_thread)
            {
                return py::none();
            }

            return py::cast(self.active_thread, py::return_value_policy::reference_internal);
        });

    py::class_<windows_emulator, std::unique_ptr<windows_emulator>>(m, "WindowsEmulator")
        .def("start", &windows_emulator::start, py::arg("count") = 0, py::call_guard<py::gil_scoped_release>())
        .def("run", &windows_emulator::start, py::arg("count") = 0, py::call_guard<py::gil_scoped_release>())
        .def("stop", &windows_emulator::stop)
        .def("setup_process_if_necessary", &windows_emulator::setup_process_if_necessary, py::call_guard<py::gil_scoped_release>())
        .def("yield_thread", &windows_emulator::yield_thread, py::arg("alertable") = false, py::call_guard<py::gil_scoped_release>())
        .def("perform_thread_switch", &windows_emulator::perform_thread_switch, py::call_guard<py::gil_scoped_release>())
        .def("activate_thread", &windows_emulator::activate_thread, py::arg("id"))
        .def("save_snapshot", &windows_emulator::save_snapshot)
        .def("restore_snapshot", &windows_emulator::restore_snapshot)
        .def_property_readonly("executed_instructions", &windows_emulator::get_executed_instructions)
        .def_property_readonly("last_stop_reason",
                               [](const windows_emulator& self) { return stop_reason_to_string(self.last_stop_reason()); })
        .def_property_readonly("last_stop_reason_code",
                               [](const windows_emulator& self) { return static_cast<int>(self.last_stop_reason()); })
        .def_property_readonly("last_stop_detail", &windows_emulator::last_stop_detail)
        .def_property_readonly("backend_name", [](const windows_emulator& self) { return self.emu().get_name(); })
        .def_property_readonly("emulation_root", [](const windows_emulator& self) { return self.emulation_root; })
        .def_property_readonly("current_thread_id",
                               [](const windows_emulator& self) -> py::object {
                                   if (!self.process.active_thread)
                                   {
                                       return py::none();
                                   }

                                   return py::int_(self.process.active_thread->id);
                               })
        .def_property_readonly("current_thread",
                               [](windows_emulator& self) -> py::object {
                                   if (!self.process.active_thread)
                                   {
                                       return py::none();
                                   }

                                   return py::cast(self.process.active_thread, py::return_value_policy::reference_internal);
                               })
        .def_property_readonly(
            "process", [](windows_emulator& self) -> process_context& { return self.process; }, py::return_value_policy::reference_internal)
        .def_property_readonly(
            "memory", [](windows_emulator& self) -> memory_manager& { return self.memory; }, py::return_value_policy::reference_internal)
        .def(
            "read_memory",
            [](const windows_emulator& self, uint64_t address, size_t size) { return read_memory_bytes(self.memory, address, size); },
            py::arg("address"), py::arg("size"))
        .def(
            "write_memory",
            [](windows_emulator& self, uint64_t address, py::buffer buffer) {
                write_memory_bytes(self.memory, address, std::move(buffer));
            },
            py::arg("address"), py::arg("buffer"))
        .def(
            "read_register", [](windows_emulator& self, x86_register reg) { return py::int_(read_register_value(self, reg)); },
            py::arg("reg"))
        .def(
            "write_register", [](windows_emulator& self, x86_register reg, uint64_t value) { write_register_value(self, reg, value); },
            py::arg("reg"), py::arg("value"))
        .def("get_host_port", &windows_emulator::get_host_port, py::arg("emulator_port"))
        .def("get_emulator_port", &windows_emulator::get_emulator_port, py::arg("host_port"))
        .def("map_port", &windows_emulator::map_port, py::arg("emulator_port"), py::arg("host_port"))
        .def("set_callback", [](windows_emulator& self, const std::string& name, py::object callable) {
            if (name == "stdout")
            {
                set_py_callback(self.callbacks.on_stdout, callable);
                return;
            }
            if (name == "syscall")
            {
                if (callable.is_none())
                {
                    self.callbacks.on_syscall = {};
                    return;
                }

                py::function fn = py::reinterpret_borrow<py::function>(callable);
                self.callbacks.on_syscall = [fn = std::move(fn)](uint32_t syscall_id, std::string_view syscall_name) {
                    py::gil_scoped_acquire gil{};
                    const auto result = fn(syscall_id, std::string(syscall_name));
                    return result.cast<bool>() ? instruction_hook_continuation::skip_instruction
                                               : instruction_hook_continuation::run_instruction;
                };
                return;
            }
            if (name == "generic_activity")
            {
                set_py_callback(self.callbacks.on_generic_activity, callable);
                return;
            }
            if (name == "suspicious_activity")
            {
                set_py_callback(self.callbacks.on_suspicious_activity, callable);
                return;
            }
            if (name == "instruction")
            {
                if (callable.is_none())
                {
                    self.callbacks.on_instruction = {};
                    return;
                }

                py::function fn = py::reinterpret_borrow<py::function>(callable);
                self.callbacks.on_instruction = [fn = std::move(fn)](uint64_t address) {
                    py::gil_scoped_acquire gil{};
                    fn(address);
                };
                return;
            }
            if (name == "exception")
            {
                if (callable.is_none())
                {
                    self.callbacks.on_exception = {};
                    return;
                }

                py::function fn = py::reinterpret_borrow<py::function>(callable);
                self.callbacks.on_exception = [fn = std::move(fn)] {
                    py::gil_scoped_acquire gil{};
                    fn();
                };
                return;
            }
            if (name == "memory_protect")
            {
                if (callable.is_none())
                {
                    self.callbacks.on_memory_protect = {};
                    return;
                }

                py::function fn = py::reinterpret_borrow<py::function>(callable);
                self.callbacks.on_memory_protect = [fn = std::move(fn)](uint64_t address, uint64_t length, memory_permission permission) {
                    py::gil_scoped_acquire gil{};
                    fn(address, length, static_cast<int>(permission));
                };
                return;
            }
            if (name == "memory_allocate")
            {
                if (callable.is_none())
                {
                    self.callbacks.on_memory_allocate = {};
                    return;
                }

                py::function fn = py::reinterpret_borrow<py::function>(callable);
                self.callbacks.on_memory_allocate = [fn = std::move(fn)](uint64_t address, uint64_t length, memory_permission permission,
                                                                         bool commit) {
                    py::gil_scoped_acquire gil{};
                    fn(address, length, static_cast<int>(permission), commit);
                };
                return;
            }
            if (name == "memory_violate")
            {
                if (callable.is_none())
                {
                    self.callbacks.on_memory_violate = {};
                    return;
                }

                py::function fn = py::reinterpret_borrow<py::function>(callable);
                self.callbacks.on_memory_violate = [fn = std::move(fn)](uint64_t address, uint64_t length, memory_operation operation,
                                                                        memory_violation_type type) {
                    py::gil_scoped_acquire gil{};
                    fn(address, length, static_cast<int>(operation), static_cast<int>(type));
                };
                return;
            }
            if (name == "rdtsc")
            {
                set_py_callback(self.callbacks.on_rdtsc, callable);
                return;
            }
            if (name == "rdtscp")
            {
                set_py_callback(self.callbacks.on_rdtscp, callable);
                return;
            }
            if (name == "ioctrl")
            {
                if (callable.is_none())
                {
                    self.callbacks.on_ioctrl = {};
                    return;
                }

                py::function fn = py::reinterpret_borrow<py::function>(callable);
                self.callbacks.on_ioctrl = [fn = std::move(fn)](io_device&, std::u16string_view device_name, ULONG code) {
                    py::gil_scoped_acquire gil{};
                    fn(u16_to_u8(device_name), static_cast<uint32_t>(code));
                };
                return;
            }
            if (name == "thread_create")
            {
                if (callable.is_none())
                {
                    self.process.callbacks_->on_thread_create = {};
                    return;
                }

                py::function fn = py::reinterpret_borrow<py::function>(callable);
                self.process.callbacks_->on_thread_create = [fn = std::move(fn)](handle h, emulator_thread& thr) {
                    py::gil_scoped_acquire gil{};
                    fn(h.bits, thr.id, thr.start_address, thr.argument);
                };
                return;
            }
            if (name == "thread_terminated")
            {
                if (callable.is_none())
                {
                    self.process.callbacks_->on_thread_terminated = {};
                    return;
                }

                py::function fn = py::reinterpret_borrow<py::function>(callable);
                self.process.callbacks_->on_thread_terminated = [fn = std::move(fn)](handle h, emulator_thread& thr) {
                    py::gil_scoped_acquire gil{};
                    fn(h.bits, thr.id);
                };
                return;
            }
            if (name == "thread_set_name")
            {
                if (callable.is_none())
                {
                    self.process.callbacks_->on_thread_set_name = {};
                    return;
                }

                py::function fn = py::reinterpret_borrow<py::function>(callable);
                self.process.callbacks_->on_thread_set_name = [fn = std::move(fn)](emulator_thread& thr) {
                    py::gil_scoped_acquire gil{};
                    fn(thr.id, u16_to_u8(thr.name));
                };
                return;
            }
            if (name == "thread_switch")
            {
                if (callable.is_none())
                {
                    self.process.callbacks_->on_thread_switch = {};
                    return;
                }

                py::function fn = py::reinterpret_borrow<py::function>(callable);
                self.process.callbacks_->on_thread_switch = [fn = std::move(fn)](emulator_thread& current_thread,
                                                                                 emulator_thread& new_thread) {
                    py::gil_scoped_acquire gil{};
                    fn(current_thread.id, new_thread.id);
                };
                return;
            }

            throw std::runtime_error("unknown callback name");
        });

    m.def("create_empty", [](py::kwargs kwargs) { return create_empty_emulator(kwargs); }, "Create an empty emulator session.");
    m.def(
        "create_application",
        [](py::object application, py::object args, py::kwargs kwargs) { return create_application_emulator(application, args, kwargs); },
        py::arg("application"), py::arg("args") = py::none(), "Create an emulator session for an application.");
}
