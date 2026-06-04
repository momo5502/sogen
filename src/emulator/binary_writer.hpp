#pragma once

namespace sogen
{
    namespace utils
    {
        class aligned_binary_writer;

        template <typename T>
        concept Writable = requires(const T ac, aligned_binary_writer& writer) {
            { ac.write(writer) } -> std::same_as<void>;
        };

        class aligned_binary_writer
        {
          public:
            static constexpr size_t pointer_size_32 = 4;
            static constexpr size_t pointer_size_64 = 8;

            aligned_binary_writer(memory_interface& mem, uint64_t address, const size_t pointer_size = pointer_size_64)
                : memory(&mem),
                  base_address(address),
                  current_position(address),
                  pointer_size_(validate_pointer_size(pointer_size))
            {
            }

            explicit aligned_binary_writer(std::vector<uint8_t>& buffer, const size_t pointer_size = pointer_size_64)
                : buffer(&buffer),
                  pointer_size_(validate_pointer_size(pointer_size))
            {
            }

            void write(const void* data, size_t size, size_t alignment = 1)
            {
                align_to(alignment);
                write_at(static_cast<size_t>(current_position - base_address), data, size);
                current_position += size;
            }

            void write_at(uint64_t offset, const void* data, size_t size)
            {
                if (buffer)
                {
                    if (offset + size > buffer->size())
                    {
                        buffer->resize(static_cast<size_t>(offset + size), 0);
                    }

                    std::memcpy(buffer->data() + offset, data, size);
                }
                else
                {
                    memory->write_memory(base_address + offset, data, size);
                }
            }

            template <typename T>
            void write_at(uint64_t offset, const T& value)
            {
                write_at(offset, &value, sizeof(value));
            }

            template <typename T>
                requires(!sogen::utils::is_optional<T>::value)
            void write(const T& value)
            {
                constexpr auto is_trivially_copyable = std::is_trivially_copyable_v<T>;

                if constexpr (Writable<T>)
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
                    write_pointer_sized(0);
                    return;
                }

                write_pointer_sized(next_referent_id_);
                next_referent_id_ += pointer_size_;
            }

            void write_ndr_u16string(const std::u16string_view str, const bool include_nul = true)
            {
                const auto max_char_count = str.size() + 1;
                const auto actual_char_count = str.size() + (include_nul ? 1 : 0);

                write_pointer_sized(max_char_count);
                write_pointer_sized(0);
                write_pointer_sized(actual_char_count);
                if (!str.empty())
                {
                    write(str.data(), str.size() * sizeof(char16_t));
                }
                if (include_nul)
                {
                    constexpr char16_t nul{};
                    write(&nul, sizeof(nul));
                }
            }

            void write_pointer_sized(const uint64_t value)
            {
                if (pointer_size_ == pointer_size_32)
                {
                    write(static_cast<uint32_t>(value));
                    return;
                }

                write(value);
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

            size_t pointer_size() const
            {
                return pointer_size_;
            }

          private:
            static size_t validate_pointer_size(const size_t pointer_size)
            {
                if (pointer_size != pointer_size_32 && pointer_size != pointer_size_64)
                {
                    throw std::invalid_argument("Pointer size must be 4 or 8");
                }

                return pointer_size;
            }

            memory_interface* memory{};
            std::vector<uint8_t>* buffer{};
            uint64_t base_address{};
            uint64_t current_position{};
            size_t pointer_size_{pointer_size_64};
            uint64_t next_referent_id_{0x20000};
        };
    }

} // namespace sogen
