#include "sogen_linux_internal.hpp"

namespace sogen::py
{
    namespace
    {
        template <auto Member>
        void bind_linux_callback_property(nb::class_<linux_callback_registry>& c, const char* name, const char* slot_name)
        {
            c.def_prop_rw(
                name, [](linux_callback_registry& self) { return self.*Member; },
                [slot_name](linux_callback_registry& self, nb::object callback) { self.set(slot_name, std::move(callback)); },
                nb::arg("callback").none());
        }
    }

    void register_linux_runtime_bindings(nb::module_& m)
    {
        nb::enum_<linux_syscall_continuation>(m, "SyscallContinuation")
            .value("run_handler", linux_syscall_continuation::run_handler)
            .value("skip_handler", linux_syscall_continuation::skip_handler)
            .value("stop", linux_syscall_continuation::stop)
            .export_values();

        nb::class_<linux_syscall_info>(m, "SyscallInfo")
            .def_prop_ro("number", [](const linux_syscall_info& self) { return self.number; })
            .def_prop_ro("name", [](const linux_syscall_info& self) { return std::string(self.name); })
            .def_prop_ro("args", [](const linux_syscall_info& self) {
                std::vector<uint64_t> args(self.args.begin(), self.args.end());
                return args;
            })
            .def("arg", &linux_syscall_info::arg, nb::arg("index"))
            .def("read_c_string", &linux_syscall_info::read_c_string, nb::arg("index"), nb::arg("max_len") = 4096);

        nb::class_<linux_exported_symbol>(m, "ExportedSymbol")
            .def_prop_ro("name", [](const linux_exported_symbol& self) { return self.name; })
            .def_prop_ro("rva", [](const linux_exported_symbol& self) { return self.rva; })
            .def_prop_ro("address", [](const linux_exported_symbol& self) { return self.address; });

        nb::class_<linux_mapped_section>(m, "MappedSection")
            .def_prop_ro("name", [](const linux_mapped_section& self) { return self.name; })
            .def_prop_ro("start", [](const linux_mapped_section& self) { return self.start; })
            .def_prop_ro("length", [](const linux_mapped_section& self) { return self.length; })
            .def_prop_ro("permissions", [](const linux_mapped_section& self) { return self.permissions; });

        nb::class_<linux_mapped_module>(m, "MappedModule")
            .def_prop_ro("name", [](const linux_mapped_module& self) { return self.name; })
            .def_prop_ro("path", [](const linux_mapped_module& self) { return self.path; })
            .def_prop_ro("image_base", [](const linux_mapped_module& self) { return self.image_base; })
            .def_prop_ro("size_of_image", [](const linux_mapped_module& self) { return self.size_of_image; })
            .def_prop_ro("entry_point", [](const linux_mapped_module& self) { return self.entry_point; })
            .def_prop_ro("exports", [](const linux_mapped_module& self) { return self.exports; })
            .def_prop_ro("needed_libraries", [](const linux_mapped_module& self) { return self.needed_libraries; })
            .def_prop_ro("sections", [](const linux_mapped_module& self) { return self.sections; });

        nb::class_<linux_hook_handle>(m, "Hook")
            .def("remove", &linux_hook_handle::remove)
            .def_prop_ro("active", &linux_hook_handle::active);

        nb::class_<linux_syscall_hook_registry>(m, "SyscallHooks")
            .def("__setitem__", &linux_syscall_hook_registry::set_item)
            .def("__delitem__", &linux_syscall_hook_registry::del_item)
            .def("clear", &linux_syscall_hook_registry::clear);

        nb::class_<linux_hook_registry>(m, "Hooks")
            .def("memory_execution", &linux_hook_registry::memory_execution)
            .def("memory_execution_at", &linux_hook_registry::memory_execution_at)
            .def("memory_read", &linux_hook_registry::memory_read)
            .def("memory_write", &linux_hook_registry::memory_write)
            .def("instruction", &linux_hook_registry::instruction)
            .def("interrupt", &linux_hook_registry::interrupt)
            .def("memory_violation", &linux_hook_registry::memory_violation)
            .def("basic_block", &linux_hook_registry::basic_block)
            .def_prop_ro("syscalls", [](linux_hook_registry& self) -> linux_syscall_hook_registry& { return *self.syscalls; },
                         nb::rv_policy::reference_internal);

        nb::class_<linux_memory_manager>(m, "MemoryManager")
            .def("read_memory",
                 [](const linux_memory_manager& self, uint64_t address, size_t size) { return read_memory_bytes(self, address, size); })
            .def("write_memory",
                 [](linux_memory_manager& self, uint64_t address, const nb::bytes& buffer) { write_memory_bytes(self, address, buffer); })
            .def(
                "allocate_memory",
                [](linux_memory_manager& self, size_t size, memory_permission permissions, uint64_t start) {
                    return self.allocate_memory(size, permissions, start);
                },
                nb::arg("size"), nb::arg("permissions"), nb::arg("start") = 0)
            .def("protect_memory", &linux_memory_manager::protect_memory)
            .def("release_memory", &linux_memory_manager::release_memory)
            .def("find_free_allocation_base",
                 static_cast<uint64_t (linux_memory_manager::*)(size_t, uint64_t) const>(&linux_memory_manager::find_free_allocation_base))
            .def_prop_rw("mmap_base", &linux_memory_manager::get_mmap_base, &linux_memory_manager::set_mmap_base);

        nb::class_<linux_thread>(m, "Thread")
            .def_prop_ro("id", [](const linux_thread& self) { return self.tid; })
            .def_prop_ro("stack_base", [](const linux_thread& self) { return self.stack_base; })
            .def_prop_ro("stack_size", [](const linux_thread& self) { return self.stack_size; })
            .def_prop_ro("executed_instructions", [](const linux_thread& self) { return self.executed_instructions; });

        auto callbacks_class = nb::class_<linux_callback_registry>(m, "Callbacks");
        bind_linux_callback_property<&linux_callback_registry::stdout_cb>(callbacks_class, "on_stdout", "stdout");
        bind_linux_callback_property<&linux_callback_registry::stderr_cb>(callbacks_class, "on_stderr", "stderr");
        bind_linux_callback_property<&linux_callback_registry::syscall_cb>(callbacks_class, "on_syscall", "syscall");
        bind_linux_callback_property<&linux_callback_registry::module_load_cb>(callbacks_class, "on_module_load", "module_load");
        bind_linux_callback_property<&linux_callback_registry::instruction_cb>(callbacks_class, "on_instruction", "instruction");
        bind_linux_callback_property<&linux_callback_registry::memory_violate_cb>(callbacks_class, "on_memory_violate", "memory_violate");

        nb::class_<sogen_linux_process_context>(m, "ProcessContext")
            .def_prop_ro("exit_status",
                         [](const sogen_linux_process_context& self) -> nb::object {
                             if (!self.exit_status().has_value())
                             {
                                 return nb::none();
                             }

                             return nb::int_(*self.exit_status());
                         })
            .def_prop_ro("pid", &sogen_linux_process_context::pid)
            .def_prop_ro("active_thread", &sogen_linux_process_context::active_thread, nb::rv_policy::reference_internal);

        nb::class_<sogen_linux_emulator>(m, "Emulator")
            .def("start", &sogen_linux_emulator::start, nb::arg("count") = 0, nb::call_guard<nb::gil_scoped_release>())
            .def("stop", &sogen_linux_emulator::stop)
            .def_prop_ro("executed_instructions",
                         [](const sogen_linux_emulator& self) { return self.native().get_executed_instructions(); })
            .def_prop_ro("backend_name", [](const sogen_linux_emulator& self) { return self.native().emu().get_name(); })
            .def_prop_ro("emulation_root", [](const sogen_linux_emulator& self) { return self.native().emulation_root; })
            .def_prop_ro("process", &sogen_linux_emulator::process, nb::rv_policy::reference_internal)
            .def_prop_ro("memory", &sogen_linux_emulator::memory, nb::rv_policy::reference_internal)
            .def_prop_ro("current_thread", &sogen_linux_emulator::current_thread, nb::rv_policy::reference_internal)
            .def_prop_ro("current_thread_id",
                         [](const sogen_linux_emulator& self) -> nb::object {
                             const auto id = self.current_thread_id();
                             if (!id.has_value())
                             {
                                 return nb::none();
                             }

                             return nb::int_(*id);
                         })
            .def_prop_ro(
                "callbacks", [](sogen_linux_emulator& self) -> linux_callback_registry& { return *self.callbacks; },
                nb::rv_policy::reference_internal)
            .def_prop_ro(
                "hooks", [](sogen_linux_emulator& self) -> linux_hook_registry& { return *self.hooks; }, nb::rv_policy::reference_internal)
            .def("read_memory", &sogen_linux_emulator::read_memory)
            .def("write_memory", &sogen_linux_emulator::write_memory)
            .def("read_register", &sogen_linux_emulator::read_register)
            .def("write_register", &sogen_linux_emulator::write_register)
            .def("get_host_port", &sogen_linux_emulator::get_host_port)
            .def("get_emulator_port", &sogen_linux_emulator::get_emulator_port)
            .def("map_port", &sogen_linux_emulator::map_port);

        nb::setattr(m, "LinuxEmulator", m.attr("Emulator"));

        m.def("create_application", [](nb::object application, nb::kwargs kwargs) { // NOLINT(performance-unnecessary-value-param)
            return sogen_linux_emulator(create_linux_application_emulator(application, nb::none(), kwargs));
        });
        m.def("create_application",
              [](nb::object application, nb::object application_args, nb::kwargs kwargs) { // NOLINT(performance-unnecessary-value-param)
                  return sogen_linux_emulator(create_linux_application_emulator(application, application_args, kwargs));
              });
    }
}
