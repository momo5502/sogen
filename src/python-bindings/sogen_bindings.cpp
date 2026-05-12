#include <nanobind/nanobind.h>

#include "sogen_bindings_common.hpp"

void register_sogen_bindings(nb::module_& m)
{
    m.doc() = "Sogen Python bindings";
    register_sogen_types_bindings(m);
    register_sogen_runtime_bindings(m);
}
