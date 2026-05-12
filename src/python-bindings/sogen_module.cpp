#include <nanobind/nanobind.h>

namespace nb = nanobind;

void register_sogen_bindings(nb::module_& m);

NB_MODULE(sogen, m)
{
#ifdef SOGEN_DISABLE_NANOBIND_LEAK_WARNINGS
    nb::set_leak_warnings(false);
#endif
    register_sogen_bindings(m);
}
