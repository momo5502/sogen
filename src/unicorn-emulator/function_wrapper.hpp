#pragma once

#include <memory>
#include <functional>

#include "object.hpp"

template <typename ReturnType, typename... Args>
class function_wrapper : public object
{
  public:
    using user_data_pointer = void*;
    using c_function_type = ReturnType(Args..., user_data_pointer);
    using functor_type = std::function<ReturnType(Args...)>;

    function_wrapper() = default;

    function_wrapper(functor_type functor)
        : functor_(std::make_unique<functor_type>(std::move(functor)))
    {
    }

    c_function_type* get_c_function() const
    {
        return +[](Args... args, user_data_pointer user_data) -> ReturnType {
            return (*static_cast<functor_type*>(user_data))(std::forward<Args>(args)...);
        };
    }

    void* get_function() const
    {
        return reinterpret_cast<void*>(this->get_c_function());
    }

    user_data_pointer get_user_data() const
    {
        return this->functor_.get();
    }

  private:
    std::unique_ptr<functor_type> functor_{};
};
