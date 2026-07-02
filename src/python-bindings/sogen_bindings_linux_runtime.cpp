#include <nanobind/nanobind.h>

#include "sogen_internal.hpp"

#include <linux_emulator.hpp>
#include <memory_manager.hpp>

#include <utility>

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
        m.def(
            "symbol_call",
            [](nb::object params, nb::object restype) {
                return nb::cpp_function([params = std::move(params), restype = std::move(restype)](nb::handle fn) -> nb::object {
                    nb::setattr(fn, "_sogen_linux_symbol_params", params);
                    nb::setattr(fn, "_sogen_linux_symbol_restype", restype);
                    return nb::borrow<nb::object>(fn);
                });
            },
            nb::arg("params") = nb::none(), nb::arg("restype") = nb::none());

        nb::class_<linux_symbol_hook_registry>(m, "SymbolHooks")
            .def("__setitem__", &linux_symbol_hook_registry::set_item, nb::arg("key"), nb::arg("callback").none())
            .def("__delitem__", &linux_symbol_hook_registry::del_item)
            .def("clear", &linux_symbol_hook_registry::clear);

        nb::class_<linux_exported_symbol>(m, "ExportedSymbol")
            .def_prop_ro("name", [](const linux_exported_symbol& self) { return self.name; })
            .def_prop_ro("rva", [](const linux_exported_symbol& self) { return self.rva; })
            .def_prop_ro("address", [](const linux_exported_symbol& self) { return self.address; });

        nb::class_<linux_mapped_section>(m, "MappedSection")
            .def_prop_ro("name", [](const linux_mapped_section& self) { return self.name; })
            .def_prop_ro("start", [](const linux_mapped_section& self) { return self.start; })
            .def_prop_ro("length", [](const linux_mapped_section& self) { return self.length; })
            .def_prop_ro("permissions", [](const linux_mapped_section& self) { return self.permissions; });

        nb::class_<linux_mapped_module>(m, "LinuxMappedModule")
            .def_prop_ro("name", [](const linux_mapped_module& self) { return self.name; })
            .def_prop_ro("path", [](const linux_mapped_module& self) { return self.path; })
            .def_prop_ro("image_base", [](const linux_mapped_module& self) { return self.image_base; })
            .def_prop_ro("size_of_image", [](const linux_mapped_module& self) { return self.size_of_image; })
            .def_prop_ro("entry_point", [](const linux_mapped_module& self) { return self.entry_point; })
            .def_prop_ro(
                "exports", [](const linux_mapped_module& self) -> const std::vector<linux_exported_symbol>& { return self.exports; },
                nb::rv_policy::reference_internal)
            .def_prop_ro(
                "needed_libraries",
                [](const linux_mapped_module& self) -> const std::vector<std::string>& { return self.needed_libraries; },
                nb::rv_policy::reference_internal)
            .def_prop_ro(
                "sections", [](const linux_mapped_module& self) -> const std::vector<linux_mapped_section>& { return self.sections; },
                nb::rv_policy::reference_internal)
            .def_prop_ro("rpath", [](const linux_mapped_module& self) { return self.rpath; })
            .def_prop_ro("runpath", [](const linux_mapped_module& self) { return self.runpath; });

        nb::class_<linux_symbol_call_info>(m, "LinuxSymbolCall")
            .def_prop_ro(
                "module", [](const linux_symbol_call_info& self) -> linux_mapped_module& { return *self.module; },
                nb::rv_policy::reference_internal)
            .def_prop_ro("name", [](const linux_symbol_call_info& self) { return self.name; })
            .def_prop_ro("address", [](const linux_symbol_call_info& self) { return self.address; })
            .def_prop_ro("return_address", [](const linux_symbol_call_info& self) { return self.return_address; })
            .def_prop_rw(
                "return_value", [](const linux_symbol_call_info& self) { return self.return_value; },
                [](linux_symbol_call_info& self, uint64_t value) { self.return_value = value; });

        nb::setattr(m, "Hook", nb::module_::import_("sogen.windows").attr("Hook"));

        nb::class_<linux_hook_registry>(m, "Hooks")
            .def("memory_execution", &linux_hook_registry::memory_execution)
            .def("memory_execution_at", &linux_hook_registry::memory_execution_at)
            .def("memory_read", &linux_hook_registry::memory_read)
            .def("memory_write", &linux_hook_registry::memory_write)
            .def("instruction", &linux_hook_registry::instruction)
            .def("interrupt", &linux_hook_registry::interrupt)
            .def("memory_violation", &linux_hook_registry::memory_violation)
            .def("basic_block", &linux_hook_registry::basic_block)
            .def_prop_ro(
                "symbols", [](linux_hook_registry& self) -> linux_symbol_hook_registry& { return *self.symbols; },
                nb::rv_policy::reference_internal);

        auto callbacks_class = nb::class_<linux_callback_registry>(m, "Callbacks");
        bind_linux_callback_property<&linux_callback_registry::stdout_cb>(callbacks_class, "on_stdout", "stdout");
        bind_linux_callback_property<&linux_callback_registry::stderr_cb>(callbacks_class, "on_stderr", "stderr");
        bind_linux_callback_property<&linux_callback_registry::syscall_cb>(callbacks_class, "on_syscall", "syscall");
        bind_linux_callback_property<&linux_callback_registry::memory_violate_cb>(callbacks_class, "on_memory_violate", "memory_violate");
        bind_linux_callback_property<&linux_callback_registry::signal_cb>(callbacks_class, "on_signal", "signal");
        bind_linux_callback_property<&linux_callback_registry::signal_cb>(callbacks_class, "on_exception", "exception");
        bind_linux_callback_property<&linux_callback_registry::memory_allocate_cb>(callbacks_class, "on_memory_allocate",
                                                                                   "memory_allocate");
        bind_linux_callback_property<&linux_callback_registry::memory_protect_cb>(callbacks_class, "on_memory_protect", "memory_protect");
        bind_linux_callback_property<&linux_callback_registry::memory_release_cb>(callbacks_class, "on_memory_release", "memory_release");
        bind_linux_callback_property<&linux_callback_registry::module_load_cb>(callbacks_class, "on_module_load", "module_load");
        bind_linux_callback_property<&linux_callback_registry::thread_create_cb>(callbacks_class, "on_thread_create", "thread_create");
        bind_linux_callback_property<&linux_callback_registry::thread_terminated_cb>(callbacks_class, "on_thread_terminated",
                                                                                     "thread_terminated");
        bind_linux_callback_property<&linux_callback_registry::thread_switch_cb>(callbacks_class, "on_thread_switch", "thread_switch");
        callbacks_class.def("set", &linux_callback_registry::set)
            .def("clear", &linux_callback_registry::clear)
            .def("__repr__", [](const linux_callback_registry&) {
                return "<sogen.linux.Callbacks on_signal/on_exception share the same signal callback slot>";
            });

        nb::enum_<thread_wait_state>(m, "ThreadWaitState")
            .value("running", thread_wait_state::running)
            .value("futex_wait", thread_wait_state::futex_wait)
            .value("sleeping", thread_wait_state::sleeping)
            .export_values();

        nb::class_<sogen_linux_thread>(m, "Thread")
            .def_prop_ro("tid", [](const sogen_linux_thread& self) { return self.view().tid; })
            .def_prop_ro("stack_base", [](const sogen_linux_thread& self) { return self.view().stack_base; })
            .def_prop_ro("stack_size", [](const sogen_linux_thread& self) { return self.view().stack_size; })
            .def_prop_ro("fs_base", [](const sogen_linux_thread& self) { return self.view().fs_base; })
            .def_prop_ro("current_ip", &sogen_linux_thread::current_ip)
            .def_prop_ro("previous_ip", &sogen_linux_thread::previous_ip)
            .def_prop_ro("start_address", [](const sogen_linux_thread& self) { return self.view().start_address; })
            .def_prop_ro("wait_state", [](const sogen_linux_thread& self) { return self.view().wait_state; })
            .def_prop_ro("setup_done", [](const sogen_linux_thread&) { return true; })
            .def_prop_ro("terminated", [](const sogen_linux_thread& self) { return self.view().terminated; })
            .def_prop_ro("exit_code", [](const sogen_linux_thread& self) { return self.view().exit_code; })
            .def_prop_ro("executed_instructions", [](const sogen_linux_thread& self) { return self.view().executed_instructions; });

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
            .def_prop_ro("active_thread", &sogen_linux_process_context::active_thread)
            .def_prop_ro("threads", &sogen_linux_process_context::threads);

        nb::class_<linux_memory_region_info>(m, "MemoryRegionInfo")
            .def_prop_ro("start", [](const linux_memory_region_info& self) { return self.start; })
            .def_prop_ro("length", [](const linux_memory_region_info& self) { return self.length; })
            .def_prop_ro("permissions", [](const linux_memory_region_info& self) { return self.permissions; })
            .def_prop_ro("allocation_base", [](const linux_memory_region_info& self) { return self.allocation_base; })
            .def_prop_ro("allocation_length", [](const linux_memory_region_info& self) { return self.allocation_length; })
            .def_prop_ro("is_reserved", [](const linux_memory_region_info& self) { return self.is_reserved; })
            .def_prop_ro("is_committed", [](const linux_memory_region_info& self) { return self.is_committed; })
            .def_prop_ro("initial_permissions", [](const linux_memory_region_info& self) { return self.initial_permissions; })
            .def_prop_ro("kind", [](const linux_memory_region_info& self) { return self.kind; });

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
            .def("get_region_info", &linux_memory_manager::get_region_info)
            .def("get_mapped_regions", &linux_memory_manager::get_mapped_region_infos)
            .def("compute_memory_stats",
                 [](const linux_memory_manager& self) {
                     const auto stats = self.compute_memory_stats();
                     nb::dict result{};
                     set_dict_item(result, "region_count", stats.region_count);
                     set_dict_item(result, "mapped_bytes", stats.mapped_bytes);
                     set_dict_item(result, "executable_bytes", stats.executable_bytes);
                     return result;
                 })
            .def_prop_ro("mapped_regions", &linux_memory_manager::get_mapped_region_infos)
            .def_prop_rw("mmap_base", &linux_memory_manager::get_mmap_base, &linux_memory_manager::set_mmap_base);

        nb::class_<linux_debug_facade>(m, "Debug")
            .def("set_breakpoint", &linux_debug_facade::set_breakpoint)
            .def("clear_breakpoint", &linux_debug_facade::clear_breakpoint)
            .def("list_breakpoints", &linux_debug_facade::list_breakpoints)
            .def("step_into", &linux_debug_facade::step_into)
            .def("step_over", &linux_debug_facade::step_over)
            .def("step_out", &linux_debug_facade::step_out)
            .def("run_to", &linux_debug_facade::run_to)
            .def("continue_execution", &linux_debug_facade::continue_execution)
            .def("pause", &linux_debug_facade::pause)
            .def("registers", &linux_debug_facade::registers)
            .def("modules", &linux_debug_facade::modules)
            .def("threads", &linux_debug_facade::threads)
            .def("disassemble", &linux_debug_facade::disassemble)
            .def("call_stack", &linux_debug_facade::call_stack);

        nb::class_<sogen_linux_emulator>(m, "Emulator")
            .def("start", &sogen_linux_emulator::start, nb::arg("count") = 0, nb::call_guard<nb::gil_scoped_release>())
            .def("stop", &sogen_linux_emulator::stop)
            .def("save_snapshot", &sogen_linux_emulator::save_snapshot)
            .def("restore_snapshot", &sogen_linux_emulator::restore_snapshot)
            .def("serialize_state", &sogen_linux_emulator::serialize_state)
            .def("deserialize_state", &sogen_linux_emulator::deserialize_state)
            .def("read_memory", &sogen_linux_emulator::read_memory)
            .def("write_memory", &sogen_linux_emulator::write_memory)
            .def("read_register", &sogen_linux_emulator::read_register)
            .def("write_register", &sogen_linux_emulator::write_register)
            .def("find_module_by_address", &sogen_linux_emulator::find_module_by_address, nb::rv_policy::reference_internal)
            .def("find_module_by_name", &sogen_linux_emulator::find_module_by_name, nb::rv_policy::reference_internal)
            .def("activate_thread", &sogen_linux_emulator::activate_thread)
            .def("perform_thread_switch", &sogen_linux_emulator::perform_thread_switch)
            .def("yield_thread", &sogen_linux_emulator::yield_thread)
            .def("map_port", [](sogen_linux_emulator& self, uint16_t emulator_port,
                                uint16_t host_port) { self.native().port_mapper.map_port(emulator_port, host_port); })
            .def("get_host_port",
                 [](sogen_linux_emulator& self, uint16_t emulator_port) { return self.native().port_mapper.get_host_port(emulator_port); })
            .def("get_emulator_port",
                 [](sogen_linux_emulator& self, uint16_t host_port) { return self.native().port_mapper.get_emulator_port(host_port); })
            .def_prop_ro("executed_instructions", &sogen_linux_emulator::executed_instructions)
            .def_prop_ro("backend_name", &sogen_linux_emulator::backend_name)
            .def_prop_ro("emulation_root", &sogen_linux_emulator::emulation_root)
            .def_prop_ro("last_stop_reason", &sogen_linux_emulator::last_stop_reason)
            .def_prop_ro("last_stop_reason_code", &sogen_linux_emulator::last_stop_reason_code)
            .def_prop_ro("last_stop_detail", &sogen_linux_emulator::last_stop_detail)
            .def_prop_ro("process", &sogen_linux_emulator::process, nb::rv_policy::reference_internal)
            .def_prop_ro("memory", &sogen_linux_emulator::memory, nb::rv_policy::reference_internal)
            .def_prop_ro("modules", &sogen_linux_emulator::modules)
            .def_prop_ro("current_thread", &sogen_linux_emulator::current_thread)
            .def_prop_ro("current_thread_id",
                         [](const sogen_linux_emulator& self) -> nb::object {
                             const auto tid = self.current_thread_id();
                             if (!tid.has_value())
                             {
                                 return nb::none();
                             }
                             return nb::int_(*tid);
                         })
            .def_prop_ro(
                "callbacks",
                [](sogen_linux_emulator& self) -> linux_callback_registry& {
                    const auto owner = nb::find(&self);
                    if (owner.is_valid())
                    {
                        self.callbacks->owner = owner.ptr();
                    }
                    return *self.callbacks;
                },
                nb::rv_policy::reference_internal)
            .def_prop_ro(
                "hooks", [](sogen_linux_emulator& self) -> linux_hook_registry& { return *self.hooks; }, nb::rv_policy::reference_internal)
            .def_prop_ro(
                "debug", [](sogen_linux_emulator& self) -> linux_debug_facade& { return *self.debug; }, nb::rv_policy::reference_internal);

        nb::setattr(m, "LinuxEmulator", m.attr("Emulator"));

        m.def("create_empty", [](nb::kwargs kwargs) { // NOLINT(performance-unnecessary-value-param)
            return sogen_linux_emulator(create_empty_linux_emulator(kwargs));
        });

        m.def("create_application", [](const nb::object& application, const nb::kwargs& kwargs) {
            return sogen_linux_emulator(create_linux_application_emulator(application, nb::none(), kwargs));
        });
        m.def("create_application", [](const nb::object& application, const nb::object& application_args, const nb::kwargs& kwargs) {
            return sogen_linux_emulator(create_linux_application_emulator(application, application_args, kwargs));
        });
    }
}
