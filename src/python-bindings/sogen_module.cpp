#include <nanobind/nanobind.h>

#include "sogen_internal.hpp"

namespace nb = nanobind;

namespace
{
    void register_sogen_bindings(nb::module_& m)
    {
        m.doc() = "Sogen Python bindings";
        register_sogen_types_bindings(m);

        auto windows = m.def_submodule("windows", "Windows emulator bindings");
        register_sogen_windows_runtime_bindings(windows);
        register_sogen_runtime_bindings(m);
    }
}

NB_MODULE(sogen, m)
{
#ifdef SOGEN_DISABLE_NANOBIND_LEAK_WARNINGS
    nb::set_leak_warnings(false);
#endif
    register_sogen_bindings(m);
}
