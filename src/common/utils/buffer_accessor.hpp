#pragma once
#include <span>
#include <cstdint>
#include <stdexcept>

namespace utils
{
    template <typename T, typename S = const uint8_t>
        requires(std::is_trivially_copyable_v<T> && std::is_same_v<uint8_t, std::remove_cv_t<S>>)
    class safe_object_accessor
    {
      public:
        safe_object_accessor(const std::span<S> buffer, const size_t offset)
            : buffer_(buffer),
              offset_(offset)
        {
        }

        /*****************************************************************************
         * Object is copied to make sure platform-dependent alignment requirements
         * are respected
         ****************************************************************************/

        T get(const size_t element_index = 0) const
        {
            T value{};
            memcpy(&value, get_valid_pointer(element_index), size);
            return value;
        }

        void set(const T value, const size_t element_index = 0) const
        {
            memcpy(get_valid_pointer(element_index), &value, size);
        }

      private:
        static constexpr auto size = sizeof(T);

        std::span<S> buffer_{};
        size_t offset_{};

        S* get_valid_pointer(const size_t element_index) const
        {
            const auto start_offset = offset_ + (size * element_index);
            const auto end_offset = start_offset + size;
            if (end_offset > buffer_.size())
            {
                throw std::runtime_error("Buffer accessor overflow");
            }

            return buffer_.data() + start_offset;
        }
    };

    template <typename T>
        requires(std::is_same_v<uint8_t, std::remove_cv_t<T>>)
    class safe_buffer_accessor
    {
      public:
        safe_buffer_accessor(const std::span<T> buffer)
            : buffer_(buffer)
        {
        }

        template <typename S>
        safe_buffer_accessor(const safe_buffer_accessor<S>& obj)
            : buffer_(obj.get_buffer())
        {
        }

        template <typename S>
        safe_object_accessor<S, T> as(const size_t offset) const
        {
            return {this->buffer_, offset};
        }

        T* get_pointer_for_range(const size_t offset, const size_t size) const
        {
            this->validate(offset, size);
            return this->buffer_.data() + offset;
        }

        void validate(const size_t offset, const size_t size) const
        {
            const auto end = offset + size;
            if (end > buffer_.size())
            {
                throw std::runtime_error("Buffer accessor overflow");
            }
        }

        template <typename S = char>
        std::basic_string<S> as_string(const size_t offset) const
        {
            safe_object_accessor<S> string_accessor{this->buffer_, offset};
            std::basic_string<S> result{};

            while (true)
            {
                auto value = string_accessor.get(result.size());
                if (!value)
                {
                    return result;
                }

                result.push_back(std::move(value));
            }
        }

        std::span<T> get_buffer() const
        {
            return this->buffer_;
        }

      private:
        const std::span<T> buffer_{};
    };
}
