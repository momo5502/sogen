#pragma once

#include <functional>
#include <vector>
#include <algorithm>
#include <cstddef>

namespace utils
{
    template <typename Signature>
    class optional_function;

    template <typename Ret, typename... Args>
    class optional_function<Ret(Args...)>
    {
        std::function<Ret(Args...)> func;

      public:
        optional_function() = default;

        optional_function(std::function<Ret(Args...)> f)
            : func(std::move(f))
        {
        }

        template <typename F>
            requires(std::is_invocable_r_v<Ret, F, Args...>)
        optional_function(F&& f)
            : func(std::forward<F>(f))
        {
        }

        template <typename F>
            requires(std::is_invocable_r_v<Ret, F, Args...>)
        optional_function& operator=(F&& f)
        {
            func = std::forward<F>(f);
            return *this;
        }

        Ret operator()(Args... args) const
        {
            if (func)
            {
                return func(std::forward<Args>(args)...);
            }

            if constexpr (!std::is_void_v<Ret>)
            {
                return Ret();
            }
        }

        explicit operator bool() const noexcept
        {
            return static_cast<bool>(func);
        }
    };

    using callback_id_type = std::size_t;

    template <typename Signature>
    class callback_list;

    template <typename... Args>
    class callback_list<void(Args...)>
    {
      public:
        using function_type = std::function<void(Args...)>;

        callback_id_type add(function_type fn)
        {
            callbacks.emplace_back(next_id, std::move(fn));
            return next_id++;
        }

        void remove(callback_id_type id)
        {
            callbacks.erase(std::remove_if(callbacks.begin(), callbacks.end(), [id](auto& pair) { return pair.first == id; }),
                            callbacks.end());
        }

        void operator()(Args... args) const
        {
            for (auto& [id, fn] : callbacks)
            {
                fn(args...);
            }
        }

        explicit operator bool() const noexcept
        {
            return !callbacks.empty();
        }

      private:
        std::vector<std::pair<callback_id_type, function_type>> callbacks;
        callback_id_type next_id = 1;
    };
}
