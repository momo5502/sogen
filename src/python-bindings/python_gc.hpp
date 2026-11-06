#pragma once

#include "sogen_bindings.hpp"

#include <array>
#include <memory>
#include <type_traits>
#include <utility>

namespace sogen::py
{
    template <typename T>
    struct owned_view
    {
        T* value{};
        nb::object owner = nb::none();
        std::shared_ptr<T> storage{};

        T& get() const
        {
            return *this->value;
        }
    };

    template <typename T>
    int traverse_owner(PyObject* self, visitproc visit, void* arg)
    {
        Py_VISIT(Py_TYPE(self));
        if (nb::inst_ready(nb::handle(self)))
        {
            Py_VISIT(nb::inst_ptr<T>(nb::handle(self))->owner.ptr());
        }
        return 0;
    }

    template <typename T>
    inline std::array owner_gc_slots = {
        PyType_Slot{.slot = Py_tp_traverse, .pfunc = reinterpret_cast<void*>(traverse_owner<T>)},
        PyType_Slot{},
    };

    template <typename R, typename T, typename... Args>
    auto view_method(R (T::*method)(Args...))
    {
        return [method](const owned_view<T>& self, Args... args) -> R {
            if constexpr (std::is_same_v<R, hook_handle>)
            {
                auto result = (self.get().*method)(std::forward<Args>(args)...);
                result.owner = self.owner;
                return result;
            }
            else
            {
                return (self.get().*method)(std::forward<Args>(args)...);
            }
        };
    }

    template <typename R, typename T, typename... Args>
    auto view_method(R (T::*method)(Args...) const)
    {
        return [method](const owned_view<T>& self, Args... args) -> R { return (self.get().*method)(std::forward<Args>(args)...); };
    }

    template <typename R, typename T, typename... Args>
    auto view_method(R (T::*method)(Args...) noexcept)
    {
        return view_method(static_cast<R (T::*)(Args...)>(method));
    }

    template <typename R, typename T, typename... Args>
    auto view_method(R (T::*method)(Args...) const noexcept)
    {
        return view_method(static_cast<R (T::*)(Args...) const>(method));
    }

    extern std::array<PyType_Slot, 3> linux_emulator_gc_slots;
}
