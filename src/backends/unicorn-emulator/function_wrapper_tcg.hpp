#pragma once

#include <array>
#include <memory>
#include <functional>

#include <utils/object.hpp>

template <typename T, size_t N>
T resolve_indexed_argument_internal(const std::array<size_t, N>& args, size_t& index)
{
    const auto a1 = args[index++];

    if constexpr (sizeof(T) <= sizeof(a1) || sizeof(size_t) > 4)
    {
        return T(a1);
    }
    else
    {
        const auto a2 = args[index++];

        const auto arg = (a1 | (static_cast<uint64_t>(a2) << 32));
        return T(arg);
    }
}

template <typename T, size_t N>
T resolve_indexed_argument(const std::array<size_t, N>& args, size_t& index)
{
    auto arg = resolve_indexed_argument_internal<T, N>(args, index);
    return arg;
}

template <typename ReturnType, typename... Args>
class function_wrapper_tcg : public utils::object
{
  public:
    using user_data_pointer = void*;
    using c_function_type = ReturnType(Args..., user_data_pointer);
    using functor_type = std::function<ReturnType(Args...)>;

    function_wrapper_tcg() = default;

    function_wrapper_tcg(functor_type functor)
        : functor_(std::make_unique<functor_type>(std::move(functor)))
    {
    }

    c_function_type* get_c_function() const
    {
        auto* func = +[](const size_t a1, const size_t a2, const size_t a3, const size_t a4, const size_t a5,
                         const size_t a6, const size_t a7, const size_t a8, const size_t a9, const size_t a10,
                         const size_t a11, const size_t a12) -> uint64_t {
            const std::array arguments = {a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12};

            const auto lambda = +[](Args... args, user_data_pointer user_data) -> ReturnType {
                return (*static_cast<functor_type*>(user_data))(std::forward<Args>(args)...);
            };

            size_t index = 0;
            std::tuple<Args..., user_data_pointer> func_args{
                resolve_indexed_argument<std::remove_cv_t<std::remove_reference_t<Args>>>(arguments, index)...,
                resolve_indexed_argument<user_data_pointer>(arguments, index)};

            (void)index;

            if constexpr (!std::is_void_v<ReturnType>)
            {
                return uint64_t(std::apply(lambda, std::move(func_args)));
            }

            std::apply(lambda, std::move(func_args));
            return 0;
        };

        return reinterpret_cast<c_function_type*>(reinterpret_cast<void*>(func));
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
