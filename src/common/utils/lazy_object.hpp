#pragma once

#include <optional>
#include <type_traits>

namespace utils
{
    template <typename F, typename T>
    concept CallableWithReturn = requires(const F f) {
        { f() } -> std::same_as<T>;
    };

    template <typename T, typename F>
        requires(CallableWithReturn<F, T>)
    class lazy_object
    {
      public:
        lazy_object(F accessor)
            : accessor_(std::move(accessor))
        {
        }

        operator const T&() const
        {
            return this->get();
        }

        operator T&()
        {
            return this->get();
        }

        T& operator->()
            requires std::is_pointer_v<T>
        {
            return this->get();
        }

        const T& operator->() const
            requires std::is_pointer_v<T>
        {
            return this->get();
        }

      private:
        F accessor_{};
        mutable std::optional<T> object_{};

        T& get() const
        {
            this->ensure_construction();
            return *this->object_;
        }

        void ensure_construction() const
        {
            if (!this->object_.has_value())
            {
                this->object_.emplace(this->accessor_());
            }
        }
    };

    template <typename F>
    auto make_lazy(F accessor)
    {
        return lazy_object<std::invoke_result_t<F>, F>(std::move(accessor));
    }
}
