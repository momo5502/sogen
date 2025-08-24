#pragma once
#include <span>
#include <cstdint>
#include <stdexcept>

namespace utils
{
    template <typename Type, typename SpanElement = const std::byte>
        requires(std::is_trivially_copyable_v<Type> &&
                 (std::is_same_v<uint8_t, std::remove_cv_t<SpanElement>> || std::is_same_v<std::byte, std::remove_cv_t<SpanElement>>))
    class safe_object_accessor
    {
      public:
        safe_object_accessor(const std::span<SpanElement> buffer, const size_t offset)
            : buffer_(buffer),
              offset_(offset)
        {
        }

        /*****************************************************************************
         * Object is copied to make sure platform-dependent alignment requirements
         * are respected
         ****************************************************************************/

        Type get(const size_t element_index = 0) const
        {
            Type value{};
            memcpy(&value, get_valid_pointer(element_index), size);
            return value;
        }

        void set(const Type value, const size_t element_index = 0) const
        {
            memcpy(get_valid_pointer(element_index), &value, size);
        }

      private:
        static constexpr auto size = sizeof(Type);

        std::span<SpanElement> buffer_{};
        size_t offset_{};

        SpanElement* get_valid_pointer(const size_t element_index) const
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

    template <typename SpanElement>
        requires(std::is_same_v<uint8_t, std::remove_cv_t<SpanElement>> || std::is_same_v<std::byte, std::remove_cv_t<SpanElement>>)
    class safe_buffer_accessor
    {
      public:
        safe_buffer_accessor(const std::span<SpanElement> buffer)
            : buffer_(buffer)
        {
        }

        template <typename OtherSpanElement>
            requires(std::is_same_v<std::remove_cv_t<SpanElement>, std::remove_cv_t<OtherSpanElement>>)
        safe_buffer_accessor(const safe_buffer_accessor<OtherSpanElement>& obj)
            : buffer_(obj.get_buffer())
        {
        }

        template <typename Type>
        safe_object_accessor<Type, SpanElement> as(const size_t offset) const
        {
            return {this->buffer_, offset};
        }

        SpanElement* get_pointer_for_range(const size_t offset, const size_t size) const
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

        template <typename Char = char>
        std::basic_string<Char> as_string(const size_t offset) const
        {
            safe_object_accessor<Char> string_accessor{this->buffer_, offset};
            std::basic_string<Char> result{};

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

        std::span<SpanElement> get_buffer() const
        {
            return this->buffer_;
        }

      private:
        const std::span<SpanElement> buffer_{};
    };
}
