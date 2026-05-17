#include <nanobind/nanobind.h>

#include "sogen_internal.hpp"

using sogen_py::api_hook_registry;
using sogen_py::callback_registry;
using sogen_py::create_application_emulator;
using sogen_py::create_empty_emulator;
using sogen_py::hook_handle;
using sogen_py::hook_registry;
using sogen_py::read_memory_bytes;
using sogen_py::sogen_process_context;
using sogen_py::sogen_windows_emulator;
using sogen_py::stop_reason_to_string;
using sogen_py::write_memory_bytes;

namespace
{
    // Build a property accessor pair for a callback slot. Pairs nicely with
    // callback_registry::slot_for so all callback wiring lives in one table.
    template <auto Member>
    void bind_callback_property(nb::class_<callback_registry>& c, const char* name, const char* slot_name)
    {
        c.def_prop_rw(
            name, [](callback_registry& self) { return self.*Member; },
            [slot_name](callback_registry& self, nb::object callback) { self.set(slot_name, std::move(callback)); },
            nb::arg("callback").none());
    }
}

void register_sogen_runtime_bindings(nb::module_& m)
{
    m.def(
        "api_call",
        [](function_calling_convention cc, nb::object params, nb::object restype) {
            return nb::cpp_function([cc, params = std::move(params), restype = std::move(restype)](nb::handle fn) -> nb::object {
                nb::setattr(fn, "_sogen_api_cc", nb::cast(cc));
                nb::setattr(fn, "_sogen_api_params", params);
                nb::setattr(fn, "_sogen_api_restype", restype);
                return nb::borrow<nb::object>(fn);
            });
        },
        nb::arg("cc"), nb::arg("params") = nb::none(), nb::arg("restype") = nb::none());

    nb::class_<hook_handle>(m, "Hook").def("remove", &hook_handle::remove).def_prop_ro("active", &hook_handle::active);

    nb::class_<api_hook_registry>(m, "ApiHooks")
        .def("__setitem__", &api_hook_registry::set_item)
        .def("__delitem__", &api_hook_registry::del_item)
        .def("clear", &api_hook_registry::clear);

    nb::class_<hook_registry>(m, "Hooks")
        .def("memory_execution", &hook_registry::memory_execution)
        .def("memory_execution_at", &hook_registry::memory_execution_at)
        .def("memory_read", &hook_registry::memory_read)
        .def("memory_write", &hook_registry::memory_write)
        .def("instruction", &hook_registry::instruction)
        .def("interrupt", &hook_registry::interrupt)
        .def("memory_violation", &hook_registry::memory_violation)
        .def("basic_block", &hook_registry::basic_block)
        .def_prop_ro("apis", [](hook_registry& self) -> api_hook_registry& { return *self.apis; }, nb::rv_policy::reference_internal);

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

    auto callbacks_class = nb::class_<callback_registry>(m, "Callbacks");
    bind_callback_property<&callback_registry::module_load_cb>(callbacks_class, "on_module_load", "module_load");
    bind_callback_property<&callback_registry::module_unload_cb>(callbacks_class, "on_module_unload", "module_unload");
    bind_callback_property<&callback_registry::stdout_cb>(callbacks_class, "on_stdout", "stdout");
    bind_callback_property<&callback_registry::syscall_cb>(callbacks_class, "on_syscall", "syscall");
    bind_callback_property<&callback_registry::generic_access_cb>(callbacks_class, "on_generic_access", "generic_access");
    bind_callback_property<&callback_registry::generic_activity_cb>(callbacks_class, "on_generic_activity", "generic_activity");
    bind_callback_property<&callback_registry::suspicious_activity_cb>(callbacks_class, "on_suspicious_activity", "suspicious_activity");
    bind_callback_property<&callback_registry::exception_cb>(callbacks_class, "on_exception", "exception");
    bind_callback_property<&callback_registry::instruction_cb>(callbacks_class, "on_instruction", "instruction");
    bind_callback_property<&callback_registry::memory_protect_cb>(callbacks_class, "on_memory_protect", "memory_protect");
    bind_callback_property<&callback_registry::memory_allocate_cb>(callbacks_class, "on_memory_allocate", "memory_allocate");
    bind_callback_property<&callback_registry::memory_violate_cb>(callbacks_class, "on_memory_violate", "memory_violate");
    bind_callback_property<&callback_registry::rdtsc_cb>(callbacks_class, "on_rdtsc", "rdtsc");
    bind_callback_property<&callback_registry::rdtscp_cb>(callbacks_class, "on_rdtscp", "rdtscp");
    bind_callback_property<&callback_registry::ioctrl_cb>(callbacks_class, "on_ioctrl", "ioctrl");
    bind_callback_property<&callback_registry::debug_string_cb>(callbacks_class, "on_debug_string", "debug_string");
    bind_callback_property<&callback_registry::thread_create_cb>(callbacks_class, "on_thread_create", "thread_create");
    bind_callback_property<&callback_registry::thread_terminated_cb>(callbacks_class, "on_thread_terminated", "thread_terminated");
    bind_callback_property<&callback_registry::thread_set_name_cb>(callbacks_class, "on_thread_set_name", "thread_set_name");
    bind_callback_property<&callback_registry::thread_switch_cb>(callbacks_class, "on_thread_switch", "thread_switch");

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
        .def("stop", &sogen_windows_emulator::stop)
        .def("save_snapshot", &sogen_windows_emulator::save_snapshot)
        .def("restore_snapshot", &sogen_windows_emulator::restore_snapshot)
        .def("serialize_state", &sogen_windows_emulator::serialize_state)
        .def("deserialize_state", &sogen_windows_emulator::deserialize_state)
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

    m.def("create_empty", [](nb::kwargs kwargs) { // NOLINT(performance-unnecessary-value-param)
        return sogen_windows_emulator(create_empty_emulator(kwargs));
    });
    m.def(
        "create_application",
        [](nb::object application, nb::object application_args, nb::kwargs kwargs) { // NOLINT(performance-unnecessary-value-param)
            return sogen_windows_emulator(create_application_emulator(application, application_args, kwargs));
        },
        nb::arg("application"), nb::arg("args").none() = nb::none());
}
