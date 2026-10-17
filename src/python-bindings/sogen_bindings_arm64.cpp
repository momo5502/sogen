#include "sogen_bindings.hpp"

#include <memory_manager.hpp>

namespace sogen::py
{
    namespace
    {
        class arm64_machine
        {
          public:
            arm64_machine()
                : arm64_machine(get_arm64_emulator_backend_from_environment())
            {
            }

            explicit arm64_machine(const backend_type backend)
                : machine_(create_arm64_emulator(backend)),
                  memory_(*machine_)
            {
                machine_->hook_instruction(arm64_hookable_instructions::svc, [this](cpu_interface&, const uint64_t id) {
                    nb::gil_scoped_acquire acquire{};
                    if (syscall_callback_.is_none())
                    {
                        throw std::runtime_error("ARM64 SVC requires an on_svc Python callback");
                    }
                    auto callback = syscall_callback_;
                    return coerce_instruction_continuation(callback(nb::cast(this, nb::rv_policy::reference), id));
                });
            }

            void start(const size_t count)
            {
                machine_->start(count);
            }

            void step()
            {
                start(1);
            }

            void stop()
            {
                machine_->stop();
            }

            void map_memory(const uint64_t address, const size_t size, const memory_permission permissions)
            {
                if (!memory_.allocate_memory(address, size, permissions))
                {
                    throw std::runtime_error("Cannot allocate ARM64 guest memory at " + std::to_string(address));
                }
            }

            bool unmap_memory(const uint64_t address, const size_t size)
            {
                return memory_.release_memory(address, size);
            }

            nb::bytes read_memory(const uint64_t address, const size_t size) const
            {
                return read_memory_bytes(*machine_, address, size);
            }

            void write_memory(const uint64_t address, const nb::bytes& data)
            {
                write_memory_bytes(*machine_, address, data);
            }

            uint64_t read_register(const arm64_register reg) const
            {
                return machine_->reg(reg);
            }

            void write_register(const arm64_register reg, const uint64_t value)
            {
                machine_->reg(reg, value);
            }

            nb::bytes save_registers() const
            {
                const auto data = machine_->save_registers();
                return nb::bytes(reinterpret_cast<const char*>(data.data()), data.size());
            }

            void restore_registers(const nb::bytes& data)
            {
                std::vector<std::byte> bytes(data.size());
                if (!bytes.empty())
                {
                    std::memcpy(bytes.data(), data.c_str(), bytes.size());
                }
                machine_->restore_registers(bytes);
            }

            nb::object on_svc() const
            {
                return syscall_callback_;
            }

            void on_svc(const nb::object& callback)
            {
                if (!callback.is_none() && !PyCallable_Check(callback.ptr()))
                {
                    throw std::invalid_argument("on_svc must be callable or None");
                }
                syscall_callback_ = callback;
            }

            std::string backend_name() const
            {
                return machine_->get_name();
            }

            bool supports_single_step() const
            {
                return machine_->supports_instruction_counting();
            }

          private:
            std::unique_ptr<arm64_emulator> machine_;
            memory_manager memory_;
            nb::object syscall_callback_ = nb::none();
        };
    }

    void register_arm64_runtime_bindings(nb::module_& root)
    {
        auto module = root.def_submodule("arm64", "Low-level ARM64 guest CPU bindings; not a Windows process personality");
        module.def("default_backend", &get_arm64_emulator_backend_from_environment,
                   "The backend ARM64 guests use without an explicit choice (hardware where usable, else Unicorn)");
        auto registers = nb::enum_<arm64_register>(module, "Register");
        for (int index = 0; index <= 30; ++index)
        {
            auto xreg = static_cast<arm64_register>(static_cast<int>(arm64_register::x0) + index);
            if (index == 29)
            {
                xreg = arm64_register::x29;
            }
            else if (index == 30)
            {
                xreg = arm64_register::x30;
            }
            registers.value(("x" + std::to_string(index)).c_str(), xreg);
            registers.value(("w" + std::to_string(index)).c_str(),
                            static_cast<arm64_register>(static_cast<int>(arm64_register::w0) + index));
        }
        registers.value("pc", arm64_register::pc)
            .value("sp", arm64_register::sp)
            .value("nzcv", arm64_register::nzcv)
            .value("pstate", arm64_register::pstate)
            .value("xzr", arm64_register::xzr);

        nb::class_<arm64_machine>(module, "Emulator")
            .def(nb::init<>())
            .def(nb::init<backend_type>(), nb::arg("backend"))
            .def("start", &arm64_machine::start, nb::arg("count") = 0, nb::call_guard<nb::gil_scoped_release>())
            .def("step", &arm64_machine::step, nb::call_guard<nb::gil_scoped_release>())
            .def("stop", &arm64_machine::stop)
            .def("map_memory", &arm64_machine::map_memory, nb::arg("address"), nb::arg("size"), nb::arg("permissions"))
            .def("unmap_memory", &arm64_machine::unmap_memory)
            .def("read_memory", &arm64_machine::read_memory)
            .def("write_memory", &arm64_machine::write_memory)
            .def("read_register", &arm64_machine::read_register)
            .def("write_register", &arm64_machine::write_register)
            .def("save_registers", &arm64_machine::save_registers)
            .def("restore_registers", &arm64_machine::restore_registers)
            .def_prop_rw("on_svc", static_cast<nb::object (arm64_machine::*)() const>(&arm64_machine::on_svc),
                         static_cast<void (arm64_machine::*)(const nb::object&)>(&arm64_machine::on_svc), nb::arg("callback").none())
            .def_prop_ro("backend_name", &arm64_machine::backend_name)
            .def_prop_ro("supports_single_step", &arm64_machine::supports_single_step);
    }
}
