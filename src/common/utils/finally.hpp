#pragma once
#include <utility>
#include <type_traits>

namespace utils
{
    /*
     * Copied from here:
     * https://github.com/microsoft/GSL/blob/e0880931ae5885eb988d1a8a57acf8bc2b8dacda/include/gsl/util#L57
     */

    template <class F>
    class final_action
    {
      public:
        static_assert(!std::is_reference_v<F> && !std::is_const_v<F> && !std::is_volatile_v<F>,
                      "Final_action should store its callable by value");

        explicit final_action(F f) noexcept
            : f_(std::move(f))
        {
        }

        final_action(final_action&& other) noexcept
            : f_(std::move(other.f_)),
              invoke_(std::exchange(other.invoke_, false))
        {
        }

        final_action(const final_action&) = delete;
        final_action& operator=(const final_action&) = delete;
        final_action& operator=(final_action&&) = delete;

        ~final_action() noexcept
        {
            if (this->invoke_)
            {
                this->f_();
            }
        }

        // Added by momo5502
        void cancel()
        {
            this->invoke_ = false;
        }

      private:
        F f_;
        bool invoke_{true};
    };

    template <class F>
    final_action<std::remove_cv_t<std::remove_reference_t<F>>> finally(F&& f) noexcept
    {
        return final_action<std::remove_cv_t<std::remove_reference_t<F>>>(std::forward<F>(f));
    }
}
