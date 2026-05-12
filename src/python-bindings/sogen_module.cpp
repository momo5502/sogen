#include <nanobind/nanobind.h>

namespace nb = nanobind;

void register_sogen_bindings(nb::module_& m);

NB_MODULE(sogen, m)
{
    register_sogen_bindings(m);
}
