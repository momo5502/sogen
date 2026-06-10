#include <nanobind/nanobind.h>

#include "sogen_internal.hpp"

namespace nb = nanobind;

namespace sogen::py
{
    namespace
    {
        void register_bindings(nb::module_& m)
        {
            m.doc() = "Sogen Python bindings";
            register_types_bindings(m);

            auto windows = m.def_submodule("windows", "Windows emulator bindings");
            register_windows_runtime_bindings(windows);

            auto linux_mod = m.def_submodule("linux", "Linux emulator bindings");
            register_linux_runtime_bindings(linux_mod);

            register_runtime_bindings(m);
        }
    }
}

NB_MODULE(sogen, m)
{
#ifdef SOGEN_DISABLE_NANOBIND_LEAK_WARNINGS
    nb::set_leak_warnings(false);
#endif
    sogen::py::register_bindings(m);
}
