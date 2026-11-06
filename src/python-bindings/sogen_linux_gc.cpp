#include "sogen_internal.hpp"

#include <linux_emulator.hpp>

#include <algorithm>
#include <exception>

namespace sogen::py
{
    namespace
    {
        int traverse_linux_emulator(PyObject* self, visitproc visit, void* arg)
        {
            Py_VISIT(Py_TYPE(self));
            if (!nb::inst_ready(nb::handle(self)))
            {
                return 0;
            }

            const auto* emulator = nb::inst_ptr<sogen_linux_emulator>(nb::handle(self));
            if (emulator->callbacks)
            {
                if (const auto result = emulator->callbacks->traverse(visit, arg))
                {
                    return result;
                }
            }
            if (emulator->hooks)
            {
                for (const auto& reference : emulator->hooks->callback_references)
                {
                    if (const auto callback = reference.lock())
                    {
                        Py_VISIT(callback->ptr());
                    }
                }
                for (const auto& [_, entry] : emulator->hooks->symbols->entries)
                {
                    Py_VISIT(entry.params.ptr());
                    Py_VISIT(entry.restype.ptr());
                    Py_VISIT(entry.callback.ptr());
                }
            }
            for (const auto& [_, reference] : emulator->view_cache)
            {
                Py_VISIT(reference.ptr());
            }
            return 0;
        }

        int clear_linux_emulator(PyObject* self)
        {
            if (!nb::inst_ready(nb::handle(self)))
            {
                return 0;
            }
            auto guard = nb::borrow<nb::object>(nb::handle(self));
            auto* emulator = nb::inst_ptr<sogen_linux_emulator>(nb::handle(self));
            try
            {
                if (emulator->hooks)
                {
                    auto hooks = std::exchange(emulator->hooks->active_hooks, {});
                    auto symbols = std::exchange(emulator->hooks->symbols->entries, {});
                    for (const auto& hook : hooks)
                    {
                        hook.remove();
                    }
                    for (auto& [_, entry] : symbols)
                    {
                        entry.hooks.clear();
                    }
                }
                if (emulator->callbacks)
                {
                    emulator->callbacks->clear_references();
                }
                emulator->view_cache.clear();
                return 0;
            }
            catch (const std::exception& error)
            {
                PyErr_SetString(PyExc_RuntimeError, error.what());
                return -1;
            }
        }
    }

    std::array<PyType_Slot, 3> linux_emulator_gc_slots = {
        PyType_Slot{.slot = Py_tp_traverse, .pfunc = reinterpret_cast<void*>(traverse_linux_emulator)},
        PyType_Slot{.slot = Py_tp_clear, .pfunc = reinterpret_cast<void*>(clear_linux_emulator)},
        PyType_Slot{},
    };

    std::shared_ptr<nb::object> linux_hook_registry::capture_callback(nb::object callback)
    {
        std::erase_if(this->callback_references, [](const auto& reference) { return reference.expired(); });
        auto result = std::shared_ptr<nb::object>{
            new nb::object(std::move(callback)),
            [](nb::object* value) {
                nb::gil_scoped_acquire gil{};
                delete value;
            },
        };
        this->callback_references.emplace_back(result);
        return result;
    }

    nb::object copy_linux_module(const linux_mapped_module& module)
    {
        auto storage = std::make_shared<linux_mapped_module>(module);
        return nb::cast(owned_view<linux_mapped_module>{.value = storage.get(), .storage = std::move(storage)});
    }
}
