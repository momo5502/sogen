#include <nanobind/nanobind.h>

#include "sogen_internal.hpp"

#include <linux_emulator.hpp>

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
        auto callbacks_class = nb::class_<linux_callback_registry>(m, "Callbacks");
        bind_linux_callback_property<&linux_callback_registry::stdout_cb>(callbacks_class, "on_stdout", "stdout");
        bind_linux_callback_property<&linux_callback_registry::stderr_cb>(callbacks_class, "on_stderr", "stderr");
        callbacks_class.def("set", &linux_callback_registry::set).def("clear", &linux_callback_registry::clear);

        nb::class_<linux_thread>(m, "Thread")
            .def_prop_ro("tid", [](const linux_thread& self) { return self.tid; })
            .def_prop_ro("stack_base", [](const linux_thread& self) { return self.stack_base; })
            .def_prop_ro("stack_size", [](const linux_thread& self) { return self.stack_size; })
            .def_prop_ro("fs_base", [](const linux_thread& self) { return self.fs_base; })
            .def_prop_ro("terminated", [](const linux_thread& self) { return self.terminated; })
            .def_prop_ro("exit_code", [](const linux_thread& self) { return self.exit_code; })
            .def_prop_ro("executed_instructions", [](const linux_thread& self) { return self.executed_instructions; });

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
            .def_prop_ro("ppid", &sogen_linux_process_context::ppid)
            .def_prop_ro("uid", &sogen_linux_process_context::uid)
            .def_prop_ro("gid", &sogen_linux_process_context::gid)
            .def_prop_ro("euid", &sogen_linux_process_context::euid)
            .def_prop_ro("egid", &sogen_linux_process_context::egid)
            .def_prop_ro("thread_count", &sogen_linux_process_context::thread_count)
            .def_prop_ro("active_thread", &sogen_linux_process_context::active_thread, nb::rv_policy::reference_internal);

        nb::class_<linux_memory_manager>(m, "MemoryManager")
            .def("read_memory",
                 [](const linux_memory_manager& self, uint64_t address, size_t size) { return read_memory_bytes(self, address, size); })
            .def("write_memory",
                 [](linux_memory_manager& self, uint64_t address, const nb::bytes& buffer) { write_memory_bytes(self, address, buffer); })
            .def("allocate_memory",
                 static_cast<uint64_t (linux_memory_manager::*)(size_t, memory_permission, uint64_t)>(
                     &linux_memory_manager::allocate_memory),
                 nb::arg("size"), nb::arg("permissions"), nb::arg("start") = 0)
            .def("allocate_memory_at",
                 static_cast<bool (linux_memory_manager::*)(uint64_t, size_t, memory_permission)>(&linux_memory_manager::allocate_memory),
                 nb::arg("address"), nb::arg("size"), nb::arg("permissions"))
            .def("protect_memory", &linux_memory_manager::protect_memory)
            .def("release_memory", &linux_memory_manager::release_memory)
            .def("find_free_allocation_base", &linux_memory_manager::find_free_allocation_base, nb::arg("size"), nb::arg("start") = 0)
            .def_prop_rw("mmap_base", &linux_memory_manager::get_mmap_base, &linux_memory_manager::set_mmap_base);

        nb::class_<sogen_linux_emulator>(m, "Emulator")
            .def("start", &sogen_linux_emulator::start, nb::arg("count") = 0, nb::call_guard<nb::gil_scoped_release>())
            .def("stop", &sogen_linux_emulator::stop)
            .def("read_memory", &sogen_linux_emulator::read_memory)
            .def("write_memory", &sogen_linux_emulator::write_memory)
            .def("read_register", &sogen_linux_emulator::read_register)
            .def("write_register", &sogen_linux_emulator::write_register)
            .def_prop_ro("executed_instructions", &sogen_linux_emulator::executed_instructions)
            .def_prop_ro("backend_name", &sogen_linux_emulator::backend_name)
            .def_prop_ro("emulation_root", &sogen_linux_emulator::emulation_root)
            .def_prop_ro("process", &sogen_linux_emulator::process, nb::rv_policy::reference_internal)
            .def_prop_ro("memory", &sogen_linux_emulator::memory, nb::rv_policy::reference_internal)
            .def_prop_ro(
                "callbacks", [](sogen_linux_emulator& self) -> linux_callback_registry& { return *self.callbacks; },
                nb::rv_policy::reference_internal);

        nb::setattr(m, "LinuxEmulator", m.attr("Emulator"));

        m.def("create_application", [](const nb::object& application, const nb::kwargs& kwargs) {
            return sogen_linux_emulator(create_linux_application_emulator(application, nb::none(), kwargs));
        });
        m.def("create_application", [](const nb::object& application, const nb::object& application_args, const nb::kwargs& kwargs) {
            return sogen_linux_emulator(create_linux_application_emulator(application, application_args, kwargs));
        });
    }
}
