#pragma once
#include <type_traits>

namespace utils
{
    class moved_marker
    {
      public:
        moved_marker() = default;

        moved_marker(const moved_marker& copy) = default;
        moved_marker& operator=(const moved_marker&) = default;

        moved_marker(moved_marker&& obj) noexcept
            : moved_marker()
        {
            this->operator=(std::move(obj));
        }

        moved_marker& operator=(moved_marker&& obj) noexcept
        {
            if (this != &obj)
            {
                this->was_moved_ = obj.was_moved_;
                obj.was_moved_ = true;
            }

            return *this;
        }

        ~moved_marker() = default;

        bool was_moved() const
        {
            return this->was_moved_;
        }

        void mark_as_moved()
        {
            this->was_moved_ = true;
        }

      private:
        bool was_moved_{false};
    };
}
