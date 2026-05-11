#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/stl/filesystem.h>

#include <backend_selection.hpp>
#include <windows_emulator.hpp>
#include <platform/unicode.hpp>

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

        if (py::isinstance<py::str>(object) || py::isinstance<py::bytes>(object))
        {
            throw std::runtime_error("args must be a sequence of strings");
        }

        const py::sequence seq = py::reinterpret_borrow<py::sequence>(object);
        result.reserve(static_cast<size_t>(seq.size()));
        for (const auto& item : seq)
        {
            const auto value = py::cast<std::string>(item);
            result.emplace_back(u8_to_u16(value));
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
}

PYBIND11_MODULE(sogen, m)
{
    m.doc() = "Sogen Python bindings";

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
        .def_property_readonly("last_stop_reason", [](const windows_emulator& self) { return stop_reason_to_string(self.last_stop_reason()); })
        .def_property_readonly("last_stop_reason_code", [](const windows_emulator& self) { return static_cast<int>(self.last_stop_reason()); })
        .def_property_readonly("last_stop_detail", &windows_emulator::last_stop_detail)
        .def_property_readonly("backend_name", [](const windows_emulator& self) { return self.emu().get_name(); })
        .def_property_readonly("emulation_root", [](const windows_emulator& self) { return self.emulation_root; })
        .def_property_readonly("current_thread_id", [](const windows_emulator& self) -> py::object {
            if (!self.process.active_thread)
            {
                return py::none();
            }

            return py::int_(self.process.active_thread->id);
        });

    m.def("create_empty", [](py::kwargs kwargs) { return create_empty_emulator(kwargs); }, "Create an empty emulator session.");
    m.def("create_application",
          [](py::object application, py::object args, py::kwargs kwargs) { return create_application_emulator(application, args, kwargs); },
          py::arg("application"), py::arg("args") = py::none(), "Create an emulator session for an application.");
}
