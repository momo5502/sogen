#pragma once

namespace utils
{
    template <typename Traits>
    class aligned_binary_writer;

    template <typename T, typename Traits>
    concept Writable = requires(const T ac, aligned_binary_writer<Traits>& writer) {
        { ac.write(writer) } -> std::same_as<void>;
    };

    template <typename Traits>
    class aligned_binary_writer
    {
      public:
        aligned_binary_writer(memory_interface& mem, uint64_t address)
            : memory(mem),
              base_address(address),
              current_position(address)
        {
        }

        void write(const void* data, size_t size, size_t alignment = 1)
        {
            align_to(alignment);
            memory.write_memory(current_position, data, size);
            current_position += size;
        }

        template <typename T>
            requires(!is_optional<T>::value)
        void write(const T& value)
        {
            constexpr auto is_trivially_copyable = std::is_trivially_copyable_v<T>;

            if constexpr (Writable<T, Traits>)
            {
                value.write(*this);
            }
            else if constexpr (is_trivially_copyable)
            {
                write(&value, sizeof(T), alignof(T));
            }
            else
            {
                static_assert(std::is_trivially_copyable_v<T>, "Type must be trivially copyable or be writable!");
                std::abort();
            }
        }

        void write_ndr_pointer(bool not_null)
        {
            if (!not_null)
            {
                write<typename Traits::PVOID>(0);
                return;
            }

            write<typename Traits::PVOID>(next_referent_id_);
            next_referent_id_ += sizeof(typename Traits::PVOID);
        }

        void write_ndr_u16string(const std::u16string& str)
        {
            size_t char_count = str.size() + 1;
            size_t byte_length = char_count * sizeof(char16_t);

            write<typename Traits::SIZE_T>(char_count);
            write<typename Traits::SIZE_T>(0);
            write<typename Traits::SIZE_T>(char_count);
            write(str.c_str(), byte_length);
        }

        void pad(size_t count)
        {
            std::vector<uint8_t> padding(count, 0);
            write(padding.data(), count);
        }

        void align_to(size_t alignment)
        {
            size_t offset_val = static_cast<size_t>(current_position) % alignment;
            if (offset_val != 0)
            {
                pad(alignment - offset_val);
            }
        }

        uint64_t position() const
        {
            return current_position;
        }

        uint64_t offset() const
        {
            return current_position - base_address;
        }

      private:
        memory_interface& memory; // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members)
        uint64_t base_address;
        uint64_t current_position;
        typename Traits::PVOID next_referent_id_{0x20000};
    };
}
