#pragma once

#include <memory>
#include <functional>

#include <utils/object.hpp>

uint32_t resolve_indexed_argument_part(uint32_t* args, size_t& index)
{
    return args[index++];
}

template <typename T>
T resolve_indexed_argument_internal(uint32_t* args, size_t& index)
{
    const auto a1 = resolve_indexed_argument_part(args, index);

    if(sizeof(T) <= sizeof(a1)) {
        return (T)a1;
    }

    const auto a2 = resolve_indexed_argument_part(args, index);

    const auto arg = (a1 | ((uint64_t)a2  << 32));
    return (T)arg;
}

template <typename T>
T resolve_indexed_argument(uint32_t* args, size_t& index)
{
    auto arg = resolve_indexed_argument_internal<T>(args, index);
    return arg;
}

template <typename ReturnType, typename... Args>
class function_wrapper2 : public utils::object
{
  public:
    using user_data_pointer = void*;
    using c_function_type = ReturnType(Args..., user_data_pointer);
    using functor_type = std::function<ReturnType(Args...)>;

    function_wrapper2() = default;

    function_wrapper2(functor_type functor)
        : functor_(std::make_unique<functor_type>(std::move(functor)))
    {
    }

    c_function_type* get_c_function() const
    {
        return (c_function_type*)(void*)+[](uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4,uint32_t a5,uint32_t a6,uint32_t a7,uint32_t a8,uint32_t a9,uint32_t a10,uint32_t a11,uint32_t a12)-> uint64_t {

            uint32_t real_args[] {
                a1,a2,a3,a4,a5,a6,a7,a8,a9,a10,a11,a12
            };

            const auto lambda = +[](Args... args, user_data_pointer user_data) -> ReturnType {
                return (*static_cast<functor_type*>(user_data))(std::forward<Args>(args)...);
            };

            size_t index = 0;
    std::tuple<Args..., user_data_pointer> func_args{resolve_indexed_argument<std::remove_cv_t<std::remove_reference_t<Args>>>(real_args, index)..., resolve_indexed_argument<user_data_pointer>(real_args, index)};

    (void)index;

    if constexpr(!std::is_void_v<ReturnType>){
        return (uint64_t)std::apply(lambda, std::move(func_args));
    }
            
    std::apply(lambda, std::move(func_args));
    return 0;
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
