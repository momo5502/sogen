#pragma once
#include "memory_utils.hpp"
#include <x64_emulator.hpp>

// TODO: Replace with pointer handling structure for future 32 bit support
using emulator_pointer = uint64_t;

template <typename T>
class object_wrapper
{
    T* obj_;

  public:
    object_wrapper(T& obj)
        : obj_(&obj)
    {
    }

    T& get() const
    {
        return *this->obj_;
    }

    operator T&() const
    {
        return this->get();
    }

    void serialize(utils::buffer_serializer&) const
    {
    }

    void deserialize(utils::buffer_deserializer&)
    {
    }
};

class windows_emulator;
struct process_context;

using x64_emulator_wrapper = object_wrapper<x64_emulator>;
using process_context_wrapper = object_wrapper<process_context>;
using windows_emulator_wrapper = object_wrapper<windows_emulator>;

template <typename T>
class emulator_object
{
  public:
    using value_type = T;

    emulator_object(const x64_emulator_wrapper& wrapper, const uint64_t address = 0)
        : emulator_object(wrapper.get(), address)
    {
    }

    emulator_object(emulator& emu, const uint64_t address = 0)
        : emu_(&emu),
          address_(address)
    {
    }

    emulator_object(emulator& emu, const void* address)
        : emulator_object(emu, reinterpret_cast<uint64_t>(address))
    {
    }

    uint64_t value() const
    {
        return this->address_;
    }

    constexpr uint64_t size() const
    {
        return sizeof(T);
    }

    uint64_t end() const
    {
        return this->value() + this->size();
    }

    T* ptr() const
    {
        return reinterpret_cast<T*>(this->address_);
    }

    operator bool() const
    {
        return this->address_ != 0;
    }

    T read(const size_t index = 0) const
    {
        T obj{};
        this->emu_->read_memory(this->address_ + index * this->size(), &obj, sizeof(obj));
        return obj;
    }

    void write(const T& value, const size_t index = 0) const
    {
        this->emu_->write_memory(this->address_ + index * this->size(), &value, sizeof(value));
    }

    void write_if_valid(const T& value, const size_t index = 0) const
    {
        if (this->operator bool())
        {
            this->write(value, index);
        }
    }

    template <typename F>
    void access(const F& accessor, const size_t index = 0) const
    {
        T obj{};
        this->emu_->read_memory(this->address_ + index * this->size(), &obj, sizeof(obj));

        accessor(obj);

        this->write(obj, index);
    }

    void serialize(utils::buffer_serializer& buffer) const
    {
        buffer.write(this->address_);
    }

    void deserialize(utils::buffer_deserializer& buffer)
    {
        buffer.read(this->address_);
    }

    void set_address(const uint64_t address)
    {
        this->address_ = address;
    }

  private:
    emulator* emu_{};
    uint64_t address_{};
};

// TODO: warning emulator_utils is hardcoded for 64bit unicode_string usage
class emulator_allocator
{
  public:
    emulator_allocator(emulator& emu)
        : emu_(&emu)
    {
    }

    emulator_allocator(emulator& emu, const uint64_t address, const uint64_t size)
        : emu_(&emu),
          address_(address),
          size_(size),
          active_address_(address)
    {
    }

    uint64_t reserve(const uint64_t count, const uint64_t alignment = 1)
    {
        const auto potential_start = align_up(this->active_address_, alignment);
        const auto potential_end = potential_start + count;
        const auto total_end = this->address_ + this->size_;

        if (potential_end > total_end)
        {
            throw std::runtime_error("Out of memory");
        }

        this->active_address_ = potential_end;

        return potential_start;
    }

    template <typename T>
    emulator_object<T> reserve(const size_t count = 1)
    {
        const auto potential_start = this->reserve(sizeof(T) * count, alignof(T));
        return emulator_object<T>(*this->emu_, potential_start);
    }

    char16_t* copy_string(const std::u16string_view str)
    {
        UNICODE_STRING<EmulatorTraits<Emu64>> uc_str{};
        this->make_unicode_string(uc_str, str);
        return reinterpret_cast<char16_t*>(uc_str.Buffer);
    }

    void make_unicode_string(UNICODE_STRING<EmulatorTraits<Emu64>>& result, const std::u16string_view str)
    {
        constexpr auto element_size = sizeof(str[0]);
        constexpr auto required_alignment = alignof(decltype(str[0]));
        const auto total_length = str.size() * element_size;

        const auto string_buffer = this->reserve(total_length + element_size, required_alignment);

        this->emu_->write_memory(string_buffer, str.data(), total_length);

        constexpr std::array<char, element_size> nullbyte{};
        this->emu_->write_memory(string_buffer + total_length, nullbyte.data(), nullbyte.size());

        result.Buffer = string_buffer;
        result.Length = static_cast<USHORT>(total_length);
        result.MaximumLength = static_cast<USHORT>(total_length + element_size);
    }

    emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> make_unicode_string(const std::u16string_view str)
    {
        const auto unicode_string = this->reserve<UNICODE_STRING<EmulatorTraits<Emu64>>>();

        unicode_string.access(
            [&](UNICODE_STRING<EmulatorTraits<Emu64>>& unicode_str) { this->make_unicode_string(unicode_str, str); });

        return unicode_string;
    }

    uint64_t get_base() const
    {
        return this->address_;
    }

    uint64_t get_size() const
    {
        return this->size_;
    }

    uint64_t get_next_address() const
    {
        return this->active_address_;
    }

    emulator& get_emulator() const
    {
        return *this->emu_;
    }

    void serialize(utils::buffer_serializer& buffer) const
    {
        buffer.write(this->address_);
        buffer.write(this->size_);
        buffer.write(this->active_address_);
    }

    void deserialize(utils::buffer_deserializer& buffer)
    {
        buffer.read(this->address_);
        buffer.read(this->size_);
        buffer.read(this->active_address_);
    }

    void release()
    {
        if (this->emu_ && this->address_ && this->size_)
        {
            this->emu_->release_memory(this->address_, this->size_);
            this->address_ = 0;
            this->size_ = 0;
        }
    }

  private:
    emulator* emu_{};
    uint64_t address_{};
    uint64_t size_{};
    uint64_t active_address_{0};
};

inline std::u16string read_unicode_string(const emulator& emu, const UNICODE_STRING<EmulatorTraits<Emu64>> ucs)
{
    static_assert(offsetof(UNICODE_STRING<EmulatorTraits<Emu64>>, Length) == 0);
    static_assert(offsetof(UNICODE_STRING<EmulatorTraits<Emu64>>, MaximumLength) == 2);
    static_assert(offsetof(UNICODE_STRING<EmulatorTraits<Emu64>>, Buffer) == 8);
    static_assert(sizeof(UNICODE_STRING<EmulatorTraits<Emu64>>) == 16);

    std::u16string result{};
    result.resize(ucs.Length / 2);

    emu.read_memory(ucs.Buffer, result.data(), ucs.Length);

    return result;
}

inline std::u16string read_unicode_string(const emulator& emu,
                                          const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> uc_string)
{
    const auto ucs = uc_string.read();
    return read_unicode_string(emu, ucs);
}

inline std::u16string read_unicode_string(emulator& emu, const UNICODE_STRING<EmulatorTraits<Emu64>>* uc_string)
{
    return read_unicode_string(emu, emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>>{emu, uc_string});
}
